#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"

#include "core/Engine.h"
#include "core/MicProcessing.h"

#include <rnnoise.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

// ===== SECTION: MicTests =====
// マイク処理のテスト（カテゴリMic、quick）。T-009のN0a〜N0e（RNNoiseそのものの確認）と、
// T-010のNoiseReducer・Engine組み込み（N3a・N4a・N6〜N11。docs/plan.md 8.3）。
// N1・N2・N3b・N4b・N5とEQ（EQ1〜）はT-011以降で追加する。
// RNNoiseの入出力はint16の値域のfloat（±32768）、1フレーム480サンプル（48kHzで10ms）。

namespace
{

constexpr double kFs = 48000.0;
constexpr int kFrame = 480;

// 一様乱数の白色雑音（振幅amp）。
std::vector<float> makeWhiteNoise (int n, float amp, int seed)
{
    std::vector<float> out (static_cast<size_t> (n));
    juce::Random rng (seed);

    for (auto& s : out)
        s = amp * (rng.nextFloat() * 2.0f - 1.0f);

    return out;
}

// inを480サンプルずつrnnoise_process_frameへ通す（nは480の倍数）。戻り値のVAD確率が[0,1]に収まらなければfalse。
bool processAll (DenoiseState* st, const std::vector<float>& in, std::vector<float>& out)
{
    out.assign (in.size(), 0.0f);
    bool vadOk = true;

    for (size_t pos = 0; pos + kFrame <= in.size(); pos += kFrame)
    {
        const float vad = rnnoise_process_frame (st, out.data() + pos, in.data() + pos);
        vadOk = vadOk && vad >= 0.0f && vad <= 1.0f;
    }

    return vadOk;
}

class RnnoiseTests final : public juce::UnitTest
{
public:
    RnnoiseTests() : juce::UnitTest ("Rnnoise", "Mic") {}

    void runTest() override
    {
        beginTest ("N0a: frame size is 480 samples");
        {
            expectEquals (rnnoise_get_frame_size(), kFrame);
        }

        beginTest ("N0b: sine, white noise and silence (1 s each) give finite output");
        {
            const int n = static_cast<int> (kFs);
            const auto sine = vc::test::makeSine (1000.0, kFs, n, 8000.0f);
            const auto noise = makeWhiteNoise (n, 8000.0f, 12345);
            const std::vector<float> silence (static_cast<size_t> (n), 0.0f);

            const std::vector<float>* inputs[] = { &sine, &noise, &silence };
            const char* names[] = { "sine", "white noise", "silence" };

            for (int k = 0; k < 3; ++k)
            {
                auto* st = rnnoise_create (nullptr);
                expect (st != nullptr, "rnnoise_create failed");

                if (st == nullptr)
                    continue;

                std::vector<float> out;
                const bool vadOk = processAll (st, *inputs[k], out);
                rnnoise_destroy (st);

                expect (vadOk, juce::String (names[k]) + ": VAD probability outside [0, 1]");
                expect (vc::test::allFinite (out.data(), static_cast<int> (out.size())),
                        juce::String (names[k]) + ": non-finite output");
            }
        }

        beginTest ("N0c: intrinsic delay of rnnoise_process_frame is 480 or 960 samples (+-2)");
        {
            // 2秒分。インパルス・1kHzのバースト・雑音バーストはRNNoiseが雑音として抑えるため相互相関にならず、
            // 抑えられない合成母音（makeSyllables）で測る。
            const int n = static_cast<int> (2.0 * kFs);
            const auto in = vc::test::makeSyllables (n);
            std::vector<float> out;

            auto* st = rnnoise_create (nullptr);
            expect (st != nullptr, "rnnoise_create failed");

            if (st != nullptr)
            {
                processAll (st, in, out);
                rnnoise_destroy (st);

                int bestLag = 0;
                double bestCorr = -1.0e300;
                double inEnergy = 0.0;
                double outEnergy = 0.0;

                for (int i = 0; i < n; ++i)
                {
                    inEnergy += (double) in[(size_t) i] * in[(size_t) i];
                    outEnergy += (double) out[(size_t) i] * out[(size_t) i];
                }

                for (int lag = 0; lag < 2000; ++lag)
                {
                    double c = 0.0;

                    for (int i = 0; i + lag < n; ++i)
                        c += (double) in[(size_t) i] * out[(size_t) (i + lag)];

                    if (c > bestCorr)
                    {
                        bestCorr = c;
                        bestLag = lag;
                    }
                }

                logMessage ("N0c measured delay: " + juce::String (bestLag) + " samples ("
                            + juce::String (1000.0 * bestLag / kFs, 2) + " ms at 48 kHz), output/input energy "
                            + juce::String (outEnergy / inEnergy, 3));

                expect (outEnergy > 0.5 * inEnergy, "the test vowel was suppressed; the delay measurement is not valid");
                expect (std::abs (bestLag - 480) <= 2 || std::abs (bestLag - 960) <= 2,
                        "delay " + juce::String (bestLag) + " samples is neither 480 nor 960");
            }
        }

        beginTest ("N0d: rnnoise_process_frame and rnnoise_init (default model) do not allocate");
        {
            // 確保は作成時（rnnoise_create）だけ。ガード中のmalloc等の計数はglibc環境のみ（tests/TestMain.cpp）。
            auto* st = rnnoise_create (nullptr);
            expect (st != nullptr, "rnnoise_create failed");

            if (st != nullptr)
            {
                const auto in = vc::test::makeSyllables (kFrame * 50);
                const auto noise = makeWhiteNoise (kFrame * 50, 4000.0f, 777);
                std::vector<float> out (static_cast<size_t> (kFrame));

                std::size_t processCount = 0;
                std::size_t initCount = 0;
                int initResult = -1;

                {
                    vc::test::ScopedAllocationGuard guard;

                    for (int f = 0; f < 50; ++f)
                    {
                        rnnoise_process_frame (st, out.data(), in.data() + f * kFrame);
                        rnnoise_process_frame (st, out.data(), noise.data() + f * kFrame);
                    }

                    processCount = guard.count();
                }

                {
                    vc::test::ScopedAllocationGuard guard;
                    initResult = rnnoise_init (st, nullptr);
                    initCount = guard.count();
                }

                expectEquals (initResult, 0);
                expectEquals ((int) processCount, 0);
                expectEquals ((int) initCount, 0);
                rnnoise_destroy (st);
            }

#if defined(__GLIBC__)
            // 陽性対照: Cライブラリ内部のmalloc/freeがガード中に数えられること（数えられないと上の0回が意味を持たない）。
            // rnnoise_createはCの確保（calloc/malloc）、rnnoise_destroyは解放（free）を行う。
            {
                std::size_t createCount = 0;
                std::size_t destroyCount = 0;
                DenoiseState* created = nullptr;

                {
                    vc::test::ScopedAllocationGuard guard;
                    created = rnnoise_create (nullptr);
                    createCount = guard.count();
                }

                {
                    vc::test::ScopedAllocationGuard guard;
                    rnnoise_destroy (created);
                    destroyCount = guard.count();
                }

                expect (createCount >= 1, "rnnoise_create was not counted by the guard: " + juce::String ((int) createCount));
                expect (destroyCount >= 1, "rnnoise_destroy was not counted by the guard: " + juce::String ((int) destroyCount));
                expect (createCount + destroyCount >= 2, "C allocations inside the library were not counted");
            }
#endif
        }

        beginTest ("N0e: CPU cost of 10 s of 48 kHz audio (reference only, never fails)");
        {
            const int n = static_cast<int> (10.0 * kFs);
            auto in = vc::test::makeSyllables (n);
            const auto noise = makeWhiteNoise (n, 500.0f, 4242); // 話し声＋背景雑音
            for (size_t i = 0; i < in.size(); ++i)
                in[i] += noise[i];

            auto* st = rnnoise_create (nullptr);
            expect (st != nullptr, "rnnoise_create failed");

            if (st != nullptr)
            {
                std::vector<float> out (static_cast<size_t> (kFrame));
                double maxFrameMs = 0.0;
                double totalMs = 0.0;

                for (int pos = 0; pos + kFrame <= n; pos += kFrame)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    rnnoise_process_frame (st, out.data(), in.data() + pos);
                    const auto t1 = std::chrono::steady_clock::now();

                    const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
                    totalMs += ms;
                    maxFrameMs = std::max (maxFrameMs, ms);
                }

                rnnoise_destroy (st);

                logMessage ("N0e CPU: " + juce::String (totalMs, 1) + " ms for 10000 ms of audio = "
                            + juce::String (100.0 * totalMs / 10000.0, 3) + " % of one core, max "
                            + juce::String (maxFrameMs, 3) + " ms per 10 ms frame");
            }
        }
    }
};

static RnnoiseTests rnnoiseTests;

// ===== SECTION: NoiseReducer（T-010） =====

constexpr int kNativeD = 1440; // 48kHzのD（フレーム収集480 + RNNoise固有960）

// ブロック長の混在列（N11）。
constexpr int kMixedBlocks[] = { 1, 7, 128, 441, 480, 4096 };

// signalをblockSizeごとにEngineで処理する（その場）。
void processInBlocks (vc::Engine& engine, std::vector<float>& data, int blockSize, int from = 0, int to = -1)
{
    const int end = to < 0 ? (int) data.size() : to;

    for (int pos = from; pos < end; pos += blockSize)
        engine.process (data.data() + pos, std::min (blockSize, end - pos));
}

// 信号のピークをpeakへそろえる。
void setPeak (std::vector<float>& v, float peak)
{
    const double p = vc::test::peakAbs (v.data(), (int) v.size());

    if (p > 0.0)
        for (auto& x : v)
            x = (float) ((double) x * (double) peak / p);
}

// NoiseReducer単体で、背景ノイズbgのまま全体を処理して返す（blockSizeごと）。
std::vector<float> runNoiseReducer (double fs, const std::vector<float>& in, float bg, int blockSize = 480)
{
    vc::NoiseReducer nr;
    nr.prepare (fs, 4096);

    std::vector<float> out (in);

    for (int pos = 0; pos < (int) out.size(); pos += blockSize)
    {
        nr.setTarget (true, bg, 0.0f);
        nr.process (out.data() + pos, std::min (blockSize, (int) out.size() - pos));
    }

    return out;
}

constexpr double kThirdOctaveCenters[] = { 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000 };

struct SpectrumComparison
{
    double levelChangeDb = 0.0;       // 全体のレベル変化（out/in）
    double maxBandDiffDb = 0.0;       // 1/3オクターブ長時間スペクトルの差の最大（in側が最大帯域-25dB以上の帯域のみ）
    int bandsCompared = 0;
    double fundamentalChangeDb = 0.0; // f0の±10%の帯域パワーの変化
};

// in[start, start+len)とout[start+delay, ...)を比べる（出力はdelayだけ遅れている）。
SpectrumComparison compareSpectra (const std::vector<float>& in, const std::vector<float>& out, int delay, int start, int len,
                                   double fs, double f0)
{
    SpectrumComparison r;
    const float* a = in.data() + start;
    const float* b = out.data() + start + delay;

    r.levelChangeDb = 20.0 * std::log10 (vc::test::rms (b, len) / vc::test::rms (a, len));

    constexpr int order = 14;
    const auto magIn = vc::test::averagedMagnitudeSpectrum (a, len, order);
    const auto magOut = vc::test::averagedMagnitudeSpectrum (b, len, order);
    const int fftSize = 1 << order;
    const double edge = std::pow (2.0, 1.0 / 6.0);

    std::vector<double> bandIn, bandOut;
    double maxIn = -1.0e300;

    for (const double fc : kThirdOctaveCenters)
    {
        bandIn.push_back (vc::test::bandPowerDb (magIn, fs, fftSize, fc / edge, fc * edge));
        bandOut.push_back (vc::test::bandPowerDb (magOut, fs, fftSize, fc / edge, fc * edge));
        maxIn = std::max (maxIn, bandIn.back());
    }

    for (size_t i = 0; i < bandIn.size(); ++i)
    {
        if (bandIn[i] < maxIn - 25.0)
            continue;

        r.maxBandDiffDb = std::max (r.maxBandDiffDb, std::abs (bandOut[i] - bandIn[i]));
        ++r.bandsCompared;
    }

    r.fundamentalChangeDb = vc::test::bandPowerDb (magOut, fs, fftSize, 0.9 * f0, 1.1 * f0)
                            - vc::test::bandPowerDb (magIn, fs, fftSize, 0.9 * f0, 1.1 * f0);
    return r;
}

class NoiseReducerTests final : public juce::UnitTest
{
public:
    NoiseReducerTests() : juce::UnitTest ("NoiseReducer", "Mic") {}

    void runTest() override
    {
        runN6();
        runN6States();
        runN7();
        runN8();
        runN9a();
        runN10();
        runN11();
        runN3a();
        runN3aControls();
        runN4a();
        runBypassTests();
        runCpuReference();
    }

private:
    // ----- N6: 遅延 -----
    // 報告値 = 実測値（±1サンプル）。ブロック長を変えても一定。RNNoiseを通る経路（背景70%: d = 0）と、
    // 原音の遅延線の経路（背景0%: d = 1）の両方を測る（後者はD丁度、前者はRNNoise・変換の遅延を含む）。
    void runN6()
    {
        beginTest ("N6: reported latency equals measured delay (+-1 sample) at 48k/44.1k/96k, for several block sizes");

        for (const double fs : { 48000.0, 44100.0, 96000.0 })
        {
            const int n = (int) (3.0 * fs);
            auto in = vc::test::makeSyllables (n, fs);
            setPeak (in, 0.3f);

            for (const int blockSize : { 480, 128, 441 })
            {
                for (const float bg : { 0.7f, 0.0f })
                {
                    vc::Engine engine;
                    engine.prepare ({ fs, 1024 });
                    engine.params().nrEnabled.store (true);
                    engine.params().nrBackground.store (bg);
                    expectEquals (engine.getNoiseReducerLatencySamples(), 0);

                    auto out = in;
                    processInBlocks (engine, out, blockSize);

                    const int reported = engine.getNoiseReducerLatencySamples();
                    const double measured = vc::test::measureDelaySamples (in, out, reported - 100, reported + 100);
                    const juce::String label = juce::String (fs, 0) + " Hz, block " + juce::String (blockSize)
                                               + ", " + (bg > 0.5f ? "RNNoise path" : "dry path");

                    logMessage ("N6 " + label + ": reported " + juce::String (reported) + ", measured "
                                + juce::String (measured, 2));
                    expect (reported > 0, label + ": latency is not reported");
                    expect (std::abs (measured - reported) <= 1.0, label + ": reported " + juce::String (reported)
                                                                       + " vs measured " + juce::String (measured, 2));

                    if (std::abs (fs - kFs) < 0.5)
                        expectEquals (reported, kNativeD);
                }
            }
        }
    }

    // 報告値の規則（PitchShifterと同じ）: Restingで0、Primingで0、FadingIn・Active・FadingOutでD。
    void runN6States()
    {
        beginTest ("N6: reported latency follows the state (0 in Resting/Priming, D in FadingIn/Active/FadingOut, 0 again after Resting)");

        vc::Engine engine;
        engine.prepare ({ kFs, 512 });
        auto data = vc::test::makeSpeechLikeVowel (140.0, kFs, kFrame * 40, 0.3f);
        int pos = 0;
        const auto block = [&]
        {
            engine.process (data.data() + pos, kFrame);
            pos += kFrame;
        };

        block();
        expectEquals (engine.getNoiseReducerLatencySamples(), 0, "OFF");
        engine.params().nrEnabled.store (true);
        block();
        block();
        expectEquals (engine.getNoiseReducerLatencySamples(), 0, "Priming (960 of 1440 samples fed)");
        block();
        block(); // 1440サンプル送り込んだ時点でFadingIn
        expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, "FadingIn");
        block();
        block();
        block();
        expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, "Active");
        engine.params().nrEnabled.store (false);
        block();
        expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, "FadingOut");
        block();
        block();
        expectEquals (engine.getNoiseReducerLatencySamples(), 0, "Resting after FadingOut");
    }

    // ----- N7: アロケーション -----
    // ON/OFFの全遷移・パラメータ掃引・NaN注入によるリセット（rnnoise_init）・バイパスを含め、Engine::process中の確保0回。
    // C言語のmalloc（RNNoise）はglibc環境のみ数える（tests/TestMain.cpp）。
    void runN7()
    {
        beginTest ("N7: no allocations in Engine::process for all noise-reduction transitions, sweeps and the 44.1 kHz path");

        for (const double fs : { 48000.0, 44100.0 })
        {
            constexpr int maxBlock = 512;
            const int n = (int) (2.0 * fs);

            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.25f);
            const auto pink = vc::test::makePinkNoise (n, 0.02f);
            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += pink[i];
            vc::test::addClick (signal, n / 2, fs, 3000.0, 1.0, 0.3f);

            vc::Engine engine;
            engine.prepare ({ fs, maxBlock });
            auto& params = engine.params();

            std::vector<float> work ((size_t) maxBlock);
            int cursor = 0;
            int blockToggle = 0;

            // 音声区間を循環して読み、480と137を交互に処理する。
            const auto run = [&] (int samples, bool injectNaN = false)
            {
                for (int done = 0; done < samples;)
                {
                    const int len = std::min ((blockToggle++ & 1) ? 137 : 480, samples - done);

                    for (int i = 0; i < len; ++i)
                    {
                        work[(size_t) i] = signal[(size_t) cursor];
                        cursor = (cursor + 1) % n;
                    }

                    if (injectNaN)
                        work[3] = std::numeric_limits<float>::quiet_NaN();

                    engine.process (work.data(), len);
                    done += len;
                }
            };

            const int ms20 = (int) (0.02 * fs);
            const int settle = (int) (0.4 * fs); // Priming（30ms）+ フェード（20ms）+ 余裕

            std::size_t allocations = 0;
            {
                vc::test::ScopedAllocationGuard guard;

                run (ms20 * 5);                                  // OFF（Resting）
                params.nrEnabled.store (true);                   // Resting → Priming → FadingIn → Active
                run (settle);
                params.nrBackground.store (0.0f);                // 掃引（背景・インパクト）
                for (int step = 0; step <= 100; ++step)
                {
                    params.nrBackground.store ((float) step / 100.0f);
                    params.nrImpact.store ((float) (100 - step) / 100.0f);
                    run (ms20 / 2);
                }
                params.nrBackground.store (std::numeric_limits<float>::quiet_NaN()); // 非有限値も安全に扱う
                params.nrImpact.store (std::numeric_limits<float>::infinity());
                run (ms20);
                params.nrBackground.store (0.7f);
                params.nrImpact.store (0.0f);

                params.nrEnabled.store (false);                  // Active → FadingOut
                run (ms20 / 2);
                params.nrEnabled.store (true);                   // FadingOut → FadingIn（反転）
                run (ms20 / 2);
                params.nrEnabled.store (false);                  // FadingIn → FadingOut → Resting
                run (settle);

                params.nrEnabled.store (true);                   // Priming → Resting（中止）
                run (ms20);
                params.nrEnabled.store (false);
                run (ms20);
                params.nrEnabled.store (true);                   // Priming → FadingIn → 反転
                run (ms20 * 2 + ms20 / 2);
                params.nrEnabled.store (false);
                run (ms20);
                params.nrEnabled.store (true);
                run (settle);

                params.enabled.store (false);                    // バイパス中もノイズ除去は動く
                run (settle);
                params.enabled.store (true);
                run (settle);

                run (ms20, true);                                // NaN注入 → 全体リセット（rnnoise_init）→ 再Priming
                run (settle);
                params.enabled.store (false);
                run (ms20, true);                                // バイパス中のNaN
                run (settle);
                params.enabled.store (true);

                allocations = guard.count();
            }

            expectEquals ((int) allocations, 0, "allocations at " + juce::String (fs, 0) + " Hz");
        }
    }

    // ----- N8: NaN/Inf -----
    // Active（ほかにPriming・FadingIn・バイパス中）でNaN/Inf → そのブロックの出力0・フラグ。
    // 以後D + 2ブロック以内に非ゼロへ復帰し、以後すべて有限。
    void runN8()
    {
        beginTest ("N8: NaN/Inf injected while noise reduction is on: zeroed block, flag, recovery within D + 2 blocks, finite afterwards");

        constexpr int block = 480;
        constexpr int maxBlock = 512;
        const int n = (int) (6.0 * kFs);
        auto signal = vc::test::makeSpeechLikeVowel (140.0, kFs, n, 0.3f);

        struct Case
        {
            const char* name;
            float bad;
            bool bypass;
            int injectAfterBlocks; // 稼働要求から何ブロック目に注入するか（Active = 100ブロック目ごろ、Priming = 1、FadingIn = 3〜4）
        };

        const Case cases[] = {
            { "Active NaN", std::numeric_limits<float>::quiet_NaN(), false, 100 },
            { "Active Inf", std::numeric_limits<float>::infinity(), false, 100 },
            { "Active NaN (bypass)", std::numeric_limits<float>::quiet_NaN(), true, 100 },
            { "Active Inf (bypass)", std::numeric_limits<float>::infinity(), true, 100 },
            { "Priming NaN", std::numeric_limits<float>::quiet_NaN(), false, 1 },
            { "FadingIn Inf", std::numeric_limits<float>::infinity(), false, 4 },
        };

        for (const auto& c : cases)
        {
            vc::Engine engine;
            engine.prepare ({ kFs, maxBlock });
            engine.params().enabled.store (! c.bypass);
            engine.params().nrEnabled.store (true);

            auto data = signal;
            int pos = 0;
            int blockIndex = 0;

            for (; blockIndex < c.injectAfterBlocks; ++blockIndex, pos += block)
                engine.process (data.data() + pos, block);

            const int expectedLatency = engine.getNoiseReducerLatencySamples();
            if (c.injectAfterBlocks >= 100)
                expect (expectedLatency == kNativeD, juce::String (c.name) + ": not Active before the injection");

            data[(size_t) pos + 10] = c.bad;
            engine.process (data.data() + pos, block);
            const bool silenced = vc::test::peakAbs (data.data() + pos, block) <= 0.0;
            pos += block;

            expect (silenced, juce::String (c.name) + ": the block was not silenced");
            expect ((engine.getErrorFlags() & 0x1u) != 0, juce::String (c.name) + ": bit0 not set");
            engine.clearErrorFlags();

            int firstNonZeroBlocks = -1;
            int blocksRun = 0;

            for (; pos + block <= n; pos += block, ++blocksRun)
            {
                engine.process (data.data() + pos, block);

                if (firstNonZeroBlocks < 0 && vc::test::peakAbs (data.data() + pos, block) > 1.0e-6)
                    firstNonZeroBlocks = blocksRun + 1;
            }

            const int limitBlocks = (kNativeD + block - 1) / block + 2;
            expect (firstNonZeroBlocks > 0 && firstNonZeroBlocks <= limitBlocks,
                    juce::String (c.name) + ": recovered after " + juce::String (firstNonZeroBlocks) + " blocks (limit " + juce::String (limitBlocks) + ")");
            expect (vc::test::allFinite (data.data() + (n - blocksRun * block), blocksRun * block),
                    juce::String (c.name) + ": non-finite output after recovery");
            expect (engine.getErrorFlags() == 0, juce::String (c.name) + ": a further error was flagged after recovery");
            expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, juce::String (c.name) + ": did not return to Active");
        }
    }

    // ----- N9a: 切り替えのクリック -----
    // 200Hz・A=0.3の正弦（-80dBFSの雑音付き）。checkNoClickの区間: 前=切替の直前0.5秒、切替区間=切替から0.4秒、後=切替の1.0秒後から0.5秒。
    struct Step
    {
        int atSample;
        std::function<void (vc::AtomicParams&)> apply;
    };

    std::vector<float> runScenario (bool nrInitiallyOn, float bgInitial, std::vector<Step> steps, int total,
                                    const std::vector<float>& signal, int blockSize = 480)
    {
        vc::Engine engine;
        engine.prepare ({ kFs, 1024 });
        engine.params().nrEnabled.store (nrInitiallyOn);
        engine.params().nrBackground.store (bgInitial);

        std::sort (steps.begin(), steps.end(), [] (const Step& a, const Step& b) { return a.atSample < b.atSample; });

        auto out = signal;
        int pos = 0;
        size_t next = 0;

        while (pos < total)
        {
            while (next < steps.size() && steps[next].atSample <= pos)
                steps[next++].apply (engine.params());

            int len = std::min (blockSize, total - pos);

            if (next < steps.size())
                len = std::min (len, steps[next].atSample - pos);

            engine.process (out.data() + pos, len);
            pos += len;
        }

        return out;
    }

    void runN9a()
    {
        beginTest ("N9a: no clicks when noise reduction is switched (OFF/ON, abort in Priming, reversal in FadingIn/FadingOut, background 0 <-> 100 %)");

        const int total = (int) (4.2 * kFs);

        const int steady = (int) (0.5 * kFs);
        const int transLen = (int) (0.4 * kFs);
        const int afterGap = (int) (1.0 * kFs);
        const int base = (int) (2.0 * kFs);

        const auto setOn = [] (bool on) { return [on] (vc::AtomicParams& p) { p.nrEnabled.store (on); }; };
        const auto setBg = [] (float bg) { return [bg] (vc::AtomicParams& p) { p.nrBackground.store (bg); }; };

        struct Scenario
        {
            const char* name;
            bool initiallyOn;
            float bg;
            std::function<std::vector<Step> (int t)> steps;
        };

        const std::vector<Scenario> scenarios = {
            { "OFF -> ON", false, 0.7f, [&] (int t) { return std::vector<Step> { { t, setOn (true) } }; } },
            { "ON -> OFF", true, 0.7f, [&] (int t) { return std::vector<Step> { { t, setOn (false) } }; } },
            { "abort in Priming", false, 0.7f,
              [&] (int t) { return std::vector<Step> { { t, setOn (true) }, { t + 700, setOn (false) } }; } },
            { "reversal in FadingIn", false, 0.7f,
              [&] (int t) { return std::vector<Step> { { t, setOn (true) }, { t + kNativeD + 400, setOn (false) } }; } },
            { "reversal in FadingOut", true, 0.7f,
              [&] (int t) { return std::vector<Step> { { t, setOn (false) }, { t + 400, setOn (true) } }; } },
            { "background 0 -> 100 %", true, 0.0f, [&] (int t) { return std::vector<Step> { { t, setBg (1.0f) } }; } },
            { "background 100 -> 0 %", true, 1.0f, [&] (int t) { return std::vector<Step> { { t, setBg (0.0f) } }; } },
        };

        // 200Hzは仕様どおり（Dの1440サンプルはちょうど6周期で、原音と処理後が同位相）。210Hzは位相がずれた状態でのクロスフェードを確認する。
        for (const double freq : { 200.0, 210.0 })
        for (const auto& sc : scenarios)
        {
            auto signal = vc::test::makeSine (freq, kFs, total, 0.3f);
            vc::test::addNoiseFloor (signal, -80.0f);

            for (const int offset : { 0, 37, 211 })
            {
                const int t = base + offset;
                const auto out = runScenario (sc.initiallyOn, sc.bg, sc.steps (t), total, signal);

                const bool ok = vc::test::checkNoClick (out.data() + t - steady, steady, out.data() + t, transLen,
                                                        out.data() + t + afterGap);
                const double before = vc::test::maxAdjacentDiff (out.data() + t - steady, steady);
                const double after = vc::test::maxAdjacentDiff (out.data() + t + afterGap, steady);
                const double trans = vc::test::maxAdjacentDiff (out.data() + t, transLen);
                logMessage ("N9a " + juce::String (freq, 0) + " Hz " + juce::String (sc.name) + " offset " + juce::String (offset) + ": transition/threshold ratio "
                            + juce::String (trans / (1.5 * std::max (before, after)), 3));
                expect (ok, juce::String ("click detected: ") + sc.name + ", " + juce::String (freq, 0) + " Hz, offset " + juce::String (offset));
                expect (vc::test::allFinite (out.data(), total), juce::String ("non-finite output: ") + sc.name);
            }
        }
    }

    // ----- N10: OFF時のビット一致 -----
    void runN10()
    {
        beginTest ("N10: NoiseReducer::process leaves the buffer bit-identical while OFF (also after ON -> OFF completes)");

        juce::Random rng (2024);
        const auto makeInput = [&] (int n)
        {
            std::vector<float> v ((size_t) n);
            for (auto& x : v)
                x = (rng.nextFloat() * 2.0f - 1.0f) * 3.0f; // |x| > 1を含む
            return v;
        };

        vc::NoiseReducer nr;
        nr.prepare (kFs, 480);

        const auto checkIdentical = [&] (int blocks, const juce::String& label)
        {
            bool identical = true;

            for (int b = 0; b < blocks; ++b)
            {
                auto data = makeInput (480);
                const auto copy = data;
                nr.setTarget (false, 0.7f, 0.0f);
                const bool ok = nr.process (data.data(), 480);
                identical = identical && ok && std::memcmp (data.data(), copy.data(), sizeof (float) * 480) == 0;
            }

            expect (identical, label);
        };

        checkIdentical (20, "not bit-identical while OFF from the start");
        expectEquals (nr.getLatencySamples(), 0);

        // ON（Active）にしてからOFFへ。フェードアウト完了までは一致しない。完了後は一致する。
        for (int b = 0; b < 20; ++b)
        {
            auto data = makeInput (480);
            nr.setTarget (true, 0.7f, 0.0f);
            nr.process (data.data(), 480);
        }

        expectEquals (nr.getLatencySamples(), kNativeD);

        for (int b = 0; b < 5; ++b) // 20msのフェードアウト（960サンプル）を完了させる
        {
            auto data = makeInput (480);
            nr.setTarget (false, 0.7f, 0.0f);
            nr.process (data.data(), 480);
        }

        expectEquals (nr.getLatencySamples(), 0);
        checkIdentical (20, "not bit-identical after ON -> OFF completed");

        // Priming中の中止後も一致する。
        {
            auto data = makeInput (480);
            nr.setTarget (true, 0.7f, 0.0f);
            nr.process (data.data(), 480);
        }
        checkIdentical (20, "not bit-identical after an aborted Priming");
    }

    // ----- N11: 分割処理 -----
    void runN11()
    {
        beginTest ("N11: mixed block lengths give bit-identical output to one-shot processing; no output FIFO underflow");

        // (a) NoiseReducer単体: 混在列（1, 7, 128, 441, 480, 4096）と、480ごとの処理が一致。複数のレートで枯渇0回。
        for (const double fs : { 48000.0, 44100.0, 96000.0, 88200.0, 32000.0 })
        {
            const int n = (int) (1.5 * fs);
            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.3f);
            const auto pink = vc::test::makePinkNoise (n, 0.03f);
            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += pink[i];

            const auto runSequence = [&] (const std::vector<int>& sizes, int* underflows)
            {
                vc::NoiseReducer nr;
                nr.prepare (fs, 4096);
                auto out = signal;
                int pos = 0;
                size_t k = 0;

                while (pos < n)
                {
                    const int len = std::min (sizes[k++ % sizes.size()], n - pos);
                    nr.setTarget (true, 0.3f, 0.0f); // 背景30%（d = 0.16）: 遅延線の混合も通す
                    nr.process (out.data() + pos, len);
                    pos += len;
                }

                *underflows = nr.getUnderflowCount();
                return out;
            };

            int underMixed = 0, underRef = 0, underOne = 0;
            const auto mixed = runSequence (std::vector<int> (std::begin (kMixedBlocks), std::end (kMixedBlocks)), &underMixed);
            const auto ref = runSequence ({ 480 }, &underRef);
            const auto oneSample = runSequence ({ 1 }, &underOne);

            const juce::String label = juce::String (fs, 0) + " Hz";
            expect (std::memcmp (mixed.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": mixed blocks differ from 480-blocks");
            expect (std::memcmp (oneSample.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": 1-sample blocks differ from 480-blocks");
            expectEquals (underMixed + underRef + underOne, 0, label + ": output FIFO underflow");
            expect (vc::test::allFinite (ref.data(), n), label + ": non-finite output");
        }

        // (b) Engine全体: n = 3 * maxBlock + 17 の一括処理（内部でmaxBlockごとに分割）と、混在列。44.1kも含む。
        for (const double fs : { 48000.0, 44100.0 })
        {
            constexpr int maxBlock = 4096;
            const int n = 3 * maxBlock + 17;
            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.3f);

            const auto runEngine = [&] (bool oneShot)
            {
                vc::Engine engine;
                engine.prepare ({ fs, maxBlock });
                engine.params().nrEnabled.store (true);
                engine.params().nrBackground.store (0.3f);

                auto out = signal;

                if (oneShot)
                {
                    engine.process (out.data(), n);
                }
                else
                {
                    int pos = 0;
                    size_t k = 0;

                    while (pos < n)
                    {
                        const int len = std::min (kMixedBlocks[k++ % std::size (kMixedBlocks)], n - pos);
                        engine.process (out.data() + pos, len);
                        pos += len;
                    }
                }

                expectEquals (engine.debugNoiseReducer().getUnderflowCount(), 0);
                return out;
            };

            const auto a = runEngine (true);
            const auto b = runEngine (false);
            expect (std::memcmp (a.data(), b.data(), sizeof (float) * (size_t) n) == 0,
                    "Engine at " + juce::String (fs, 0) + " Hz: one-shot and mixed blocks differ");
        }
    }

    // ----- N3a: 清音の歪み（RNNoiseのみ） -----
    // 背景ノイズ50%（d = 0: 完全なRNNoise出力）。雑音のない合成母音を8秒処理し、1秒〜7秒の区間を比べる。
    void runN3a()
    {
        beginTest ("N3a: clean vowel through RNNoise only: level change <= 1.5 dB, 1/3-octave spectrum difference <= 3 dB");

        struct Config { double fs; float bg; };
        // 主条件は48kHz・背景50%。44.1kは変換の影響、背景25%は原音との混合（遅延の整合）の影響を確認する。
        for (const Config c : { Config { 48000.0, 0.5f }, Config { 44100.0, 0.5f }, Config { 48000.0, 0.25f }, Config { 44100.0, 0.25f } })
        {
            for (const double f0 : { 120.0, 200.0 })
            {
                const int n = (int) (8.0 * c.fs);
                const auto in = vc::test::makeSpeechLikeVowel (f0, c.fs, n, 0.3f);
                const auto out = runNoiseReducer (c.fs, in, c.bg);

                vc::NoiseReducer probe;
                probe.prepare (c.fs, 4096);
                const auto r = compareSpectra (in, out, probe.getDelaySamples(), (int) c.fs, (int) (6.0 * c.fs), c.fs, f0);
                const juce::String label = juce::String (c.fs, 0) + " Hz, background " + juce::String ((int) (c.bg * 100)) + " %, f0 " + juce::String (f0, 0);

                logMessage ("N3a " + label + ": level change " + juce::String (r.levelChangeDb, 2) + " dB, max 1/3-octave diff "
                            + juce::String (r.maxBandDiffDb, 2) + " dB (" + juce::String (r.bandsCompared) + " bands)");
                expect (std::abs (r.levelChangeDb) <= 1.5, label + ": level change " + juce::String (r.levelChangeDb, 2) + " dB");
                expect (r.maxBandDiffDb <= 3.0, label + ": spectrum difference " + juce::String (r.maxBandDiffDb, 2) + " dB");
            }
        }
    }

    // N3a・N4aの前提の確認（測定が意味を持つこと）: RNNoiseは雑音を減らし（雑音のみの入力で-10dB以下）、
    // 背景0%は原音をDサンプル遅らせただけ（ビット一致）になる。
    void runN3aControls()
    {
        beginTest ("N3a controls: white noise is attenuated by RNNoise; background 0 % is the input delayed by D (bit-identical)");

        const int n = (int) (4.0 * kFs);
        const auto noise = makeWhiteNoise (n, 0.05f, 4321);
        const auto out = runNoiseReducer (kFs, noise, 0.5f);
        const double attenuationDb = 20.0 * std::log10 (vc::test::rms (out.data() + (int) (2.0 * kFs), (int) kFs)
                                                        / vc::test::rms (noise.data() + (int) (2.0 * kFs) - kNativeD, (int) kFs));
        logMessage ("N3a control: white noise through RNNoise only: " + juce::String (attenuationDb, 1) + " dB");
        expect (attenuationDb <= -10.0, "white noise attenuation " + juce::String (attenuationDb, 1) + " dB");

        const auto vowel = vc::test::makeSpeechLikeVowel (140.0, kFs, n, 0.3f);
        const auto dry = runNoiseReducer (kFs, vowel, 0.0f);
        expect (std::memcmp (dry.data() + kNativeD + 2000, vowel.data() + 2000, sizeof (float) * (size_t) (n - kNativeD - 2000)) == 0,
                "background 0 % is not the input delayed by D");
    }

    // ----- D-020: バイパスとマイク処理 -----
    void runBypassTests()
    {
        beginTest ("D-020: bypass does not turn noise reduction off or reset it; noise reduction OFF keeps the bypass bit-identical");

        const int n = (int) (2.0 * kFs);
        const auto signal = vc::test::makeSpeechLikeVowel (140.0, kFs, n, 0.3f);

        // バイパス中でもノイズ除去は働く: 背景0%（原音の遅延線）なら、出力 = Dサンプル遅れの入力（|x| <= 1でクリップは何もしない）。
        {
            vc::Engine engine;
            engine.params().enabled.store (false);
            engine.params().nrEnabled.store (true);
            engine.params().nrBackground.store (0.0f);
            engine.prepare ({ kFs, 512 });

            auto out = signal;
            processInBlocks (engine, out, kFrame);
            expect (std::memcmp (out.data() + kNativeD + 2000, signal.data() + 2000, sizeof (float) * (size_t) (n - kNativeD - 2000)) == 0,
                    "bypass output is not the noise-reduced (delayed) signal");
            expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD);
        }

        // バイパスの解除・再投入でノイズ除去をリセットしない（状態がActiveのまま）。
        {
            vc::Engine engine;
            engine.params().nrEnabled.store (true);
            engine.prepare ({ kFs, 512 });

            auto out = signal;
            processInBlocks (engine, out, kFrame, 0, (int) kFs);
            expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD);

            for (const bool enabled : { false, true, false, true })
            {
                engine.params().enabled.store (enabled);
                processInBlocks (engine, out, kFrame, (int) kFs, (int) kFs + 4 * kFrame);
                expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, "noise reduction was reset by a bypass change");
            }
        }

        // ノイズ除去OFFではバイパスは入力とビット一致（E4aと同じ確認を、ノイズ除去を組み込んだEngineで）。
        {
            vc::Engine engine;
            engine.params().enabled.store (false);
            engine.prepare ({ kFs, 512 });

            auto out = signal;
            processInBlocks (engine, out, kFrame);
            expect (std::memcmp (out.data(), signal.data(), sizeof (float) * (size_t) n) == 0, "bypass with noise reduction OFF is not bit-identical");
        }
    }

    // ----- N4a: 低い声（RNNoiseのみ） -----
    void runN4a()
    {
        beginTest ("N4a: low-pitched clean vowels through RNNoise only: fundamental loss <= 3 dB, overall level change <= 1.5 dB");

        for (const double f0 : { 85.0, 100.0, 120.0 })
        {
            const int n = (int) (8.0 * kFs);
            const auto in = vc::test::makeSpeechLikeVowel (f0, kFs, n, 0.3f);
            const auto out = runNoiseReducer (kFs, in, 0.5f);

            const auto r = compareSpectra (in, out, kNativeD, (int) kFs, (int) (6.0 * kFs), kFs, f0);
            const juce::String label = "f0 " + juce::String (f0, 0);

            logMessage ("N4a " + label + ": fundamental change " + juce::String (r.fundamentalChangeDb, 2) + " dB, level change "
                        + juce::String (r.levelChangeDb, 2) + " dB");
            expect (r.fundamentalChangeDb >= -3.0, label + ": fundamental changed by " + juce::String (r.fundamentalChangeDb, 2) + " dB");
            expect (std::abs (r.levelChangeDb) <= 1.5, label + ": level change " + juce::String (r.levelChangeDb, 2) + " dB");
        }
    }

    // ----- N12相当（T-010時点の参考値。失敗判定なし） -----
    void runCpuReference()
    {
        beginTest ("N12 (reference, T-010: mixing only): CPU cost of 10 s of audio at 480-sample blocks (never fails)");

        struct Case { double fs; vc::Preset preset; };

        for (const Case c : { Case { kFs, vc::Preset::Normal }, Case { kFs, vc::Preset::Talkbox }, Case { kFs, vc::Preset::Minion },
                              Case { 44100.0, vc::Preset::Normal } })
        {
            const int n = (int) (10.0 * c.fs);
            auto signal = vc::test::makeSpeechLikeVowel (140.0, c.fs, n, 0.3f);
            const auto pink = vc::test::makePinkNoise (n, 0.03f);
            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += pink[i];

            double percent[2] = { 0.0, 0.0 };
            double maxBlockMs[2] = { 0.0, 0.0 };

            for (int nr = 0; nr < 2; ++nr)
            {
                vc::Engine engine;
                engine.prepare ({ c.fs, 480 });
                engine.params().preset.store ((int) c.preset);
                engine.params().nrEnabled.store (nr == 1);
                engine.params().nrBackground.store (0.7f);

                auto data = signal;
                double totalMs = 0.0;

                for (int pos = 0; pos + 480 <= n; pos += 480)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    engine.process (data.data() + pos, 480);
                    const auto t1 = std::chrono::steady_clock::now();
                    const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
                    totalMs += ms;
                    maxBlockMs[nr] = std::max (maxBlockMs[nr], ms);
                }

                percent[nr] = 100.0 * totalMs / 10000.0;
            }

            logMessage ("N12(ref) " + juce::String (c.fs, 0) + " Hz " + juce::String (vc::kPresets[(size_t) c.preset].id) + ": OFF "
                        + juce::String (percent[0], 3) + " %, noise reduction ON (background 70 %) " + juce::String (percent[1], 3)
                        + " % (increase " + juce::String (percent[1] - percent[0], 3) + " points), max block "
                        + juce::String (maxBlockMs[1], 3) + " ms");
        }
    }
};

static NoiseReducerTests noiseReducerTests;

} // namespace
