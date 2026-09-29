#include "MicProcessing.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vc
{

namespace
{
constexpr double kNativeRate = 48000.0;
constexpr int kFrame = 480;            // RNNoiseの1フレーム（48kHzで10ms）
constexpr int kRnnoiseDelay = 960;     // RNNoise固有の遅延（サンプル@48kHz）。T-009のN0cの実測値
constexpr int kInterpLatency = 2;      // juce::LagrangeInterpolatorのアルゴリズム遅延（入力側のサンプル数）
constexpr int kPrefillMargin = 4;      // 48kHz以外の出力FIFOの初期充填の余裕（枯渇しない上限に対する余り）
constexpr double kFadeSeconds = 0.020;
constexpr double kMixRampSeconds = 0.050;
constexpr float kInt16Scale = 32768.0f;
constexpr float kInt16ScaleInv = 1.0f / 32768.0f;

bool allFinite (const float* data, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (data[i]))
            return false;

    return true;
}
} // namespace

void NoiseReducer::RnnoiseDeleter::operator() (DenoiseState* st) const noexcept
{
    rnnoise_destroy (st);
}

void NoiseReducer::prepare (double newSampleRate, int newMaxBlockSamples)
{
    jassert (newSampleRate > 0.0 && newMaxBlockSamples > 0);

    sampleRate = newSampleRate;
    maxBlockSamples = newMaxBlockSamples;

    if (rnnoise == nullptr)
        rnnoise.reset (rnnoise_create (nullptr)); // 失敗時はnullptrのまま。setTarget()が稼働要求を無視する

    native = std::abs (sampleRate - kNativeRate) < 0.5;
    const double ratio = sampleRate / kNativeRate;
    upRatio = native ? 1.0 : ratio;
    downRatio = native ? 1.0 : 1.0 / ratio;

    // 1フレームを作るのに要る入力の上限（上げ側は480個の出力で最大 ceil(480*ratio) 個進む。余裕2）。
    upNeed = native ? kFrame : (int) std::ceil (kFrame * ratio) + 2;
    // 下げ側の1個の出力で進む入力の上限（補間位置は 1 + downRatio 未満。余裕1）。
    downNeed = (int) std::ceil (downRatio) + 1;

    if (native)
    {
        prefill = kFrame;
        delaySamples = prefill + kRnnoiseDelay;
    }
    else
    {
        prefill = (int) std::ceil (kFrame * ratio) + kPrefillMargin;
        delaySamples = prefill + (int) std::lround (kRnnoiseDelay * ratio + kInterpLatency + kInterpLatency * ratio);
    }

    const int downOutCapacity = (int) std::ceil ((kFrame + downNeed) / downRatio) + 4;

    inQueue.allocate (upNeed + maxBlockSamples + 8);
    outQueue.allocate (prefill + maxBlockSamples + downOutCapacity + 8);
    dryLine.assign ((size_t) delaySamples, 0.0f);
    wetScratch.assign ((size_t) maxBlockSamples, 0.0f);
    frameIn.assign ((size_t) kFrame, 0.0f);
    frameOut.assign ((size_t) kFrame, 0.0f);
    downStage.assign ((size_t) (kFrame + downNeed + 8), 0.0f);
    downOut.assign ((size_t) downOutCapacity, 0.0f);

    fadeLenSamples = std::max (1, (int) std::lround (kFadeSeconds * sampleRate));
    dSmoothed.reset (sampleRate, kMixRampSeconds);
    dSmoothed.setCurrentAndTargetValue (0.0f);

    underflowCount = 0;
    reset();
}

void NoiseReducer::setTarget (bool run, float background, float impact) noexcept
{
    runTarget = run && rnnoise != nullptr;
    impactTarget = impact;

    // 原音の混合比 d = (1 - 2s)^2（s <= 0.5）。s > 0.5は完全なRNNoise出力（d = 0。ゲートはT-011）。
    const float s = juce::jlimit (0.0f, 1.0f, background);
    const float d = s <= 0.5f ? (1.0f - 2.0f * s) * (1.0f - 2.0f * s) : 0.0f;

    if (state == State::Resting)
        dSmoothed.setCurrentAndTargetValue (d); // 休止中の値の変化は補間しない（起動時に古い値から動かさない）
    else
        dSmoothed.setTargetValue (d);
}

void NoiseReducer::clearPipeline() noexcept
{
    if (rnnoise != nullptr)
        rnnoise_init (rnnoise.get(), nullptr); // 確保しない（T-009のN0d）

    inQueue.clear();
    outQueue.clear();
    outQueue.push (nullptr, prefill);
    std::fill (dryLine.begin(), dryLine.end(), 0.0f);
    dryPos = 0;
    upInterp.reset();
    downInterp.reset();
    downStaged = 0;
}

void NoiseReducer::reset() noexcept
{
    clearPipeline();
    state = State::Resting;
    gain = 0.0;
    primedSamples = 0;
    updateLatencyForUi();
}

void NoiseReducer::updateLatencyForUi() noexcept
{
    const int latency = (state == State::FadingIn || state == State::Active || state == State::FadingOut) ? delaySamples : 0;
    latencySamplesForUi.store (latency, std::memory_order_relaxed);
}

// 入力をFIFOへ入れ、フレームがそろうごとにRNNoiseを通して出力FIFOへ足す。
void NoiseReducer::feed (const float* in, int n) noexcept
{
    inQueue.push (in, n);

    if (native)
    {
        while (inQueue.size() >= kFrame)
        {
            const float* src = inQueue.readPtr();

            for (int i = 0; i < kFrame; ++i)
                frameIn[(size_t) i] = src[i] * kInt16Scale;

            inQueue.drop (kFrame);
            processFrame();
            outQueue.push (frameOut.data(), kFrame);
        }

        return;
    }

    while (inQueue.size() >= upNeed)
    {
        const int used = upInterp.process (upRatio, inQueue.readPtr(), frameIn.data(), kFrame);
        inQueue.drop (used);

        for (int i = 0; i < kFrame; ++i)
            frameIn[(size_t) i] *= kInt16Scale;

        processFrame();

        std::memcpy (downStage.data() + downStaged, frameOut.data(), sizeof (float) * (size_t) kFrame);
        downStaged += kFrame;

        // 1個ずつ出し、進む入力が足りなくなる手前で止める。端数は次のフレームへ持ち越す。
        int consumed = 0;
        int produced = 0;

        while (downStaged - consumed >= downNeed)
        {
            consumed += downInterp.process (downRatio, downStage.data() + consumed, downOut.data() + produced, 1);
            ++produced;
        }

        downStaged -= consumed;
        std::memmove (downStage.data(), downStage.data() + consumed, sizeof (float) * (size_t) downStaged);
        outQueue.push (downOut.data(), produced);
    }
}

// frameInの480サンプル（int16の値域）をRNNoiseへ通し、frameOutへ書く（-1〜1へ戻す）。
void NoiseReducer::processFrame() noexcept
{
    rnnoise_process_frame (rnnoise.get(), frameOut.data(), frameIn.data());

    for (int i = 0; i < kFrame; ++i)
        frameOut[(size_t) i] *= kInt16ScaleInv;
}

// 出力FIFOからn個取り出してwetScratchへ書く（足りなければ無音で埋め、underflowCountを数える）。
void NoiseReducer::pullMixed (int n) noexcept
{
    const int take = std::min (outQueue.size(), n);

    if (take < n)
        ++underflowCount;

    std::memcpy (wetScratch.data(), outQueue.readPtr(), sizeof (float) * (size_t) take);
    std::fill (wetScratch.begin() + take, wetScratch.begin() + n, 0.0f);
    outQueue.drop (take);
}

bool NoiseReducer::process (float* buf, int n) noexcept
{
    jassert (n <= maxBlockSamples);

    // ----- 状態遷移（ブロックの先頭で、稼働要求に応じて確定させる） -----
    switch (state)
    {
        case State::Resting:
            if (runTarget)
            {
                clearPipeline();
                primedSamples = 0;
                state = State::Priming;
            }
            break;

        case State::Priming:
            if (! runTarget)
                state = State::Resting;
            break;

        case State::FadingIn:
        case State::Active:
            if (! runTarget)
                state = State::FadingOut;
            break;

        case State::FadingOut:
            if (runTarget)
                state = State::FadingIn;
            break;
    }

    if (state == State::Resting)
    {
        updateLatencyForUi();
        return true;
    }

    if (! allFinite (buf, n))
        return false;

    // ----- パイプライン（Priming以降は常に進める） -----
    feed (buf, n);
    pullMixed (n);

    // 原音を遅延線へ通し、混合する: 出力 = (1 - d) * RNNoise + d * 原音（Dサンプル遅れ）。
    for (int i = 0; i < n; ++i)
    {
        const float d = dSmoothed.getNextValue();
        const float delayedDry = dryLine[(size_t) dryPos];
        dryLine[(size_t) dryPos] = buf[i];

        if (++dryPos >= delaySamples)
            dryPos = 0;

        wetScratch[(size_t) i] = wetScratch[(size_t) i] * (1.0f - d) + delayedDry * d;
    }

    // ----- 出力（状態の切り替わりはサンプル単位。ブロック長に依存しない） -----
    int pos = 0;

    while (pos < n)
    {
        switch (state)
        {
            case State::Resting: // FadingOutの完了後。残りは入力のまま
                pos = n;
                break;

            case State::Priming:
            {
                const int seg = std::min (n - pos, delaySamples - primedSamples);
                primedSamples += seg;
                pos += seg;

                if (primedSamples >= delaySamples)
                    state = State::FadingIn;

                break;
            }

            case State::Active:
                std::memcpy (buf + pos, wetScratch.data() + pos, sizeof (float) * (size_t) (n - pos));
                pos = n;
                break;

            case State::FadingIn:
            case State::FadingOut:
            {
                const bool ascending = (state == State::FadingIn);
                const double step = 1.0 / (double) fadeLenSamples;

                while (pos < n)
                {
                    buf[pos] = (float) ((double) buf[pos] * (1.0 - gain) + (double) wetScratch[(size_t) pos] * gain);
                    ++pos;
                    gain += ascending ? step : -step;
                    gain = juce::jlimit (0.0, 1.0, gain);

                    if (ascending && gain >= 1.0)
                    {
                        state = State::Active;
                        break;
                    }

                    if (! ascending && gain <= 0.0)
                    {
                        state = State::Resting;
                        break;
                    }
                }

                break;
            }
        }
    }

    updateLatencyForUi();
    return true;
}

} // namespace vc
