#include "ResamplingFifo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vc
{

namespace
{
constexpr double kSmoothingTauSeconds = 2.0;
constexpr double kMaxSpeedCorrection = 1.0e-3; // ±0.1%
constexpr double kFadeMs = 2.0;
constexpr double kInitialJitterMarginMs = 2.0;
constexpr double kMaxJitterMarginMs = 20.0;
constexpr double kJitterStepMs = 2.0;
constexpr double kOverrunMultiple = 3.0;
} // namespace

void ResamplingFifo::prepare (double newInRate, double newOutRate, int maxInBlock, int maxOutBlock, int inBlock, int outBlock)
{
    jassert (newInRate > 0.0 && newOutRate > 0.0 && maxInBlock > 0 && maxOutBlock > 0 && inBlock > 0 && outBlock > 0);

    inRate = newInRate;
    outRate = newOutRate;
    nominalRatio = inRate / outRate;
    speedRatio = nominalRatio;
    inBlockSamples = inBlock;
    outBlockSamples = outBlock;

    // リングバッファは入力レートで1秒分。AbstractFifoの実容量はbufferSize-1なので+1しておく。
    const int capacity = (int) std::ceil (inRate) + 1;
    fifo.setTotalSize (capacity);
    fifo.reset();
    buffer.assign ((size_t) capacity, 0.0f);

    // 線形化用スクラッチ（docs/plan.md 2.5節）: ceil(maxOutBlock*公称比*1.001)+8。
    const int scratchSize = (int) std::ceil ((double) maxOutBlock * nominalRatio * 1.001) + 8;
    scratch.assign ((size_t) scratchSize, 0.0f);

    interp.reset();

    jitterMarginMs = kInitialJitterMarginMs;
    recomputeTarget();

    fillSmoothed = 0.0;
    fifoStats.fillSmoothedSamples.store (0.0f, std::memory_order_relaxed);
    fifoStats.speedCorrectionPpm.store (0.0f, std::memory_order_relaxed);
    fifoStats.jitterMarginMs.store ((float) jitterMarginMs, std::memory_order_relaxed);
    fifoStats.underruns.store (0, std::memory_order_relaxed);
    fifoStats.overruns.store (0, std::memory_order_relaxed);
    fifoStats.discards.store (0, std::memory_order_relaxed);

    fadeLenSamplesOut = juce::jmax (1, (int) std::lround (kFadeMs / 1000.0 * outRate));

    phase = Phase::Refilling;
    phaseCount = 0;
    gain = 0.0;

    lastPushBlockLen.store (0, std::memory_order_relaxed);
    lastPushTime.store (0.0, std::memory_order_relaxed);

    (void) maxInBlock; // 現状はバッファ容量(1秒)が常に上回るため未使用。将来capacityの下限チェックに使う余地を残す。
}

// D-012: 生の充填量を「入力が連続的に到着した」とみなす連続換算値へ直す。
// 直前のpushからnowSecondsまでの経過時間ぶん、直前のブロックが徐々に到着したとみなして按分する。
double ResamplingFifo::continuousFill (int rawFilled, double nowSeconds) const noexcept
{
    const double t = lastPushTime.load (std::memory_order_acquire);
    const int biLast = lastPushBlockLen.load (std::memory_order_relaxed);

    if (biLast <= 0)
        return (double) rawFilled;

    const double elapsed = std::max (0.0, nowSeconds - t);
    const double credit = juce::jlimit (0.0, (double) biLast, inRate * elapsed);
    const double fillC = (double) rawFilled - (double) biLast + credit;

    return std::max (0.0, fillC);
}

void ResamplingFifo::recomputeTarget() noexcept
{
    const double inBlockMs = (double) inBlockSamples / inRate * 1000.0;
    const double outBlockMs = (double) outBlockSamples / outRate * 1000.0;
    const double targetMs = std::max (inBlockMs, outBlockMs) + jitterMarginMs;
    targetSamples = targetMs / 1000.0 * inRate;
    fifoStats.targetSamples.store ((float) targetSamples, std::memory_order_relaxed);
    fifoStats.jitterMarginMs.store ((float) jitterMarginMs, std::memory_order_relaxed);
}

int ResamplingFifo::neededInput (int outSamples) const noexcept
{
    return (int) std::ceil (speedRatio * (double) outSamples) + 2;
}

void ResamplingFifo::updateFillStats (double fillCSamples, int outputSamplesElapsed) noexcept
{
    const double dt = outputSamplesElapsed > 0 ? (double) outputSamplesElapsed / outRate : 0.0;
    const double alpha = dt > 0.0 ? std::exp (-dt / kSmoothingTauSeconds) : 1.0;
    fillSmoothed = alpha * fillSmoothed + (1.0 - alpha) * fillCSamples;
    fifoStats.fillSmoothedSamples.store ((float) fillSmoothed, std::memory_order_relaxed);
}

void ResamplingFifo::updateSpeedRatio() noexcept
{
    // 比例制御のみ。
    // ponytail: 定常状態では誤差が0にならないと補正も0になり、補正がなければ誤差は増え続けるため、
    // 実際には「誤差に比例した補正が入力/出力のクロックずれをちょうど打ち消す」点で釣り合う定常偏差が残る。
    // 100ppmのクロックずれに対しては目標充填量の約10%（誤差係数1e-3の逆数）がその定常偏差になる。
    // 積分項を足せば偏差は消せるが、要件（±10ppm以内の速度比補正・有界な充填量）は比例制御のみで満たせるため追加しない。
    const double error = targetSamples > 0.0 ? (fillSmoothed - targetSamples) / targetSamples : 0.0;
    const double correction = std::clamp (1.0e-3 * error, -kMaxSpeedCorrection, kMaxSpeedCorrection);
    speedRatio = nominalRatio * (1.0 + correction);
    fifoStats.speedCorrectionPpm.store ((float) (correction * 1.0e6), std::memory_order_relaxed);
}

void ResamplingFifo::processNormal (float* out, int chunk) noexcept
{
    updateSpeedRatio();

    const int needed = neededInput (chunk);
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (needed, start1, size1, start2, size2);

    if (size1 > 0)
        std::memcpy (scratch.data(), buffer.data() + start1, sizeof (float) * (size_t) size1);
    if (size2 > 0)
        std::memcpy (scratch.data() + size1, buffer.data() + start2, sizeof (float) * (size_t) size2);

    const int available = size1 + size2;
    const int used = interp.process (speedRatio, scratch.data(), out, chunk, available, 0);

    fifo.finishedRead (used);
}

void ResamplingFifo::applyRamp (float* out, int chunk, bool ascending) noexcept
{
    for (int i = 0; i < chunk; ++i)
    {
        out[i] *= (float) gain;
        gain += ascending ? (1.0 / (double) fadeLenSamplesOut) : -(1.0 / (double) fadeLenSamplesOut);
        gain = juce::jlimit (0.0, 1.0, gain);
    }
}

void ResamplingFifo::handleUnderrun (float* out, int offset, int remaining, int filled, double nowSeconds) noexcept
{
    int availableOut = 0;

    if (filled > 2)
        availableOut = juce::jmin (remaining, (int) std::floor ((double) (filled - 2) / speedRatio));

    if (availableOut > 0)
    {
        processNormal (out + offset, availableOut);

        const int fadeLen = juce::jmin (fadeLenSamplesOut, availableOut);

        for (int i = 0; i < fadeLen; ++i)
        {
            const double g = 1.0 - (double) (i + 1) / (double) fadeLen;
            out[offset + availableOut - fadeLen + i] *= (float) g;
        }
    }

    if (availableOut < remaining)
        juce::FloatVectorOperations::clear (out + offset + availableOut, remaining - availableOut);

    updateFillStats (continuousFill (fifo.getNumReady(), nowSeconds), remaining);

    fifoStats.underruns.fetch_add (1, std::memory_order_relaxed);
    jitterMarginMs = std::min (kMaxJitterMarginMs, jitterMarginMs + kJitterStepMs);
    recomputeTarget();
    interp.reset();

    phase = Phase::Refilling;
    phaseCount = 0;
    gain = 0.0;
}

void ResamplingFifo::pull (float* out, int n, double nowSeconds) noexcept
{
    int produced = 0;

    while (produced < n)
    {
        const int remaining = n - produced;

        if (phase == Phase::Refilling)
        {
            const int filled = fifo.getNumReady();
            const double fillC = continuousFill (filled, nowSeconds);
            const int needed = neededInput (remaining);

            if (fillC >= targetSamples + (double) needed)
            {
                phase = Phase::FadeIn;
                phaseCount = fadeLenSamplesOut;
                gain = 0.0;
                continue;
            }

            juce::FloatVectorOperations::clear (out + produced, remaining);
            updateFillStats (fillC, remaining);
            produced = n;
            break;
        }

        if (phase == Phase::Idle)
        {
            const int filled = fifo.getNumReady();
            const double fillC = continuousFill (filled, nowSeconds);

            if (fillC > kOverrunMultiple * targetSamples)
            {
                phase = Phase::OverrunFadeOut;
                phaseCount = fadeLenSamplesOut;
                gain = 1.0;
                fifoStats.discards.fetch_add (1, std::memory_order_relaxed);
            }
            else
            {
                const int needed = neededInput (remaining);

                // アンダーラン判定は生の充填量のまま行う(D-012)。
                if (filled < needed)
                {
                    handleUnderrun (out, produced, remaining, filled, nowSeconds);
                    produced = n;
                    break;
                }
            }
        }

        if (phase == Phase::Idle)
        {
            processNormal (out + produced, remaining);
            produced += remaining;
            updateFillStats (continuousFill (fifo.getNumReady(), nowSeconds), remaining);
            continue;
        }

        // フェード区間（FadeIn / OverrunFadeOut / OverrunFadeIn）。複数回のpull()呼び出しにまたがり得る。
        const int chunk = juce::jmin (remaining, phaseCount);
        processNormal (out + produced, chunk);

        const bool ascending = phase != Phase::OverrunFadeOut;
        applyRamp (out + produced, chunk, ascending);

        phaseCount -= chunk;
        produced += chunk;
        updateFillStats (continuousFill (fifo.getNumReady(), nowSeconds), chunk);

        if (phaseCount == 0)
        {
            if (phase == Phase::FadeIn)
            {
                phase = Phase::Idle;
            }
            else if (phase == Phase::OverrunFadeOut)
            {
                const int filledNow = fifo.getNumReady();
                const int toDrop = filledNow - (int) targetSamples;

                if (toDrop > 0)
                {
                    int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
                    fifo.prepareToRead (toDrop, s1, b1, s2, b2);
                    fifo.finishedRead (b1 + b2);
                }

                phase = Phase::OverrunFadeIn;
                phaseCount = fadeLenSamplesOut;
                gain = 0.0;
            }
            else if (phase == Phase::OverrunFadeIn)
            {
                phase = Phase::Idle;
            }
        }
    }
}

void ResamplingFifo::push (const float* mono, int n, double nowSeconds) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (n, start1, size1, start2, size2);

    if (size1 > 0)
        std::memcpy (buffer.data() + start1, mono, sizeof (float) * (size_t) size1);
    if (size2 > 0)
        std::memcpy (buffer.data() + start2, mono + size1, sizeof (float) * (size_t) size2);

    fifo.finishedWrite (size1 + size2);

    if (size1 + size2 < n)
        fifoStats.overruns.fetch_add (1, std::memory_order_relaxed);

    // D-012: 連続換算のため直前のブロック長と時刻を記録する。blockLenを先に書き、timeを
    // release書きすることで、読み手がtimeの新しい値をacquireで見た時点でblockLenも新しい
    // 値が見える（対で読める）。nは実際に書けた量ではなく要求量(=物理ブロック長)を使う。
    lastPushBlockLen.store (n, std::memory_order_relaxed);
    lastPushTime.store (nowSeconds, std::memory_order_release);
}

double ResamplingFifo::getLatencyMs() const noexcept
{
    const double fillMs = (double) fifoStats.fillSmoothedSamples.load (std::memory_order_relaxed) / inRate * 1000.0;
    const double inBlockMs = (double) inBlockSamples / inRate * 1000.0;
    const double outBlockMs = (double) outBlockSamples / outRate * 1000.0;
    return fillMs + 0.5 * (inBlockMs + outBlockMs);
}

} // namespace vc
