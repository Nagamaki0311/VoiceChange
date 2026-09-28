#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/Engine.h"
#include "core/Params.h"
#include "core/ResamplingFifo.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// ===== SECTION: LongRunTests =====
// docs/plan.md 3章「T-008」。ResamplingFifo（44.1kHz→48kHz、入力+100ppm、ジッタ0〜3ms）→ Engineを
// 音声時間3600秒ぶんオフラインで回す（カテゴリLongRun、long）。模擬ドライバはRingBufferTests.cppと同じ離散イベント方式。
// 実機のWASAPI・デバイス遅延・OSスケジューリングは含まない（READMEの実機手順で確認する）。

namespace
{

constexpr double kInRate = 44100.0;
constexpr double kOutRate = 48000.0;
constexpr int kBi = 441;  // 10ms
constexpr int kBo = 480;  // 10ms
constexpr double kPpmIn = 100.0;
constexpr double kDurationSeconds = 3600.0;

// 1本のストリーム（入力または出力）の次イベント時刻。ジッタは固定シードの一様乱数で、時刻が逆転しないようclampする。
class EventClock
{
public:
    EventClock (double periodSecondsIn, juce::int64 seed, double jitterMaxSecondsIn)
        : periodSeconds (periodSecondsIn), jitterMaxSeconds (jitterMaxSecondsIn), rng (seed) {}

    double pop() noexcept
    {
        double t = nextTime + rng.nextDouble() * jitterMaxSeconds;
        t = std::max (t, lastEmitted);
        lastEmitted = t;
        nextTime += periodSeconds;
        return t;
    }

private:
    double periodSeconds, jitterMaxSeconds;
    double nextTime = 0.0, lastEmitted = -1.0;
    juce::Random rng;
};

// 基本周波数が滑る母音（のこぎり波を3つの2極共振器に通す）。
class GlidingVowel
{
public:
    explicit GlidingVowel (double sampleRateIn) : sampleRate (sampleRateIn)
    {
        const std::array<double, 3> freq { 730.0, 1090.0, 2440.0 };
        const std::array<double, 3> bw { 80.0, 90.0, 120.0 };

        for (size_t i = 0; i < 3; ++i)
        {
            const double r = std::exp (-juce::MathConstants<double>::pi * bw[i] / sampleRate);
            a1[i] = 2.0 * r * std::cos (juce::MathConstants<double>::twoPi * freq[i] / sampleRate);
            a2[i] = -r * r;
            gain[i] = 1.0 - r;
        }
    }

    float next (double f0Hz) noexcept
    {
        phase += f0Hz / sampleRate;
        phase -= std::floor (phase);
        double x = 2.0 * phase - 1.0;

        for (size_t i = 0; i < 3; ++i)
        {
            const double y = gain[i] * x + a1[i] * y1[i] + a2[i] * y2[i];
            y2[i] = y1[i];
            y1[i] = y;
            x = y;
        }

        return (float) x;
    }

private:
    double sampleRate, phase = 0.0;
    std::array<double, 3> a1 {}, a2 {}, gain {}, y1 {}, y2 {};
};

// 雑音バーストのピーク（振幅に対する比）。バイパス時はEngineが入力をそのまま出すため、0dBFS一杯の広帯域雑音だと
// ResamplingFifoのLagrange補間のオーバーシュートで出力が1.0を超える（実測1.36倍）。実際のマイク雑音は満振れの
// 一様白色雑音ではないので、補間後も1.0以内に収まる-6dBFSに抑える。
constexpr float kNoisePeak = 0.5f;

// 6秒周期: 母音3秒（f0 100→220Hz）→ 雑音1.5秒 → デジタル無音1.5秒。振幅は周期ごとに-40〜-1dBFS（補間のオーバーシュート分を見込み満振れは避ける）を巡回する。
class MixedInput
{
public:
    MixedInput() : vowel (kInRate)
    {
        // 母音のピークを1.0へ正規化する係数を、同じ生成器でf0の滑り全体（3秒）を回して求める。
        // 入力が±1.0を超えるとバイパス時（入力そのまま出力）にピーク検査が入力側の理由で失敗する。
        GlidingVowel calibration (kInRate);
        float peak = 0.0f;
        for (int i = 0; i < (int) (3.0 * kInRate); ++i)
            peak = std::max (peak, std::abs (calibration.next (100.0 + 120.0 * (double) i / (3.0 * kInRate))));
        vowelScale = peak > 0.0f ? 1.0f / peak : 1.0f;
    }

    void generate (float* out, int n) noexcept
    {
        constexpr std::array<float, 6> levelsDb { -30.0f, -12.0f, -3.0f, -40.0f, -20.0f, -1.0f };

        for (int i = 0; i < n; ++i)
        {
            const double t = (double) sampleIndex / kInRate;
            const auto cycle = (std::int64_t) (t / 6.0);
            const double local = t - (double) cycle * 6.0;
            const float amp = (float) std::pow (10.0, (double) levelsDb[(size_t) (cycle % 6)] / 20.0);

            if (local < 3.0)
                out[i] = amp * vowelScale * vowel.next (100.0 + 120.0 * local / 3.0);
            else if (local < 4.5)
                out[i] = kNoisePeak * amp * (rng.nextFloat() * 2.0f - 1.0f);
            else
                out[i] = 0.0f;

            ++sampleIndex;
        }
    }

private:
    GlidingVowel vowel;
    float vowelScale = 1.0f;
    juce::Random rng { 4242 };
    std::int64_t sampleIndex = 0;
};

} // namespace

class LongRunTests final : public juce::UnitTest
{
public:
    LongRunTests() : juce::UnitTest ("LongRun", "LongRun") {}

    void runTest() override
    {
        beginTest ("LongRun: 44.1k→48k +100ppm ジッタ0-3ms、Engine通し、音声時間3600秒");

        vc::ResamplingFifo fifo;
        fifo.prepare (kInRate, kOutRate, 4096, 4096, kBi, kBo);

        vc::Engine engine;
        engine.prepare ({ kOutRate, 1024 });
        engine.params().gainDb.store (6.0f);
        engine.params().reverb.store (0.2f);

        const double inPeriod = (double) kBi / (kInRate * (1.0 + kPpmIn * 1.0e-6));
        const double outPeriod = (double) kBo / kOutRate;
        EventClock inClock (inPeriod, 1001, 0.003);
        EventClock outClock (outPeriod, 2002, 0.003);

        MixedInput source;
        std::vector<float> inBuf ((size_t) kBi), outBuf ((size_t) kBo);
        juce::Random pitchRng (77);

        double pendingIn = inClock.pop();
        double pendingOut = outClock.pop();

        std::size_t allocations = 0;
        std::uint64_t nonFiniteBlocks = 0, overPeakBlocks = 0;
        double maxAbs = 0.0, outSumSquares = 0.0;
        std::uint64_t outSamples = 0;

        // 制御の切替は出力イベントの模擬時刻で行う。
        int lastPresetSlot = -1, lastPitchSlot = -1, lastBypassSlot = 0;
        int presetChanges = 0, pitchChanges = 0, bypassChanges = 0;

        // c/d/e用（RingBufferTests.cppのS7と同じ適用区間）。
        constexpr double kCFrom = 300.0, kDStart = 300.0, kEFrom = 600.0;
        std::uint64_t cViolations = 0;
        double sumFillD = 0.0, sumFillLast10 = 0.0, sumPpm = 0.0;
        std::int64_t countD = 0, countLast10 = 0, countPpm = 0;
        double latencyMin = 1.0e9, latencyMax = -1.0e9;

        // CPU参考値（Engine::processの実時間 ÷ 音声時間）。プリセット別にも集計する。
        std::array<double, 8> cpuSecondsByPreset {}, audioSecondsByPreset {};
        double cpuSeconds = 0.0;
        const auto wallStart = juce::Time::getHighResolutionTicks();

        while (true)
        {
            const double now = std::min (pendingIn, pendingOut);

            if (now >= kDurationSeconds)
                break;

            if (pendingIn <= pendingOut)
            {
                source.generate (inBuf.data(), kBi);

                std::size_t count = 0;
                {
                    vc::test::ScopedAllocationGuard guard;
                    fifo.push (inBuf.data(), kBi, pendingIn);
                    count = guard.count();
                }
                allocations += count;

                pendingIn = inClock.pop();
                continue;
            }

            const int presetSlot = (int) (pendingOut / 10.0);
            if (presetSlot != lastPresetSlot)
            {
                engine.params().preset.store (presetSlot % (int) vc::kPresets.size());
                lastPresetSlot = presetSlot;
                ++presetChanges;
            }

            const int pitchSlot = (int) (pendingOut / 7.0);
            if (pitchSlot != lastPitchSlot)
            {
                engine.params().pitch.store (pitchSlot == 0 ? 0 : pitchRng.nextInt (25) - 12);
                lastPitchSlot = pitchSlot;
                ++pitchChanges;
            }

            const int bypassSlot = (int) (pendingOut / 60.0);
            if (bypassSlot != lastBypassSlot)
            {
                engine.params().enabled.store (bypassSlot % 2 == 0);
                lastBypassSlot = bypassSlot;
                ++bypassChanges;
            }

            std::size_t count = 0;
            {
                vc::test::ScopedAllocationGuard guard;
                fifo.pull (outBuf.data(), kBo, pendingOut);
                count = guard.count();
            }
            allocations += count;

            const auto t0 = juce::Time::getHighResolutionTicks();
            {
                vc::test::ScopedAllocationGuard guard;
                engine.process (outBuf.data(), kBo);
                count = guard.count();
            }
            const double elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            allocations += count;

            cpuSeconds += elapsed;
            const auto presetIdx = (size_t) (presetSlot % (int) vc::kPresets.size());
            cpuSecondsByPreset[presetIdx] += elapsed;
            audioSecondsByPreset[presetIdx] += outPeriod;

            if (! vc::test::allFinite (outBuf.data(), kBo))
                ++nonFiniteBlocks;

            const double peak = vc::test::peakAbs (outBuf.data(), kBo);
            maxAbs = std::max (maxAbs, peak);
            if (peak > 1.0)
                ++overPeakBlocks;

            for (const float v : outBuf)
                outSumSquares += (double) v * (double) v;
            outSamples += (std::uint64_t) kBo;

            const double fillSmoothed = fifo.stats().fillSmoothedSamples.load (std::memory_order_relaxed);
            const double target = fifo.stats().targetSamples.load (std::memory_order_relaxed);
            const double ppmVal = fifo.stats().speedCorrectionPpm.load (std::memory_order_relaxed);

            if (pendingOut >= kCFrom && target > 0.0 && std::abs (fillSmoothed - target) > 0.25 * target)
                ++cViolations;

            if (pendingOut >= kDStart && pendingOut < kDStart + 600.0)
            {
                sumFillD += fillSmoothed;
                ++countD;
            }

            if (pendingOut >= kDurationSeconds - 600.0)
            {
                sumFillLast10 += fillSmoothed;
                ++countLast10;
            }

            if (pendingOut >= kEFrom)
            {
                sumPpm += ppmVal;
                ++countPpm;
            }

            if (pendingOut >= kCFrom)
            {
                const double latency = fifo.getLatencyMs();
                latencyMin = std::min (latencyMin, latency);
                latencyMax = std::max (latencyMax, latency);
            }

            pendingOut = outClock.pop();
        }

        const double wallSeconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - wallStart);

        const auto& stats = fifo.stats();
        const auto underruns = stats.underruns.load (std::memory_order_relaxed);
        const auto overruns = stats.overruns.load (std::memory_order_relaxed);
        const auto discards = stats.discards.load (std::memory_order_relaxed);
        const double avgFillD = countD > 0 ? sumFillD / (double) countD : 0.0;
        const double avgFillLast10 = countLast10 > 0 ? sumFillLast10 / (double) countLast10 : 0.0;
        const double avgPpm = countPpm > 0 ? sumPpm / (double) countPpm : 0.0;
        const double expectedPpm = kPpmIn;
        const double diffMs = (avgFillLast10 - avgFillD) / kInRate * 1000.0;
        const double outRms = std::sqrt (outSumSquares / (double) std::max<std::uint64_t> (outSamples, 1));

        logMessage (juce::String ("LongRun: underruns=") + juce::String ((int) underruns)
                    + ", overruns=" + juce::String ((int) overruns)
                    + ", discards=" + juce::String ((int) discards)
                    + ", jitterMarginMs=" + juce::String (stats.jitterMarginMs.load(), 2)
                    + ", avgFill 300-900s=" + juce::String (avgFillD, 1)
                    + ", avgFill last10min=" + juce::String (avgFillLast10, 1)
                    + " (diff " + juce::String (diffMs, 3) + "ms)"
                    + ", avgPpm=" + juce::String (avgPpm, 2) + " (expected " + juce::String (expectedPpm, 2) + ")"
                    + ", cViolations=" + juce::String ((int) cViolations));
        logMessage (juce::String ("LongRun: fifo latency (t>=300s) min=") + juce::String (latencyMin, 2)
                    + "ms, max=" + juce::String (latencyMax, 2) + "ms"
                    + ", peak=" + juce::String (maxAbs, 4) + ", outRms=" + juce::String (outRms, 4)
                    + ", errorFlags=" + juce::String ((int) engine.getErrorFlags())
                    + ", allocations=" + juce::String ((int) allocations)
                    + ", changes preset=" + juce::String (presetChanges) + " pitch=" + juce::String (pitchChanges)
                    + " bypass=" + juce::String (bypassChanges));

        juce::String cpuText ("LongRun: CPU (process time / audio time) total=" + juce::String (100.0 * cpuSeconds / kDurationSeconds, 2) + "%");
        for (size_t p = 0; p < 8; ++p)
            if (audioSecondsByPreset[p] > 0.0)
                cpuText << ", " << vc::kPresets[p].id << "=" << juce::String (100.0 * cpuSecondsByPreset[p] / audioSecondsByPreset[p], 2) << "%";
        logMessage (cpuText);
        logMessage (juce::String ("LongRun: wall time=") + juce::String (wallSeconds, 1) + "s");

        expectEquals ((int) nonFiniteBlocks, 0, "NaN/Infを含む出力ブロックがある");
        expectEquals ((int) overPeakBlocks, 0, "ピークが1.0を超えた");
        expectEquals ((int) underruns, 0, "初期充填後にunderrunが発生した");
        expectEquals ((int) overruns, 0, "overrunsが発生した");
        expectEquals ((int) discards, 0, "discardsが発生した");
        expectEquals ((int) engine.getErrorFlags(), 0, "エラーフラグが立った");
        expectEquals ((int) allocations, 0, "push/pull/processでアロケーションが発生した");
        expect (outRms > 1.0e-3, "出力がほぼ無音（信号経路が生きていない）");
        expect (presetChanges >= 360 && pitchChanges >= 514 && bypassChanges == 59, "制御の切替回数が想定と異なる");

        expectEquals ((int) cViolations, 0, "c) 平滑充填が目標±25%を外れた区間がある");
        expect (diffMs <= 0.5, "d) 平滑充填の後半平均が前半平均より0.5ms相当を超えて増加している");
        expectWithinAbsoluteError (avgPpm, expectedPpm, 10.0, "e) 速度比補正の平均が期待値の±10ppmを外れている");
    }
};

static LongRunTests longRunTests;
