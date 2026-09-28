#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/ResamplingFifo.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ===== SECTION: RingBufferTests =====
// docs/plan.md 3章「T-003」のR1〜R6（カテゴリRingBuffer）とS1〜S8（カテゴリRingBufferLong、1時間模擬）。
// 模擬ドライバは離散イベント方式（入出力それぞれの次イベント時刻を比較し、早い方から処理する）。

namespace
{

// 入力クロックに同期した連続位相の三角波（周期2秒、振幅0.5、docs/plan.md 3章のS1〜S7条件f）参照）。
class TriangleWave
{
public:
    explicit TriangleWave (double sampleRateIn, double freqHzIn = 0.5, float ampIn = 0.5f)
        : sampleRate (sampleRateIn), freqHz (freqHzIn), amp (ampIn) {}

    void generate (float* out, int n) noexcept
    {
        const double period = 1.0 / freqHz;

        for (int i = 0; i < n; ++i)
        {
            const double t = std::fmod (phase, period);
            const double frac = t / period;
            double value;

            if (frac < 0.25)       value = frac * 4.0;
            else if (frac < 0.75)  value = 2.0 - frac * 4.0;
            else                   value = frac * 4.0 - 4.0;

            out[i] = (float) (amp * value);
            phase += 1.0 / sampleRate;
        }
    }

private:
    double sampleRate, freqHz, phase = 0.0;
    float amp;
};

// 連続位相の正弦波（R5用）。
class SineWave
{
public:
    SineWave (double freqHzIn, double sampleRateIn) : freqHz (freqHzIn), sampleRate (sampleRateIn) {}

    void generate (float* out, int n, float amp) noexcept
    {
        const double inc = juce::MathConstants<double>::twoPi * freqHz / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            out[i] = (float) (amp * std::sin (phase));
            phase += inc;
        }
    }

private:
    double freqHz, sampleRate, phase = 0.0;
};

// 離散イベント方式の模擬ドライバ用: 1本のストリーム（入力または出力）の次イベント時刻を管理する。
// ジッタは固定シードの一様乱数。ストリーム内で時刻が逆転しないようclampする。
class EventClock
{
public:
    EventClock (double periodSecondsIn, juce::int64 seed) : periodSeconds (periodSecondsIn), rng (seed) {}

    double pop() noexcept
    {
        const double jitter = jitterMaxSeconds > 0.0 ? rng.nextDouble() * jitterMaxSeconds : 0.0;
        double t = nextTime + jitter;

        if (t < lastEmitted)
            t = lastEmitted;

        lastEmitted = t;
        nextTime += periodSeconds;
        return t;
    }

    double periodSeconds;
    double nextTime = 0.0;
    double lastEmitted = -1.0;
    double jitterMaxSeconds = 0.0;
    juce::Random rng;
};

// ===== S1〜S8共通の模擬シナリオ =====

struct ScenarioParams
{
    enum class JitterMode { None, Uniform0to3ms, Special30sDelay15ms };

    juce::String name;
    double inRate = 48000.0, outRate = 48000.0;
    double ppmIn = 0.0, ppmOut = 0.0;
    int bi = 480, bo = 480;
    JitterMode jitter = JitterMode::None;
    double durationSeconds = 3600.0;

    // 条件の適用開始時刻(秒)。S1〜S7は既定値、S8はb〜fすべてt≥10分（600秒）にする。
    double cQualifyFromSeconds = 300.0;
    double dWindowStartSeconds = 300.0; // d)の「5〜15分」窓の開始。窓幅は常に600秒。
    double eQualifyFromSeconds = 600.0;
    double fQualifyFromSeconds = 0.0;   // f)は既定で初期充填後すぐから。
    double bQualifyFromSeconds = 0.0;   // overruns/discardsを数え始める時刻。
};

struct ScenarioResult
{
    std::uint32_t totalUnderruns = 0;
    std::uint32_t underrunsAfter600 = 0;
    std::uint32_t overrunsFromQualify = 0;
    std::uint32_t discardsFromQualify = 0;
    std::uint64_t cViolations = 0;
    std::uint64_t secondDiffViolations = 0;
    double avgFillWindowD = 0.0;   // d)の「5〜15分」（またはS8では「10〜20分」）窓平均
    double avgFillLast10Min = 0.0;
    double avgSpeedCorrectionPpmFromQualify = 0.0;
    float jitterMarginMsFinal = 0.0f;
    double expectedSpeedCorrectionPpm = 0.0;
};

ScenarioResult runScenario (const ScenarioParams& p)
{
    ScenarioResult result;
    result.expectedSpeedCorrectionPpm = ((1.0 + p.ppmIn * 1.0e-6) / (1.0 + p.ppmOut * 1.0e-6) - 1.0) * 1.0e6;

    vc::ResamplingFifo fifo;
    const int maxIn = juce::jmax (p.bi * 4, 4096);
    const int maxOut = juce::jmax (p.bo * 4, 4096);
    fifo.prepare (p.inRate, p.outRate, maxIn, maxOut, p.bi, p.bo);

    const double inPeriod = (double) p.bi / (p.inRate * (1.0 + p.ppmIn * 1.0e-6));
    const double outPeriod = (double) p.bo / (p.outRate * (1.0 + p.ppmOut * 1.0e-6));

    EventClock inClock (inPeriod, 1001);
    EventClock outClock (outPeriod, 2002);

    if (p.jitter == ScenarioParams::JitterMode::Uniform0to3ms)
    {
        inClock.jitterMaxSeconds = 0.003;
        outClock.jitterMaxSeconds = 0.003;
    }

    TriangleWave gen (p.inRate);
    std::vector<float> inBuf ((size_t) p.bi);
    std::vector<float> outBuf ((size_t) p.bo);

    double pendingIn = inClock.pop();
    double pendingOut = outClock.pop();
    double nextSpecialMark = 30.0;

    // 2階差分チェック(f)用の状態。
    bool steadyStarted = false;
    int steadyGuardRemaining = 0;
    float prev1 = 0.0f, prev2 = 0.0f;
    bool havePrev = false;

    const double slopePerSecond = 0.5 * 4.0 / 2.0; // A=0.5, 周期2秒の三角波の傾き(units/s)
    const double outSlopePerSample = slopePerSecond / p.outRate;
    const double inSlopePerSample = slopePerSecond / p.inRate;
    const double diffThreshold = 0.25 * outSlopePerSample;
    const double exclusionBand = 0.5 - 4.0 * inSlopePerSample;
    const int fadeGuardSamples = juce::jmax (1, (int) std::lround (0.005 * p.outRate));

    double sumFillWindowD = 0.0; std::int64_t countWindowD = 0;
    double sumFillLast10 = 0.0; std::int64_t countLast10 = 0;
    double sumPpmFromQualify = 0.0; std::int64_t countPpmFromQualify = 0;

    const double windowDStart = p.dWindowStartSeconds;
    const double windowDEnd = p.dWindowStartSeconds + 600.0;
    const double last10Start = p.durationSeconds - 600.0;

    bool bSnapshotTaken = false;
    std::uint32_t overrunsAtQualify = 0, discardsAtQualify = 0;

    bool snapshot600Taken = false;
    std::uint32_t underrunsAt600 = 0;

    while (true)
    {
        const double now = std::min (pendingIn, pendingOut);

        if (now >= p.durationSeconds)
            break;

        if (! bSnapshotTaken && now >= p.bQualifyFromSeconds)
        {
            overrunsAtQualify = fifo.stats().overruns.load (std::memory_order_relaxed);
            discardsAtQualify = fifo.stats().discards.load (std::memory_order_relaxed);
            bSnapshotTaken = true;
        }

        if (! snapshot600Taken && now >= 600.0)
        {
            underrunsAt600 = fifo.stats().underruns.load (std::memory_order_relaxed);
            snapshot600Taken = true;
        }

        if (pendingIn <= pendingOut)
        {
            gen.generate (inBuf.data(), p.bi);
            fifo.push (inBuf.data(), p.bi, pendingIn);

            const bool crossedMark = p.jitter == ScenarioParams::JitterMode::Special30sDelay15ms
                                      && pendingIn >= nextSpecialMark;

            double t = inClock.pop();

            if (crossedMark)
            {
                t += 0.015;
                inClock.lastEmitted = t;
                nextSpecialMark += 30.0;
            }

            pendingIn = t;
        }
        else
        {
            fifo.pull (outBuf.data(), p.bo, pendingOut);

            for (int i = 0; i < p.bo; ++i)
            {
                const float x = outBuf[(size_t) i];

                if (! steadyStarted)
                {
                    if (x != 0.0f)
                    {
                        steadyStarted = true;
                        steadyGuardRemaining = fadeGuardSamples;
                    }
                }
                else if (steadyGuardRemaining > 0)
                {
                    --steadyGuardRemaining;
                }
                else if (havePrev && pendingOut >= p.fQualifyFromSeconds)
                {
                    if (std::abs ((double) x) <= exclusionBand)
                    {
                        const double d2 = (double) x - 2.0 * (double) prev1 + (double) prev2;

                        if (std::abs (d2) > diffThreshold)
                            ++result.secondDiffViolations;
                    }
                }

                prev2 = prev1;
                prev1 = x;
                havePrev = steadyStarted;
            }

            const double fillSmoothed = fifo.stats().fillSmoothedSamples.load (std::memory_order_relaxed);
            const double target = fifo.stats().targetSamples.load (std::memory_order_relaxed);
            const double ppmVal = fifo.stats().speedCorrectionPpm.load (std::memory_order_relaxed);

            if (pendingOut >= p.cQualifyFromSeconds)
                if (target > 0.0 && std::abs (fillSmoothed - target) > 0.25 * target)
                    ++result.cViolations;

            if (pendingOut >= windowDStart && pendingOut < windowDEnd)
            {
                sumFillWindowD += fillSmoothed;
                ++countWindowD;
            }

            if (pendingOut >= last10Start)
            {
                sumFillLast10 += fillSmoothed;
                ++countLast10;
            }

            if (pendingOut >= p.eQualifyFromSeconds)
            {
                sumPpmFromQualify += ppmVal;
                ++countPpmFromQualify;
            }

            pendingOut = outClock.pop();
        }
    }

    result.totalUnderruns = fifo.stats().underruns.load (std::memory_order_relaxed);
    result.underrunsAfter600 = result.totalUnderruns - underrunsAt600;
    result.overrunsFromQualify = fifo.stats().overruns.load (std::memory_order_relaxed) - overrunsAtQualify;
    result.discardsFromQualify = fifo.stats().discards.load (std::memory_order_relaxed) - discardsAtQualify;
    result.jitterMarginMsFinal = fifo.stats().jitterMarginMs.load (std::memory_order_relaxed);
    result.avgFillWindowD = countWindowD > 0 ? sumFillWindowD / (double) countWindowD : 0.0;
    result.avgFillLast10Min = countLast10 > 0 ? sumFillLast10 / (double) countLast10 : 0.0;
    result.avgSpeedCorrectionPpmFromQualify = countPpmFromQualify > 0 ? sumPpmFromQualify / (double) countPpmFromQualify : 0.0;

    return result;
}

} // namespace

// ===== SECTION: RingBuffer（R1〜R6） =====

class RingBufferTests final : public juce::UnitTest
{
public:
    RingBufferTests() : juce::UnitTest ("RingBuffer", "RingBuffer") {}

    void runTest() override
    {
        testR1();
        testR2();
        testR3();
        testR4();
        testR5();
        testR6();
    }

private:
    void testR1()
    {
        beginTest ("R1: 起動直後の再充填");

        vc::ResamplingFifo fifo;
        const double rate = 48000.0;
        const int block = 480;
        fifo.prepare (rate, rate, 4096, 4096, block, block);

        TriangleWave gen (rate);
        std::vector<float> inBuf ((size_t) block), outBuf ((size_t) block);
        double simTime = 0.0;
        const double halfPeriod = (double) block / rate * 0.5;

        bool sawNonZero = false;
        std::vector<float> envelope;

        for (int iter = 0; iter < 2000 && ! sawNonZero; ++iter)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;

            const bool allZero = std::all_of (outBuf.begin(), outBuf.end(), [] (float v) { return v == 0.0f; });

            if (! allZero)
            {
                sawNonZero = true;

                for (float v : outBuf)
                    envelope.push_back (std::abs (v));

                for (int extra = 0; extra < 4; ++extra)
                {
                    fifo.pull (outBuf.data(), block, simTime);
                    simTime += halfPeriod;

                    for (float v : outBuf)
                        envelope.push_back (std::abs (v));
                }
            }
        }

        expect (sawNonZero, "再充填が完了しなかった");

        const int fadeSamples = (int) std::lround (0.002 * rate);
        bool monotonic = true;

        for (int i = 1; i < fadeSamples && i < (int) envelope.size(); ++i)
            if (envelope[(size_t) i] < envelope[(size_t) (i - 1)] - 1.0e-6f)
                monotonic = false;

        expect (monotonic, "再開直後2msの包絡が単調増加でない");
    }

    void testR2()
    {
        beginTest ("R2: 入力停止によるアンダーラン");

        vc::ResamplingFifo fifo;
        const double rate = 48000.0;
        const int block = 480;
        fifo.prepare (rate, rate, 4096, 4096, block, block);

        TriangleWave gen (rate);
        std::vector<float> inBuf ((size_t) block), outBuf ((size_t) block);
        double simTime = 0.0;
        const double halfPeriod = (double) block / rate * 0.5;

        for (int i = 0; i < 400; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;
        }

        expectEquals ((int) fifo.stats().underruns.load(), 0, "定常運転中に想定外のunderrun");

        const double jitterBefore = fifo.stats().jitterMarginMs.load();

        bool underrunSeen = false;

        for (int i = 0; i < 200 && ! underrunSeen; ++i)
        {
            const auto before = fifo.stats().underruns.load();
            fifo.pull (outBuf.data(), block, simTime);
            simTime += (double) block / rate;

            if (fifo.stats().underruns.load() != before)
                underrunSeen = true;
        }

        expect (underrunSeen, "アンダーランが発生しなかった");
        expectEquals ((int) fifo.stats().underruns.load(), 1, "underrunsが+1でない");
        expectWithinAbsoluteError (fifo.stats().jitterMarginMs.load(), (float) (jitterBefore + 2.0), 1.0e-3f);

        const double A = 0.5;
        const double slopePerSecond = A * 4.0 / 2.0;
        const double slopePerSample = slopePerSecond / rate;
        const int fadeLen = (int) std::lround (0.002 * rate);
        const double threshold = 1.5 * (slopePerSample + A / (double) fadeLen);

        double maxDiff = 0.0;

        for (size_t i = 1; i < outBuf.size(); ++i)
            maxDiff = std::max (maxDiff, (double) std::abs (outBuf[i] - outBuf[i - 1]));

        expect (maxDiff <= threshold, "フェードアウト区間の隣接差が閾値超過");

        fifo.pull (outBuf.data(), block, simTime);
        simTime += (double) block / rate;
        expect (std::all_of (outBuf.begin(), outBuf.end(), [] (float v) { return v == 0.0f; }),
                "アンダーラン後に0でない出力");

        bool recovered = false;

        for (int i = 0; i < 2000 && ! recovered; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;

            if (std::any_of (outBuf.begin(), outBuf.end(), [] (float v) { return v != 0.0f; }))
                recovered = true;
        }

        expect (recovered, "入力再開後に再充填から復帰しなかった");
    }

    void testR3()
    {
        beginTest ("R3: アンダーラン20回");

        vc::ResamplingFifo fifo;
        const double rate = 48000.0;
        const int block = 480;
        fifo.prepare (rate, rate, 4096, 4096, block, block);

        TriangleWave gen (rate);
        std::vector<float> inBuf ((size_t) block), outBuf ((size_t) block);
        double simTime = 0.0;
        const double halfPeriod = (double) block / rate * 0.5;

        const auto runUntilUnderrun = [&]
        {
            for (int i = 0; i < 400; ++i)
            {
                const auto before = fifo.stats().underruns.load();
                fifo.pull (outBuf.data(), block, simTime);
                simTime += (double) block / rate;

                if (fifo.stats().underruns.load() != before)
                    return;
            }
        };

        const auto refillOnce = [&]
        {
            for (int i = 0; i < 5000; ++i)
            {
                gen.generate (inBuf.data(), block);
                fifo.push (inBuf.data(), block, simTime);
                simTime += halfPeriod;
                fifo.pull (outBuf.data(), block, simTime);
                simTime += halfPeriod;

                if (std::any_of (outBuf.begin(), outBuf.end(), [] (float v) { return v != 0.0f; }))
                    return;
            }
        };

        refillOnce();

        for (int cycle = 0; cycle < 20; ++cycle)
        {
            runUntilUnderrun();
            refillOnce();
        }

        expectWithinAbsoluteError (fifo.stats().jitterMarginMs.load(), 20.0f, 1.0e-3f);
    }

    void testR4()
    {
        beginTest ("R4: 出力を2秒停止");

        vc::ResamplingFifo fifo;
        const double rate = 48000.0;
        const int block = 480;
        fifo.prepare (rate, rate, 4096, 4096, block, block);

        TriangleWave gen (rate);
        std::vector<float> inBuf ((size_t) block), outBuf ((size_t) block);
        double simTime = 0.0;
        const double halfPeriod = (double) block / rate * 0.5;
        const double blockPeriod = (double) block / rate;

        for (int i = 0; i < 2000; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;

            if (std::any_of (outBuf.begin(), outBuf.end(), [] (float v) { return v != 0.0f; }))
                break;
        }

        const int stopBlocks = (int) std::lround (2.0 * rate / block);

        for (int i = 0; i < stopBlocks; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += blockPeriod;
        }

        expect (fifo.stats().overruns.load() > 0, "overrunsが発生しなかった");

        const auto discardsBefore = fifo.stats().discards.load();
        fifo.pull (outBuf.data(), block, simTime);
        simTime += halfPeriod;
        expectEquals ((int) fifo.stats().discards.load(), (int) discardsBefore + 1, "discardsが+1でない");

        // ResamplingFifoの公開APIには瞬時の充填量(平滑化前)を返す手段がないため、
        // 「破棄後の充填量が目標付近まで戻っている」ことを、直後の数回のpull()で
        // 追加の破棄が起きない（=3倍を超えていない）ことで間接的に確認する。
        const auto discardsAfterFirst = fifo.stats().discards.load();

        for (int i = 0; i < 5; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;
        }

        expectEquals ((int) fifo.stats().discards.load(), (int) discardsAfterFirst,
                       "破棄直後にさらに破棄が起きている(充填量が目標付近まで戻っていない)");

        bool converged = false;
        const int maxIters = (int) std::lround (30.0 * rate / block);

        for (int i = 0; i < maxIters && ! converged; ++i)
        {
            gen.generate (inBuf.data(), block);
            fifo.push (inBuf.data(), block, simTime);
            simTime += halfPeriod;
            fifo.pull (outBuf.data(), block, simTime);
            simTime += halfPeriod;

            const double fill = fifo.stats().fillSmoothedSamples.load();
            const double tgt = fifo.stats().targetSamples.load();

            if (std::abs (fill - tgt) <= 0.25 * tgt)
                converged = true;
        }

        expect (converged, "30秒以内に平滑充填が目標±25%へ収束しなかった");
    }

    void testR5()
    {
        beginTest ("R5: 44.1kHz→48kHz、1kHz正弦");

        vc::ResamplingFifo fifo;
        const double inRate = 44100.0, outRate = 48000.0;
        const int bi = 441, bo = 480;
        fifo.prepare (inRate, outRate, 4096, 4096, bi, bo);

        SineWave gen (1000.0, inRate);
        std::vector<float> inBuf ((size_t) bi), outBuf ((size_t) bo);
        double tPush = 0.0, tPull = 0.0;
        const double pushPeriod = (double) bi / inRate;
        const double pullPeriod = (double) bo / outRate;

        for (int i = 0; i < 4000; ++i)
        {
            gen.generate (inBuf.data(), bi, 0.5f);
            fifo.push (inBuf.data(), bi, tPush);
            tPush += pushPeriod;
            fifo.pull (outBuf.data(), bo, tPull);
            tPull += pullPeriod;
        }

        std::vector<float> collected;
        const size_t targetCount = (size_t) outRate;
        collected.reserve (targetCount + (size_t) bo);

        while (collected.size() < targetCount)
        {
            gen.generate (inBuf.data(), bi, 0.5f);
            fifo.push (inBuf.data(), bi, tPush);
            tPush += pushPeriod;
            fifo.pull (outBuf.data(), bo, tPull);
            tPull += pullPeriod;
            collected.insert (collected.end(), outBuf.begin(), outBuf.end());
        }

        const double peakHz = vc::test::findFftPeakHz (collected.data(), (int) targetCount, outRate);
        expectWithinAbsoluteError (peakHz, 1000.0, 1.0, "FFTピークが1000Hz±0.1%に収まらない");
    }

    void testR6()
    {
        beginTest ("R6: アロケーション");

        vc::ResamplingFifo fifo;
        const double rate = 48000.0;
        const int block = 480;
        fifo.prepare (rate, rate, 4096, 4096, block, block);

        TriangleWave gen (rate);
        std::vector<float> inBuf ((size_t) block), outBuf ((size_t) block);
        double simTime = 0.0;
        const double halfPeriod = (double) block / rate * 0.5;

        std::size_t count = 0;

        {
            vc::test::ScopedAllocationGuard guard;

            for (int i = 0; i < 3000; ++i)
            {
                gen.generate (inBuf.data(), block);
                fifo.push (inBuf.data(), block, simTime);
                simTime += halfPeriod;
                fifo.pull (outBuf.data(), block, simTime);
                simTime += halfPeriod;
            }

            count = guard.count();
        }

        expectEquals ((int) count, 0, "push/pull中にアロケーションが発生した");
    }
};

static RingBufferTests ringBufferTests;

// ===== SECTION: RingBufferLong（S1〜S8、1時間模擬） =====

class RingBufferLongTests final : public juce::UnitTest
{
public:
    RingBufferLongTests() : juce::UnitTest ("RingBufferLong", "RingBufferLong") {}

    void runTest() override
    {
        runStandard ("S1: 48k→48k +100ppm", 48000.0, 48000.0, 100.0, 0.0, 480, 480, ScenarioParams::JitterMode::None);
        runStandard ("S2: 48k→48k -100ppm", 48000.0, 48000.0, -100.0, 0.0, 480, 480, ScenarioParams::JitterMode::None);
        runStandard ("S3: 44.1k→48k +100ppm", 44100.0, 48000.0, 100.0, 0.0, 441, 480, ScenarioParams::JitterMode::None);
        runStandard ("S4: 48k→44.1k -100ppm", 48000.0, 44100.0, -100.0, 0.0, 480, 441, ScenarioParams::JitterMode::None);
        runStandard ("S5: 48k→48k +50ppm Bi128/Bo1024", 48000.0, 48000.0, 50.0, 0.0, 128, 1024, ScenarioParams::JitterMode::None);
        runStandard ("S6: 44.1k→48k -100ppm Bi1024/Bo144", 44100.0, 48000.0, -100.0, 0.0, 1024, 144, ScenarioParams::JitterMode::None);
        runStandard ("S7: 48k→48k +100ppm ジッタ0-3ms", 48000.0, 48000.0, 100.0, 0.0, 480, 480, ScenarioParams::JitterMode::Uniform0to3ms);
        runS8();
    }

private:
    void runStandard (const juce::String& testName, double inRate, double outRate, double ppmIn, double ppmOut,
                       int bi, int bo, ScenarioParams::JitterMode jitter)
    {
        beginTest (testName);

        ScenarioParams p;
        p.name = testName;
        p.inRate = inRate;
        p.outRate = outRate;
        p.ppmIn = ppmIn;
        p.ppmOut = ppmOut;
        p.bi = bi;
        p.bo = bo;
        p.jitter = jitter;

        const auto r = runScenario (p);

        logMessage (testName + juce::String::fromUTF8 (": underruns=") + juce::String ((int) r.totalUnderruns)
                    + ", overruns(qualify)=" + juce::String ((int) r.overrunsFromQualify)
                    + ", discards(qualify)=" + juce::String ((int) r.discardsFromQualify)
                    + ", avgFillD=" + juce::String (r.avgFillWindowD, 1)
                    + ", avgFillLast10=" + juce::String (r.avgFillLast10Min, 1)
                    + ", avgPpm=" + juce::String (r.avgSpeedCorrectionPpmFromQualify, 2)
                    + " (期待" + juce::String (r.expectedSpeedCorrectionPpm, 2) + ")"
                    + ", cViolations=" + juce::String ((int) r.cViolations)
                    + ", fViolations=" + juce::String ((int) r.secondDiffViolations));

        expectEquals ((int) r.totalUnderruns, 0, "a) 初期充填後にunderrunが発生した");
        expectEquals ((int) r.overrunsFromQualify, 0, "b) overrunsが発生した");
        expectEquals ((int) r.discardsFromQualify, 0, "b) discardsが発生した");
        expectEquals ((int) r.cViolations, 0, "c) 平滑充填が目標±25%を外れた区間がある");

        const double diffMs = (r.avgFillLast10Min - r.avgFillWindowD) / inRate * 1000.0;
        expect (diffMs <= 0.5, "d) 平滑充填の後半平均が前半平均より0.5ms相当を超えて増加している");

        expectWithinAbsoluteError (r.avgSpeedCorrectionPpmFromQualify, r.expectedSpeedCorrectionPpm, 10.0,
                                    "e) 速度比補正の平均が期待値の±10ppmを外れている");

        expectEquals ((int) r.secondDiffViolations, 0, "f) 出力の2階差分が閾値を超えるサンプルがある");
    }

    void runS8()
    {
        beginTest ("S8: 48k→48k +100ppm 約30秒ごとに15ms遅延");

        ScenarioParams p;
        p.name = "S8";
        p.inRate = 48000.0;
        p.outRate = 48000.0;
        p.ppmIn = 100.0;
        p.ppmOut = 0.0;
        p.bi = 480;
        p.bo = 480;
        p.jitter = ScenarioParams::JitterMode::Special30sDelay15ms;
        p.cQualifyFromSeconds = 600.0;
        p.dWindowStartSeconds = 600.0;
        p.eQualifyFromSeconds = 600.0;
        p.fQualifyFromSeconds = 600.0;
        p.bQualifyFromSeconds = 600.0;

        const auto r = runScenario (p);

        logMessage (juce::String::fromUTF8 ("S8: underruns合計=") + juce::String ((int) r.totalUnderruns)
                    + ", underrunsAfter600=" + juce::String ((int) r.underrunsAfter600)
                    + ", jitterMarginMs=" + juce::String (r.jitterMarginMsFinal, 2)
                    + ", overruns(qualify)=" + juce::String ((int) r.overrunsFromQualify)
                    + ", discards(qualify)=" + juce::String ((int) r.discardsFromQualify)
                    + ", avgFillD=" + juce::String (r.avgFillWindowD, 1)
                    + ", avgFillLast10=" + juce::String (r.avgFillLast10Min, 1)
                    + ", avgPpm=" + juce::String (r.avgSpeedCorrectionPpmFromQualify, 2)
                    + " (期待" + juce::String (r.expectedSpeedCorrectionPpm, 2) + ")"
                    + ", fViolations=" + juce::String ((int) r.secondDiffViolations));

        expect (r.totalUnderruns <= 9, "underrunsの合計が9を超えた");
        expectEquals ((int) r.underrunsAfter600, 0, "10分以降にunderrunが発生した");
        expect (r.jitterMarginMsFinal <= 20.0f + 1.0e-3f, "ジッタ余裕が20msを超えた");

        expectEquals ((int) r.overrunsFromQualify, 0, "b) 10分以降にoverrunsが発生した");
        expectEquals ((int) r.discardsFromQualify, 0, "b) 10分以降にdiscardsが発生した");

        const double diffMs = (r.avgFillLast10Min - r.avgFillWindowD) / p.inRate * 1000.0;
        expect (diffMs <= 0.5, "d) 平滑充填の後半平均が前半平均より0.5ms相当を超えて増加している");

        expectWithinAbsoluteError (r.avgSpeedCorrectionPpmFromQualify, r.expectedSpeedCorrectionPpm, 10.0,
                                    "e) 速度比補正の平均が期待値の±10ppmを外れている");

        expectEquals ((int) r.secondDiffViolations, 0, "f) 出力の2階差分が閾値を超えるサンプルがある");
    }
};

static RingBufferLongTests ringBufferLongTests;
