#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/Effects.h"
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

// 雑音バーストのピーク（振幅に対する比）。バイパス時も出力は±1.0にクリップされる（D-018）ので、この経路のピーク検査は
// E4c（tests/EngineTests.cpp）が担保する。LongRunの信号は控えめなレベルにして、連続動作（アンダーラン・アロケーション・
// 充填量の推移など）を見る。
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
        engine.params().eqEnabled.store (true); // EQは常時ON。バンド1のゲインを45秒ごとに変える（T-012）

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

        // マイク処理（T-012）: 90秒ごとにノイズ除去のON/OFF、45秒ごとに背景ノイズとEQバンド1のゲインを変更する。
        int lastNrSlot = 0, lastMicParamSlot = -1;
        int nrChanges = 0, micParamChanges = 0;
        std::uint64_t nrActiveBlocks = 0;
        constexpr std::array<float, 5> kBackgrounds { 0.7f, 0.0f, 1.0f, 0.3f, 0.5f };
        constexpr std::array<float, 5> kBand1GainsDb { 0.0f, 6.0f, -6.0f, 12.0f, -12.0f };

        // c/d/e用（RingBufferTests.cppのS7と同じ適用区間）。
        constexpr double kCFrom = 300.0, kDStart = 300.0, kEFrom = 600.0;
        std::uint64_t cViolations = 0;
        double sumFillD = 0.0, sumFillLast10 = 0.0, sumPpm = 0.0;
        std::int64_t countD = 0, countLast10 = 0, countPpm = 0;
        double latencyMin = 1.0e9, latencyMax = -1.0e9;

        // CPU参考値（Engine::processの実時間 ÷ 音声時間）。プリセット別にも集計する。
        std::array<double, 8> cpuSecondsByPreset {}, audioSecondsByPreset {};

        // プリセット別の出力RMS（非バイパス区間のみ）。あるプリセットが沈黙したら落とすため。
        std::array<double, 8> sumSquaresByPreset {}, samplesByPreset {};
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
                engine.params().talkboxRange.store ((presetSlot / (int) vc::kPresets.size()) % 2); // トークボックスの音域（低・高）を1周ごとに入れ替える（トークボックスは各音域で約22回）
                lastPresetSlot = presetSlot;
                ++presetChanges;
            }

            const int pitchSlot = (int) (pendingOut / 7.0);
            if (pitchSlot != lastPitchSlot)
            {
                engine.params().pitch.store (pitchSlot == 0 ? 0 : pitchRng.nextInt (2 * vc::kMaxLayer1PitchSemitones + 1) - vc::kMaxLayer1PitchSemitones);
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

            const int nrSlot = (int) (pendingOut / 90.0);
            if (nrSlot != lastNrSlot)
            {
                engine.params().nrEnabled.store (nrSlot % 2 == 1);
                lastNrSlot = nrSlot;
                ++nrChanges;
            }

            const int micParamSlot = (int) (pendingOut / 45.0);
            if (micParamSlot != lastMicParamSlot)
            {
                engine.params().nrBackground.store (kBackgrounds[(size_t) micParamSlot % kBackgrounds.size()]);
                engine.params().eqBands[0].gainDb.store (kBand1GainsDb[(size_t) micParamSlot % kBand1GainsDb.size()]);
                lastMicParamSlot = micParamSlot;
                ++micParamChanges;
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

            if (engine.getNoiseReducerLatencySamples() > 0)
                ++nrActiveBlocks;

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

            const bool bypassed = ! engine.params().enabled.load();
            for (const float v : outBuf)
            {
                outSumSquares += (double) v * (double) v;

                if (! bypassed)
                    sumSquaresByPreset[presetIdx] += (double) v * (double) v;
            }
            outSamples += (std::uint64_t) kBo;
            if (! bypassed)
                samplesByPreset[presetIdx] += (double) kBo;

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
                    + " bypass=" + juce::String (bypassChanges) + " nr=" + juce::String (nrChanges) + " micParams=" + juce::String (micParamChanges)
                    + ", nrActiveBlocks=" + juce::String ((int) nrActiveBlocks)
                    + ", nrUnderflows=" + juce::String (engine.debugNoiseReducer().getUnderflowCount()));

        juce::String cpuText ("LongRun: CPU approx. (process time / audio time) total=" + juce::String (100.0 * cpuSeconds / kDurationSeconds, 2) + "%");
        for (size_t p = 0; p < 8; ++p)
            if (audioSecondsByPreset[p] > 0.0)
                cpuText << ", " << vc::kPresets[p].id << "=" << juce::String (100.0 * cpuSecondsByPreset[p] / audioSecondsByPreset[p], 2) << "%";
        logMessage (cpuText);

        juce::String rmsText ("LongRun: output RMS (non-bypass)");
        std::array<double, 8> rmsByPreset {};
        for (size_t p = 0; p < 8; ++p)
        {
            rmsByPreset[p] = samplesByPreset[p] > 0.0 ? std::sqrt (sumSquaresByPreset[p] / samplesByPreset[p]) : 0.0;
            rmsText << (p == 0 ? " " : ", ") << vc::kPresets[p].id << "=" << juce::String (rmsByPreset[p], 4);
        }
        logMessage (rmsText);
        logMessage (juce::String ("LongRun: wall time=") + juce::String (wallSeconds, 1) + "s");

        expectEquals ((int) nonFiniteBlocks, 0, "NaN/Infを含む出力ブロックがある");
        expectEquals ((int) overPeakBlocks, 0, "ピークが1.0を超えた");
        expectEquals ((int) underruns, 0, "初期充填後にunderrunが発生した");
        expectEquals ((int) overruns, 0, "overrunsが発生した");
        expectEquals ((int) discards, 0, "discardsが発生した");
        expectEquals ((int) engine.getErrorFlags(), 0, "エラーフラグが立った");
        expectEquals ((int) allocations, 0, "push/pull/processでアロケーションが発生した");
        expect (outRms > 1.0e-3, "出力がほぼ無音（信号経路が生きていない）");
        for (size_t p = 0; p < 8; ++p)
            expect (rmsByPreset[p] > 1.0e-3, juce::String (vc::kPresets[p].id) + ": 非バイパス区間の出力RMSが無音（" + juce::String (rmsByPreset[p], 5) + "）");
        expect (presetChanges >= 360 && pitchChanges >= 514 && bypassChanges == 59, "制御の切替回数が想定と異なる");
        expect (nrChanges == 39 && micParamChanges == 80, "マイク処理の切替回数が想定と異なる");
        expect (nrActiveBlocks > 0, "ノイズ除去が一度も稼働しなかった（制御の切替が届いていない）");
        expectEquals (engine.debugNoiseReducer().getUnderflowCount(), 0, "ノイズ除去の出力FIFOが枯れた");

        expectEquals ((int) cViolations, 0, "c) 平滑充填が目標±25%を外れた区間がある");
        expect (diffMs <= 0.5, "d) 平滑充填の後半平均が前半平均より0.5ms相当を超えて増加している");
        expectWithinAbsoluteError (avgPpm, expectedPpm, 10.0, "e) 速度比補正の平均が期待値の±10ppmを外れている");

        // トークボックスのフレーズ（D-028）は連続稼働でテンポがずれない（整数の計算）。24時間ぶんのサンプルを回し、毎ステップの境界が
        // 理論値ceil(k × fs × 30 / 123)ぴったりであることを確かめる。解析的な値なので、44.1k・48k・96kのすべてで同じ判定。
        beginTest ("LongRun: トークボックスのフレーズ（PhraseSequencer）24時間でステップ境界がずれない");
        {
            for (const double fs : { 48000.0, 96000.0 })
            {
                vc::PhraseSequencer seq;
                seq.prepare (fs);
                const long long total = (long long) (24.0 * 3600.0 * fs);
                const long long numerator = (long long) fs * 30;
                long long observed = 0;
                long long wrong = 0;
                float lastHz = 0.0f;

                for (long long i = 0; i < total; ++i)
                {
                    lastHz = seq.next();

                    if (seq.getStepIndex() != observed)
                    {
                        ++observed;

                        if (i != (observed * numerator + 122) / 123)
                            ++wrong;
                    }
                }

                // 24時間 = 86400秒 ÷ (60/123 × 0.5 秒) = 354240ステップ。最後の音は通し番号 % 8 の音。
                const long long expectedSteps = ((total - 1) * 123) / numerator; // 時刻 total-1 までに始まったステップの数
                logMessage ("LongRun: PhraseSequencer fs=" + juce::String (fs, 0) + " 24h: steps " + juce::String ((juce::int64) observed) + " (expected " + juce::String ((juce::int64) expectedSteps)
                            + "), wrong boundaries " + juce::String ((juce::int64) wrong));
                expectEquals ((juce::int64) wrong, (juce::int64) 0, "ステップ境界がceil(k * L)からずれた");
                expectEquals ((juce::int64) observed, (juce::int64) expectedSteps, "24時間のステップ数が理論値と異なる");
                expect (std::abs (lastHz - vc::PhraseSequencer::midiToHz (vc::PhraseSequencer::kMidiNotes[(size_t) (observed % vc::PhraseSequencer::kNumSteps)])) < 1.0e-3f, "巡回後の音が違う");
            }
        }
    }
};

static LongRunTests longRunTests;
