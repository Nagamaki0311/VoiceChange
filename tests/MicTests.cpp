#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "AllocationGuard.h"
#include "TestSignals.h"

#include "core/Engine.h"
#include "core/MicProcessing.h"
#include "RecordingTool.h"

#include <rnnoise.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

// ===== SECTION: MicTests =====
// マイク処理のテスト（カテゴリMic、quick）。T-009のN0a〜N0e（RNNoiseそのものの確認）と、
// T-010のNoiseReducer・Engine組み込み（N3a・N4a・N6〜N11。docs/plan.md 8.3）。
// N1・N2・N3b・N4b・N5はT-011、EQ（EQ1〜EQ9）はT-012、実録音比較ツール（tests/RecordingTool.cpp）の指標の検証M1はT-014で追加した。
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
std::vector<float> runNoiseReducer (double fs, const std::vector<float>& in, float bg, int blockSize = 480, float impact = 0.0f)
{
    vc::NoiseReducer nr;
    nr.prepare (fs, 4096);

    std::vector<float> out (in);

    for (int pos = 0; pos < (int) out.size(); pos += blockSize)
    {
        nr.setTarget (true, bg, impact);
        nr.process (out.data() + pos, std::min (blockSize, (int) out.size() - pos));
    }

    return out;
}

// ----- ゲート・インパクト抑制のテストと共有する小さな測定関数 -----

double energyOf (const float* a, int n)
{
    double e = 0.0;

    for (int i = 0; i < n; ++i)
        e += (double) a[i] * (double) a[i];

    return e;
}

double dbOf (double powerRatio)
{
    return 10.0 * std::log10 (std::max (powerRatio, 1.0e-30));
}

void addPink (std::vector<float>& signal, float rmsLevel, int seed = 1)
{
    const auto pink = vc::test::makePinkNoise ((int) signal.size(), rmsLevel, seed);

    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] += pink[i];
}

// inの[from, from + len)と、Dだけ遅れた出力の同じ区間のエネルギーの比[dB]（out/in）。
double energyChangeDb (const std::vector<float>& in, const std::vector<float>& out, int from, int len, int delay = kNativeD)
{
    return dbOf (energyOf (out.data() + from + delay, len) / energyOf (in.data() + from, len));
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
        runN8Huge();
        runN9a();
        runN9a (1.0f, 1.0f);
        runN10();
        runReOnMatchesFresh();
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
        beginTest ("N6: reported latency equals measured delay (+-1 sample) at 48k/44.1k/96k/192k, for several block sizes");

        for (const double fs : { 48000.0, 44100.0, 96000.0, 192000.0 })
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
            // 非有限値の検出でマイク処理もリセットされる（D-020）: Restingへ戻り、報告遅延が0になる。
            expectEquals (engine.getNoiseReducerLatencySamples(), 0, juce::String (c.name) + ": noise reduction was not reset");
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

    // Active中に「有限だが巨大な値」（1e35。RNNoiseの入力の32768倍でinfになり、内部状態がNaNになる）が入るケース。
    // 入力の検査は通り、RNNoiseの出力側で遅れて検出される。マイク処理をリセットしないと、NaNの状態が残って
    // 以後のフレームが毎回非有限になる。合格条件: 無音は数ブロックだけ、フラグは1回、以後有限、Activeへ復帰。
    void runN8Huge()
    {
        beginTest ("N8: a finite huge value (1e35) during Active poisons RNNoise: silenced for a few blocks only, one error, recovers to Active");

        constexpr int block = 480;
        const int n = (int) (6.0 * kFs);
        const auto signal = vc::test::makeSpeechLikeVowel (140.0, kFs, n, 0.3f);

        for (const bool bypass : { false, true })
        {
            const juce::String label = bypass ? "bypass" : "chain";
            vc::Engine engine;
            engine.prepare ({ kFs, 512 });
            engine.params().enabled.store (! bypass);
            engine.params().nrEnabled.store (true);

            auto data = signal;
            int pos = 0;

            for (int b = 0; b < 100; ++b, pos += block)
                engine.process (data.data() + pos, block);

            expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, label + ": not Active before the injection");
            engine.clearErrorFlags();

            data[(size_t) pos + 10] = 1.0e35f;
            int flaggedBlocks = 0;
            int silentBlocks = 0;
            bool allOutputFinite = true;

            for (int b = 0; pos + block <= n; ++b, pos += block)
            {
                engine.process (data.data() + pos, block);
                allOutputFinite = allOutputFinite && vc::test::allFinite (data.data() + pos, block);

                if (b > 0 && vc::test::peakAbs (data.data() + pos, block) <= 0.0)
                    ++silentBlocks;

                if (engine.getErrorFlags() != 0)
                    ++flaggedBlocks;

                engine.clearErrorFlags();
            }

            logMessage ("N8 huge (" + label + "): flagged blocks " + juce::String (flaggedBlocks) + ", silent blocks " + juce::String (silentBlocks));
            expect (allOutputFinite, label + ": non-finite output");
            expectEquals (flaggedBlocks, 1, label + ": the error must be flagged exactly once");
            expect (silentBlocks >= 1 && silentBlocks <= 4, label + ": silent blocks " + juce::String (silentBlocks));
            expectEquals (engine.getNoiseReducerLatencySamples(), kNativeD, label + ": did not return to Active");
        }
    }

    // ----- N9a: 切り替えのクリック -----
    // 200Hz・A=0.3の正弦（-80dBFSの雑音付き）。checkNoClickの区間: 前=切替の直前0.5秒、切替区間=切替から0.4秒、後=切替の1.0秒後から0.5秒。
    struct Step
    {
        int atSample;
        std::function<void (vc::AtomicParams&)> apply;
    };

    float scenarioImpact = 0.0f; // N9aのインパクト（T-011でゲート・インパクトを含めて再実行するため）

    std::vector<float> runScenario (bool nrInitiallyOn, float bgInitial, std::vector<Step> steps, int total,
                                    const std::vector<float>& signal, int blockSize = 480)
    {
        vc::Engine engine;
        engine.prepare ({ kFs, 1024 });
        engine.params().nrEnabled.store (nrInitiallyOn);
        engine.params().nrBackground.store (bgInitial);
        engine.params().nrImpact.store (scenarioImpact);

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

    // nominalBg: 通常のシナリオの背景ノイズ（0.7ならゲートは働く方向、1.0なら最大）。impact: インパクト抑制（T-011）。
    void runN9a (float nominalBg = 0.7f, float impact = 0.0f)
    {
        beginTest ("N9a: no clicks when noise reduction is switched (OFF/ON, abort in Priming, reversal in FadingIn/FadingOut, background 0 <-> 100 %)"
                   + juce::String (impact > 0.0f ? " [with gate at background " + juce::String ((int) (nominalBg * 100)) + " % and impact " + juce::String ((int) (impact * 100)) + " %]" : ""));
        scenarioImpact = impact;

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
            { "OFF -> ON", false, nominalBg, [&] (int t) { return std::vector<Step> { { t, setOn (true) } }; } },
            { "ON -> OFF", true, nominalBg, [&] (int t) { return std::vector<Step> { { t, setOn (false) } }; } },
            { "abort in Priming", false, nominalBg,
              [&] (int t) { return std::vector<Step> { { t, setOn (true) }, { t + 700, setOn (false) } }; } },
            { "reversal in FadingIn", false, nominalBg,
              [&] (int t) { return std::vector<Step> { { t, setOn (true) }, { t + kNativeD + 400, setOn (false) } }; } },
            { "reversal in FadingOut", true, nominalBg,
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

    // ON→OFF（Restingまで）→ONの出力が、新しいインスタンスの初回ONと一致する（Resting→Primingでパイプラインを消去する）。
    void runReOnMatchesFresh()
    {
        beginTest ("N10b: ON -> OFF (to Resting) -> ON gives the same output as a fresh instance's first ON");

        for (const double fs : { 48000.0, 44100.0 })
        {
            const int n = (int) (1.0 * fs);
            auto first = vc::test::makeSpeechLikeVowel (170.0, fs, n, 0.3f);
            auto second = vc::test::makeSpeechLikeVowel (110.0, fs, n, 0.3f);
            const auto pink = vc::test::makePinkNoise (n, 0.05f, 7);
            for (size_t i = 0; i < second.size(); ++i)
                second[i] += pink[i];

            const auto run = [&] (vc::NoiseReducer& nr, std::vector<float>& data, bool on)
            {
                for (int pos = 0; pos < (int) data.size(); pos += 480)
                {
                    nr.setTarget (on, 0.3f, 0.0f);
                    nr.process (data.data() + pos, std::min (480, (int) data.size() - pos));
                }
            };

            vc::NoiseReducer reused;
            reused.prepare (fs, 4096);
            run (reused, first, true);
            auto tail = vc::test::makeSpeechLikeVowel (90.0, fs, (int) (0.2 * fs), 0.3f);
            run (reused, tail, false); // FadingOut → Resting
            expectEquals (reused.getLatencySamples(), 0);

            vc::NoiseReducer fresh;
            fresh.prepare (fs, 4096);

            auto outReused = second;
            auto outFresh = second;
            run (reused, outReused, true);
            run (fresh, outFresh, true);

            expect (std::memcmp (outReused.data(), outFresh.data(), sizeof (float) * (size_t) n) == 0,
                    juce::String (fs, 0) + " Hz: the second ON differs from a fresh instance (pipeline not cleared)");
        }
    }

    // ----- N11: 分割処理 -----
    void runN11()
    {
        beginTest ("N11: mixed block lengths give bit-identical output to one-shot processing; no output FIFO underflow");

        // (a) NoiseReducer単体: 混在列（1, 7, 128, 441, 480, 4096）と、480ごとの処理が一致。複数のレートで枯渇0回。
        // 2通り: 背景30%・インパクト0%（混合のみ）と、背景100%・インパクト70%（ゲート・インパクト抑制。発話・無音・クリックの信号）。
        for (const bool gateMode : { false, true })
        for (const double fs : { 48000.0, 44100.0, 96000.0, 88200.0, 32000.0, 192000.0, 384000.0 })
        {
            if (gateMode && fs > 200000.0)
                continue; // 384kHzは処理が重いので、ゲートの経路はここまで

            const int n = (int) ((gateMode ? 3.0 : 1.5) * fs);
            auto signal = gateMode ? vc::test::makeSpeechAndPauses (n, fs, 0.4, 0.6, 0.3f) : vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.3f);
            // ゲートモードは定常な白色雑音（ピンク雑音は短時間RMSが揺れて、ゲートが閉じるまで時間がかかる）と大きめのクリック。
            const auto noise = gateMode ? makeWhiteNoise (n, 0.005f, 91) : vc::test::makePinkNoise (n, 0.03f);
            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += noise[i];

            if (gateMode)
                for (int k = 0; k < 5; ++k)
                    vc::test::addClick (signal, (int) ((1.0 + 0.4 * k) * fs), fs, 3000.0, 1.0 + k % 2, 0.5f);

            int gateClosed = 0, impactAttenuated = 0;

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
                    nr.setTarget (true, gateMode ? 1.0f : 0.3f, gateMode ? 0.7f : 0.0f); // 背景30%（d = 0.16）: 遅延線の混合も通す
                    nr.process (out.data() + pos, len);
                    pos += len;
                }

                *underflows = nr.getUnderflowCount();
                gateClosed = nr.getGateClosedSampleCount();
                impactAttenuated = nr.getImpactAttenuatedSampleCount();
                return out;
            };

            int underMixed = 0, underRef = 0, underOne = 0;
            const auto mixed = runSequence (std::vector<int> (std::begin (kMixedBlocks), std::end (kMixedBlocks)), &underMixed);
            const auto ref = runSequence ({ 480 }, &underRef);
            const int refGateClosed = gateClosed, refImpactAttenuated = impactAttenuated;
            const auto oneSample = runSequence ({ 1 }, &underOne);

            const juce::String label = juce::String (fs, 0) + " Hz" + (gateMode ? " (gate/impact)" : "");
            expect (std::memcmp (mixed.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": mixed blocks differ from 480-blocks");
            expect (std::memcmp (oneSample.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": 1-sample blocks differ from 480-blocks");
            expectEquals (underMixed + underRef + underOne, 0, label + ": output FIFO underflow");

            if (gateMode)
            {
                expect (refGateClosed > 0, label + ": control: the gate never closed");
                expect (refImpactAttenuated > 0, label + ": control: the impact stage never attenuated");
                expectEquals (gateClosed, refGateClosed, label + ": gate-closed sample count depends on the block length");
                expectEquals (impactAttenuated, refImpactAttenuated, label + ": impact-attenuated sample count depends on the block length");
            }

            // 端数の多いブロック長でも枯れず、結果が変わらない（高いレートでは端数の持ち越しが大きい）。
            for (const int oddBlock : { 2, 7, 333, 1000 })
            {
                int underOdd = 0;
                const auto odd = runSequence ({ oddBlock }, &underOdd);
                expectEquals (underOdd, 0, label + ": output FIFO underflow with block " + juce::String (oddBlock));
                expect (std::memcmp (odd.data(), ref.data(), sizeof (float) * (size_t) n) == 0,
                        label + ": block " + juce::String (oddBlock) + " differs from 480-blocks");
            }
            expect (vc::test::allFinite (ref.data(), n), label + ": non-finite output");
        }

        // (b) Engine全体: n = 3 * maxBlock + 17 の一括処理（内部でmaxBlockごとに分割）と、混在列。44.1kも含む。ゲート・インパクトあり（背景100%・インパクト70%）も。
        for (const bool gateMode : { false, true })
        for (const double fs : { 48000.0, 44100.0 })
        {
            constexpr int maxBlock = 4096;
            const int n = 3 * maxBlock + 17;
            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.3f);

            if (gateMode)
            {
                signal = vc::test::makeSpeechAndPauses (n, fs, 0.06, 0.06, 0.3f);
                addPink (signal, 0.01f, 5);
                vc::test::addClick (signal, (int) (0.09 * fs), fs, 3000.0, 1.0, 0.2f);
            }

            const auto runEngine = [&] (bool oneShot)
            {
                vc::Engine engine;
                engine.prepare ({ fs, maxBlock });
                engine.params().nrEnabled.store (true);
                engine.params().nrBackground.store (gateMode ? 1.0f : 0.3f);
                engine.params().nrImpact.store (gateMode ? 0.7f : 0.0f);

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
                    "Engine at " + juce::String (fs, 0) + " Hz" + (gateMode ? " (gate/impact)" : "") + ": one-shot and mixed blocks differ");
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

    // ----- N12（参考値。失敗判定なし） -----
    // 48kHz・480ブロック・10秒で、ノイズ除去OFF / ON（背景70%・インパクト0%）/ ON（背景70%・インパクト50%）の処理時間÷音声時間。
    // EQはOFF（EQを含む測定はT-012のEQ9）。2回測って小さい方を採る。ON・インパクト0%とOFFの差がT-010（混合まで）に当たり、
    // インパクト50%との差がゲート・インパクト抑制の追加分。増分が3ポイントを超えたら報告する。
    void runCpuReference()
    {
        beginTest ("N12 (reference): CPU cost of 10 s of audio at 480-sample blocks: OFF / noise reduction ON (background 70 %) / ON with impact 50 % (never fails)");

        struct Case { double fs; vc::Preset preset; };

        for (const Case c : { Case { kFs, vc::Preset::Normal }, Case { kFs, vc::Preset::Talkbox }, Case { kFs, vc::Preset::Minion },
                              Case { 44100.0, vc::Preset::Normal } })
        {
            const int n = (int) (10.0 * c.fs);
            auto signal = vc::test::makeSpeechLikeVowel (140.0, c.fs, n, 0.3f);
            const auto pink = vc::test::makePinkNoise (n, 0.03f);
            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += pink[i];

            constexpr int modes = 3; // 0: OFF, 1: ON、インパクト0%, 2: ON、インパクト50%
            double percent[modes] = { 1.0e9, 1.0e9, 1.0e9 };
            double maxBlockMs[modes] = { 0.0, 0.0, 0.0 };

            for (int repeat = 0; repeat < 2; ++repeat)
            {
                for (int mode = 0; mode < modes; ++mode)
                {
                    vc::Engine engine;
                    engine.prepare ({ c.fs, 480 });
                    engine.params().preset.store ((int) c.preset);
                    engine.params().nrEnabled.store (mode >= 1);
                    engine.params().nrBackground.store (0.7f);
                    engine.params().nrImpact.store (mode == 2 ? 0.5f : 0.0f);

                    auto data = signal;
                    double totalMs = 0.0;
                    double maxMs = 0.0;

                    for (int pos = 0; pos + 480 <= n; pos += 480)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        engine.process (data.data() + pos, 480);
                        const auto t1 = std::chrono::steady_clock::now();
                        const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
                        totalMs += ms;
                        maxMs = std::max (maxMs, ms);
                    }

                    percent[mode] = std::min (percent[mode], 100.0 * totalMs / 10000.0);
                    maxBlockMs[mode] = maxMs;
                }
            }

            logMessage ("N12 " + juce::String (c.fs, 0) + " Hz " + juce::String (vc::kPresets[(size_t) c.preset].id) + ": OFF "
                        + juce::String (percent[0], 3) + " %, noise reduction ON (background 70 %) " + juce::String (percent[1], 3)
                        + " % (increase " + juce::String (percent[1] - percent[0], 3) + " points), with impact 50 % " + juce::String (percent[2], 3)
                        + " % (increase " + juce::String (percent[2] - percent[0], 3) + " points, of which gate/impact stage "
                        + juce::String (percent[2] - percent[1], 3) + " points), max block " + juce::String (maxBlockMs[2], 3) + " ms");
        }
    }
};

static NoiseReducerTests noiseReducerTests;

// ===== SECTION: ゲート・インパクト抑制（T-011） =====
// N1・N2・N3b・N4b・N5a・N5b（客観指標。閾値は暫定で、外れたら緩めずに実測値を報告する）、N7・N9bのゲート・インパクト版、N12。
// 時間の基準: 入力の時刻tの音は、出力の時刻t + D（D = kNativeD）に現れる。

class GateImpactTests final : public juce::UnitTest
{
public:
    GateImpactTests() : juce::UnitTest ("GateImpact", "Mic") {}

    void runTest() override
    {
        runN1();
        runGatePhase();
        runN2();
        runN2Gate();
        runN3b();
        runN3bSyllables();
        runGateOnset();
        runGateRmsConditions();
        runN4b();
        runN5a();
        runN5b();
        runN5bBitExact();
        runN7Gate();
        runN9b();
    }

private:
    // ----- N1: SNR改善量 -----
    // 発話1秒・無音1秒の繰り返しにピンク雑音をSNR 10dB（発話区間のRMS比）で重ね、背景ノイズ70%。
    // 区間 = 入力の1〜7秒。出力SNR = 清音（Dだけ遅らせたもの）のエネルギー / (出力 - 遅延させた清音)のエネルギー。
    // 入力SNR = 清音のエネルギー / 雑音のエネルギー。発話・無音の両方を含む区間全体の改善量が主条件で、発話区間だけの値も記録する。
    void runN1()
    {
        beginTest ("N1: SNR improvement >= 6 dB (speech 1 s / pause 1 s, pink noise at 10 dB SNR, background 70 %)");

        const int n = (int) (8.0 * kFs);
        std::vector<char> isSpeech;
        const auto clean = vc::test::makeSpeechAndPauses (n, kFs, 1.0, 1.0, 0.3f, &isSpeech);

        double speechEnergy = 0.0;
        int speechCount = 0;

        for (int i = 0; i < n; ++i)
            if (isSpeech[(size_t) i] != 0)
            {
                speechEnergy += (double) clean[(size_t) i] * clean[(size_t) i];
                ++speechCount;
            }

        const double speechRms = std::sqrt (speechEnergy / speechCount);
        const auto noise = vc::test::makePinkNoise (n, (float) (speechRms * std::pow (10.0, -10.0 / 20.0)), 11);
        auto in = clean;

        for (int i = 0; i < n; ++i)
            in[(size_t) i] += noise[(size_t) i];

        const auto out = runNoiseReducer (kFs, in, 0.7f);
        const int from = (int) kFs;
        const int to = (int) (7.0 * kFs);

        for (const bool speechOnly : { false, true })
        {
            double cleanE = 0.0, noiseE = 0.0, errE = 0.0;

            for (int t = from; t < to; ++t)
            {
                if (speechOnly && isSpeech[(size_t) t] == 0)
                    continue;

                const double c = clean[(size_t) t];
                const double e = (double) out[(size_t) (t + kNativeD)] - c;
                cleanE += c * c;
                noiseE += (double) noise[(size_t) t] * noise[(size_t) t];
                errE += e * e;
            }

            const double snrIn = dbOf (cleanE / noiseE);
            const double snrOut = dbOf (cleanE / errE);
            logMessage ("N1 " + juce::String (speechOnly ? "speech only" : "whole region") + ": input SNR " + juce::String (snrIn, 2)
                        + " dB, output SNR " + juce::String (snrOut, 2) + " dB, improvement " + juce::String (snrOut - snrIn, 2) + " dB");

            if (! speechOnly)
                expect (snrOut - snrIn >= 6.0, "SNR improvement " + juce::String (snrOut - snrIn, 2) + " dB (< 6 dB)");
        }
    }

    // ----- VADの位相: ゲートは発話とともに開き、発話の間は閉じない -----
    // N1と同じ信号（発話1秒・無音1秒、SNR 10dB）で、背景ノイズ100%（ゲートあり）と50%（RNNoiseのみ）を比べる。
    // (1) 発話区間（出力の時刻。冒頭50msと末尾30msを除く）でゲートが閉じたサンプルが0、(2) 各発話の冒頭50msのエネルギーが50%比で-1dB以内
    // （VADが出力より遅れると、開くのが遅れて冒頭が削れる）、(3) 無音区間（発話の終わりから0.3秒以降）でゲートが閉じたサンプルが1割以上。
    void runGatePhase()
    {
        beginTest ("Gate phase: the gate opens with the speech (no closed samples inside speech, onset energy within -1 dB) and closes in the pauses");

        const int n = (int) (8.0 * kFs);
        std::vector<char> isSpeech;
        const auto clean = vc::test::makeSpeechAndPauses (n, kFs, 1.0, 1.0, 0.3f, &isSpeech);
        double speechEnergy = 0.0;
        int speechCount = 0;

        for (int i = 0; i < n; ++i)
            if (isSpeech[(size_t) i] != 0)
            {
                speechEnergy += (double) clean[(size_t) i] * clean[(size_t) i];
                ++speechCount;
            }

        const auto noise = vc::test::makePinkNoise (n, (float) (std::sqrt (speechEnergy / speechCount) * std::pow (10.0, -10.0 / 20.0)), 11);
        auto in = clean;

        for (int i = 0; i < n; ++i)
            in[(size_t) i] += noise[(size_t) i];

        vc::NoiseReducer nr;
        nr.prepare (kFs, 4096);
        auto gated = in;
        std::vector<int> closedAfterBlock;

        for (int pos = 0; pos < n; pos += kFrame)
        {
            nr.setTarget (true, 1.0f, 0.0f);
            nr.process (gated.data() + pos, kFrame);
            closedAfterBlock.push_back (nr.getGateClosedSampleCount());
        }

        const auto ungated = runNoiseReducer (kFs, in, 0.5f);
        const auto closedBetween = [&] (int from, int to) // 出力の時刻[from, to)。ブロック境界（480の倍数）で数える
        {
            const auto at = [&] (int t) { return t <= 0 ? 0 : closedAfterBlock[(size_t) std::min ((int) closedAfterBlock.size(), t / kFrame) - 1]; };
            return at (to) - at (from);
        };

        int closedInSpeech = 0;
        double worstOnsetDb = 1.0e9;

        for (int seg = 0; seg < 4; ++seg)
        {
            const int s = seg * 2 * (int) kFs;
            const int e = s + (int) kFs;
            const int outFrom = ((s + kNativeD + (int) (0.05 * kFs) + kFrame - 1) / kFrame) * kFrame;
            const int outTo = ((e + kNativeD - (int) (0.03 * kFs)) / kFrame) * kFrame;

            closedInSpeech += closedBetween (outFrom, outTo);
            worstOnsetDb = std::min (worstOnsetDb, dbOf (energyOf (gated.data() + s + kNativeD, (int) (0.05 * kFs))
                                                         / energyOf (ungated.data() + s + kNativeD, (int) (0.05 * kFs))));
        }

        // 無音区間（発話の終わりの0.3秒後〜次の発話の冒頭）。ホールド200msと雑音床+8dB未満の条件があり、ピンク雑音は短時間RMSの揺れが大きく、
        // 閉じるまでに1秒前後かかるため、閉じたサンプルが1割以上あることだけを見る（ゲートが全く閉じない不具合の検出用）。
        int closedInPauses = 0, pauseSamples = 0;

        for (int seg = 0; seg < 3; ++seg)
        {
            const int from = ((seg * 2 + 1) * (int) kFs + (int) (0.3 * kFs) + kNativeD) / kFrame * kFrame;
            const int to = ((seg * 2 + 2) * (int) kFs + kNativeD) / kFrame * kFrame;
            closedInPauses += closedBetween (from, to);
            pauseSamples += to - from;
        }

        logMessage ("Gate phase: closed samples inside speech " + juce::String (closedInSpeech) + ", worst onset (first 50 ms) energy vs background 50 % "
                    + juce::String (worstOnsetDb, 2) + " dB, closed in the pauses (after 0.3 s) " + juce::String (closedInPauses) + " of " + juce::String (pauseSamples));
        expectEquals (closedInSpeech, 0, "gate closed inside speech");
        expect (worstOnsetDb >= -1.0, "speech onset attenuated by the gate: " + juce::String (worstOnsetDb, 2) + " dB");
        expect (closedInPauses * 10 >= pauseSamples, "the gate did not close in the pauses: " + juce::String (closedInPauses) + " of " + juce::String (pauseSamples));
    }

    // ----- N2: 無声区間の残留雑音 -----
    // 雑音だけ（ピンク雑音、-30dBFS）。開始1秒以降の出力RMSが入力の雑音比で、50%で-12dB以下、100%で-25dB以下。
    // 0/25/50/75/100%で残留量が単調に減る。ほかのレベルは記録のみ。
    void runN2()
    {
        beginTest ("N2: residual noise in pauses: <= -12 dB at 50 %, <= -25 dB at 100 %, monotonically decreasing over 0/25/50/75/100 %");

        const int n = (int) (7.0 * kFs);
        const int from = (int) kFs;
        const int len = (int) (5.0 * kFs);

        for (const float level : { 0.03f, 0.1f, 0.003f })
        {
            const auto noise = vc::test::makePinkNoise (n, level, 21);
            double previous = 1.0e9;
            juce::String line = "N2 pink noise " + juce::String (20.0 * std::log10 (level), 1) + " dBFS: residual";

            for (const float bg : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const auto out = runNoiseReducer (kFs, noise, bg);
                const double db = 20.0 * std::log10 (vc::test::rms (out.data() + from + kNativeD, len) / vc::test::rms (noise.data() + from, len));
                line << " " << juce::String ((int) (bg * 100)) << "%: " << juce::String (db, 1) << " dB";

                if (level > 0.01f && level < 0.05f) // 主条件（-30dBFS）
                {
                    expect (db < previous, "residual is not monotonically decreasing at background " + juce::String ((int) (bg * 100)) + " %: " + juce::String (db, 2) + " dB");
                    previous = db;

                    if (bg == 0.5f)
                        expect (db <= -12.0, "background 50 %: residual " + juce::String (db, 2) + " dB (> -12 dB)");

                    if (bg == 1.0f)
                        expect (db <= -25.0, "background 100 %: residual " + juce::String (db, 2) + " dB (> -25 dB)");
                }
            }

            logMessage (line);
        }
    }

    // ----- N3b: 発話区間の歪み（背景ノイズ100%） -----
    // 雑音のない合成母音（f0 = 120/200Hz）。レベル変化 <= 1.5dB。有声区間（冒頭50msを除く）でゲートが閉じたサンプルが0。
    void runN3b()
    {
        beginTest ("N3b: clean vowel at background 100 %: level change <= 1.5 dB and the gate never closes in the voiced part (after the first 50 ms)");

        for (const double f0 : { 120.0, 200.0 })
        {
            const int n = (int) (8.0 * kFs);
            const auto in = vc::test::makeSpeechLikeVowel (f0, kFs, n, 0.3f);

            vc::NoiseReducer nr;
            nr.prepare (kFs, 4096);
            auto out = in;
            const int countFrom = kNativeD + (int) (0.05 * kFs); // 出力の時刻。入力の冒頭が出力に届くのがD、そこから50ms
            int closedAtStart = 0;

            for (int pos = 0; pos < n;)
            {
                int len = std::min (kFrame, n - pos);

                if (pos < countFrom && pos + len > countFrom)
                    len = countFrom - pos;

                nr.setTarget (true, 1.0f, 0.0f);
                nr.process (out.data() + pos, len);
                pos += len;

                if (pos == countFrom)
                    closedAtStart = nr.getGateClosedSampleCount();
            }

            const int closedInVoiced = nr.getGateClosedSampleCount() - closedAtStart;
            const auto r = compareSpectra (in, out, kNativeD, (int) kFs, (int) (6.0 * kFs), kFs, f0);
            const juce::String label = "f0 " + juce::String (f0, 0);

            logMessage ("N3b " + label + ": level change " + juce::String (r.levelChangeDb, 2) + " dB, gate-closed samples in the voiced part "
                        + juce::String (closedInVoiced) + " (before the 50 ms point: " + juce::String (closedAtStart) + ")");
            expect (std::abs (r.levelChangeDb) <= 1.5, label + ": level change " + juce::String (r.levelChangeDb, 2) + " dB");
            expectEquals (closedInVoiced, 0, label + ": gate-closed samples in the voiced part");
        }
    }

    // ----- N3b（音節の連続）: 短い無声の隙間でゲートが閉じない -----
    // 音節（0.3〜0.7秒）を50msの隙間で並べた連続の発話（makeSyllables。隙間ではVADが下がりうる）に、-60dBFSの白色雑音。
    // ホールド（200ms）が隙間をまたぐこと: 開始1秒以降にゲートが閉じたサンプルが0、背景50%（ゲートなし）比のレベル変化が1dB以内。
    void runN3bSyllables()
    {
        beginTest ("N3b (syllable train): the gate does not close across 50 ms gaps inside an utterance (hold 200 ms)");

        const int n = (int) (8.0 * kFs);
        auto in = vc::test::makeSyllables (n);
        const double peak = vc::test::peakAbs (in.data(), n);

        for (auto& x : in)
            x = (float) ((double) x * 0.3 / peak);

        const auto noise = makeWhiteNoise (n, 0.0017f, 5);

        for (int i = 0; i < n; ++i)
            in[(size_t) i] += noise[(size_t) i];

        vc::NoiseReducer nr;
        nr.prepare (kFs, 4096);
        auto out = in;
        const int countFrom = (int) kFs + kNativeD; // 出力の時刻
        int closedAtStart = 0;

        for (int pos = 0; pos < n;)
        {
            int len = std::min (kFrame, n - pos);

            if (pos < countFrom && pos + len > countFrom)
                len = countFrom - pos;

            nr.setTarget (true, 1.0f, 0.0f);
            nr.process (out.data() + pos, len);
            pos += len;

            if (pos == countFrom)
                closedAtStart = nr.getGateClosedSampleCount();
        }

        const int closed = nr.getGateClosedSampleCount() - closedAtStart;
        const auto ungated = runNoiseReducer (kFs, in, 0.5f);
        const double diffDb = dbOf (energyOf (out.data() + (int) kFs + kNativeD, (int) (6.0 * kFs)) / energyOf (ungated.data() + (int) kFs + kNativeD, (int) (6.0 * kFs)));

        logMessage ("N3b syllable train: gate-closed samples after 1 s " + juce::String (closed) + ", level change vs background 50 % " + juce::String (diffDb, 2) + " dB");
        expectEquals (closed, 0, "gate closed inside the utterance");
        expect (diffDb >= -1.0, "the gate removed " + juce::String (-diffDb, 2) + " dB of the utterance");
    }

    // ----- ゲートは発話の音が出力に届いた時点で開いている（VADの位相） -----
    // 発話0.5秒・無音1.5秒（デジタル無音でなく-70dBFSの白色雑音）を繰り返し、背景100%。RNNoiseのVADは出力より約1フレーム早く上がるため、
    // 発話が出力へ届く時刻（入力の発話開始 + D）から50msの間にゲートが閉じたサンプルが0であること（VADが遅れると、原音のRMSで開くまでの間［数ms］閉じたままになる）。
    void runGateOnset()
    {
        beginTest ("Gate onset: the gate is already open when the speech reaches the output (VAD phase), and it closed in the pause before");

        const int n = (int) (10.0 * kFs);
        std::vector<float> in ((size_t) n, 0.0f);

        for (int k = 0; k < 4; ++k)
            vc::test::addEnvelopedVowel (in, (int) ((1.0 + 2.0 * k) * kFs), k % 2 == 0 ? 120.0 : 170.0, kFs, 0.3f, 10.0, 470.0, 3.0, 20.0);

        {
            const auto noise = makeWhiteNoise (n, (float) (std::pow (10.0, -70.0 / 20.0) * std::sqrt (3.0)), 13); // RMS -70dBFS。定常（ピンクはゲートが閉じるまで時間がかかる）
            for (int i = 0; i < n; ++i)
                in[(size_t) i] += noise[(size_t) i];
        }

        vc::NoiseReducer nr;
        nr.prepare (kFs, 4096);
        auto out = in;
        std::vector<int> cut; // 数える区間の境界（出力の時刻）: 各発話について onset+D、onset+D+50ms、その直前（onset+D-100ms）

        for (int k = 0; k < 4; ++k)
        {
            const int t = (int) ((1.0 + 2.0 * k) * kFs) + kNativeD;
            cut.push_back (t - (int) (0.1 * kFs));
            cut.push_back (t);
            cut.push_back (t + (int) (0.05 * kFs));
        }

        std::vector<int> closedAt;
        size_t next = 0;

        for (int pos = 0; pos < n;)
        {
            int len = std::min (kFrame, n - pos);

            if (next < cut.size() && pos + len > cut[next])
                len = cut[next] - pos;

            nr.setTarget (true, 1.0f, 0.0f);
            nr.process (out.data() + pos, len);
            pos += len;

            if (next < cut.size() && pos == cut[next])
            {
                closedAt.push_back (nr.getGateClosedSampleCount());
                ++next;
            }
        }

        juce::String line;
        int worstOnset = 0;
        int minBefore = 1 << 30;
        int maxBefore = 0;

        for (int k = 0; k < 4; ++k)
        {
            const int inPauseBefore = closedAt[(size_t) (3 * k + 1)] - closedAt[(size_t) (3 * k)]; // onset手前の100ms
            const int atOnset = closedAt[(size_t) (3 * k + 2)] - closedAt[(size_t) (3 * k + 1)];   // onset+D から50ms
            worstOnset = std::max (worstOnset, atOnset);

            if (k > 0) // 最初の発話の前はゲートの学習中
                minBefore = std::min (minBefore, inPauseBefore);

            maxBefore = std::max (maxBefore, inPauseBefore);

            line << " " << juce::String (inPauseBefore) << "/" << juce::String (atOnset);
        }

        logMessage ("Gate onset: gate-closed samples [100 ms before the speech reaches the output / first 50 ms after], per utterance:" + line);
        expectEquals (worstOnset, 0, "the gate was closed while the speech was arriving");
        expect (minBefore > 0, "control: the gate was not closed in the pause before the speech (nothing to open)");
        // VADは出力より約1フレーム（480サンプル）早く上がる。出力と同じ呼び出しのVADを同じ位置へ対応づけていれば、発話が届く5ms（240サンプル）以上前に
        // 開いている。1フレーム遅れて対応づけると、原音のRMSで開く発話の到着時点まで閉じたまま（手前の100msがすべて閉じたサンプルになる）。
        expect (maxBefore <= (int) (0.1 * kFs) - (int) (0.005 * kFs), "the gate opened only when the speech arrived (VAD is not in phase with the output): closed samples in the 100 ms before "
                                                                        + juce::String (maxBefore));
    }

    // ----- ゲートの開閉条件（原音のRMS） -----
    // 白色雑音（-60dBFS）を4秒（ゲートが閉じる）→ 白色雑音のバースト0.3秒（-30dBFS、床+30dB。非発話なのでVADは上がらない）→ 白色雑音-50dBFS（床+10dB）を2秒。
    // (1) バースト（到着から20ms後〜終わり）でゲートが開いている（VADが上がらなくても、RMSが床より12dB以上大きければ開く）。
    // (2) その後、雑音が床+10dB（閉じる側の+8dBと開く側の+12dBの間）のあいだは開いたまま（閉じる条件は「床+8dB未満」なので、+10dBでは閉じない）。
    void runGateRmsConditions()
    {
        beginTest ("Gate RMS conditions: opens on a loud non-speech burst (RMS >= floor + 12 dB) and stays open while the noise is floor + 10 dB (> floor + 8 dB)");

        const int n = (int) (6.3 * kFs);
        auto in = makeWhiteNoise (n, (float) (std::pow (10.0, -60.0 / 20.0) * std::sqrt (3.0)), 23);
        const int burstStart = (int) (4.0 * kFs);
        const int burstLen = (int) (0.3 * kFs);

        {
            const auto loud = makeWhiteNoise (n, (float) (std::pow (10.0, -50.0 / 20.0) * std::sqrt (3.0)), 29);
            const auto tone = makeWhiteNoise (burstLen, (float) (std::pow (10.0, -30.0 / 20.0) * std::sqrt (3.0)), 31); // 非発話の広帯域雑音のバースト（RNNoiseはVADを上げない）

            for (int i = burstStart + burstLen; i < n; ++i)
                in[(size_t) i] = loud[(size_t) i]; // 床+8dB

            for (int i = 0; i < burstLen; ++i)
                in[(size_t) (burstStart + i)] = tone[(size_t) i];
        }

        vc::NoiseReducer nr;
        nr.prepare (kFs, 4096);
        auto out = in;
        const int cuts[] = { burstStart + kNativeD - (int) (0.5 * kFs), burstStart + kNativeD, burstStart + kNativeD + (int) (0.02 * kFs),
                             burstStart + burstLen + kNativeD, burstStart + burstLen + kNativeD + (int) (0.1 * kFs), n };
        int closedAt[6] = {};
        size_t next = 0;

        for (int pos = 0; pos < n;)
        {
            int len = std::min (kFrame, n - pos);

            if (next < 6 && pos + len > cuts[next])
                len = cuts[next] - pos;

            nr.setTarget (true, 1.0f, 0.0f);
            nr.process (out.data() + pos, len);
            pos += len;

            if (next < 6 && pos == cuts[next])
                closedAt[next++] = nr.getGateClosedSampleCount();
        }

        const int closedBeforeBurst = closedAt[1] - closedAt[0];    // バースト手前の0.5秒（閉じているはず）
        const int closedInBurst = closedAt[3] - closedAt[2];         // 到着から20ms後〜バーストの終わり
        const int closedAfterBurst = nr.getGateClosedSampleCount() - closedAt[4]; // バースト終了0.1秒後〜

        logMessage ("Gate RMS conditions: closed samples in the 0.5 s before the burst " + juce::String (closedBeforeBurst) + ", in the burst (after 20 ms) "
                    + juce::String (closedInBurst) + ", after the burst (floor + 10 dB noise, from 0.1 s) " + juce::String (closedAfterBurst));
        expect (closedBeforeBurst > (int) (0.25 * kFs), "control: the gate was not closed in the noise before the burst");
        expectEquals (closedInBurst, 0, "the gate did not open on a loud non-speech burst");
        expectEquals (closedAfterBurst, 0, "the gate closed while the noise was only floor + 10 dB");
    }

    // ----- N2（ゲートの効果）: 発話の後の無音で、背景100%の残留雑音が50%（RNNoiseのみ）より小さい -----
    // 発話1秒・無音2秒（SNR 10dBの定常な白色雑音）。無音の0.7秒後から次の発話の0.1秒前まで、背景100%の出力エネルギーが50%比で-12dB以下
    // （ゲートが閉じたときの減衰量は18dB。ゲートは閉じるのに150msかかる）。
    void runN2Gate()
    {
        beginTest ("N2 (gate effect): in the pauses after speech the residual at background 100 % is >= 12 dB below background 50 %");

        const int n = (int) (10.0 * kFs);
        std::vector<char> isSpeech;
        const auto clean = vc::test::makeSpeechAndPauses (n, kFs, 1.0, 2.0, 0.3f, &isSpeech);
        double speechEnergy = 0.0;
        int speechCount = 0;

        for (int i = 0; i < n; ++i)
            if (isSpeech[(size_t) i] != 0)
            {
                speechEnergy += (double) clean[(size_t) i] * clean[(size_t) i];
                ++speechCount;
            }

        // 白色雑音（一様乱数）のRMS = amp / sqrt(3)
        const double noiseRms = std::sqrt (speechEnergy / speechCount) * std::pow (10.0, -10.0 / 20.0);
        const auto noise = makeWhiteNoise (n, (float) (noiseRms * std::sqrt (3.0)), 17);
        auto in = clean;

        for (int i = 0; i < n; ++i)
            in[(size_t) i] += noise[(size_t) i];

        const auto gated = runNoiseReducer (kFs, in, 1.0f);
        const auto ungated = runNoiseReducer (kFs, in, 0.5f);
        double worst = -1.0e9;
        juce::String line;

        for (int seg = 0; seg < 3; ++seg)
        {
            const int from = (seg * 3 + 1) * (int) kFs + (int) (0.7 * kFs) + kNativeD;
            const int len = (int) (1.2 * kFs);
            const double db = dbOf (energyOf (gated.data() + from, len) / energyOf (ungated.data() + from, len));
            worst = std::max (worst, db);
            line << " " << juce::String (db, 1);
        }

        logMessage ("N2 gate effect (background 100 % vs 50 % in the pauses) [dB]:" + line + " (limit -12)");
        expect (worst <= -12.0, "gate effect in the pauses only " + juce::String (worst, 2) + " dB (> -12 dB)");
    }

    // ----- N4b: 低い声・語尾 -----
    // f0 = 85/100/120Hzの母音（立ち上がり10ms・600ms持続）に、時定数150msの指数減衰の語尾（600ms）。語尾の後は無音（-65dBFSのピンク雑音のみ）。
    // 減衰開始から100msのエネルギーが背景ノイズ0%（= 原音をDサンプル遅らせたもの）比で-3dB以内。
    // RNNoiseは開始から数秒の間、同じ合成母音を雑音として強く抑える（学習の慣らし。N3aも開始1秒以降で測る）ため、
    // 最初の3回の発声は慣らしとして測らず、続く5回の発声のうち最悪のもので判定する。
    void runN4b()
    {
        beginTest ("N4b: low voices with a 150 ms exponential tail: energy of the first 100 ms of the tail within -3 dB of background 0 %");

        constexpr double cycleSeconds = 2.5;
        constexpr int cycles = 8;
        constexpr int warmupCycles = 3;
        const int cycle = (int) (cycleSeconds * kFs);
        const int n = cycle * cycles;
        const int tailStartOffset = (int) (0.610 * kFs); // 立ち上がり10ms + 持続600ms

        struct Config { float bg; float impact; };

        for (const double f0 : { 85.0, 100.0, 120.0 })
        {
            std::vector<float> in ((size_t) n, 0.0f);

            for (int k = 0; k < cycles; ++k)
                vc::test::addEnvelopedVowel (in, k * cycle + (int) (0.3 * kFs), f0, kFs, 0.3f, 10.0, 600.0, 150.0, 600.0);

            addPink (in, (float) std::pow (10.0, -65.0 / 20.0), 31);

            const auto ungated = runNoiseReducer (kFs, in, 0.5f, kFrame, 0.0f); // RNNoiseのみ（ゲートなし）。ゲートだけの影響を切り分ける

            for (const Config c : { Config { 1.0f, 0.0f }, Config { 1.0f, 1.0f }, Config { 0.5f, 0.0f } })
            {
                const auto out = runNoiseReducer (kFs, in, c.bg, kFrame, c.impact);
                double worst = 1.0e9;
                double sumIn = 0.0, sumOut = 0.0;
                double worstLong = 1.0e9;     // 語尾の最初の400ms（仕様の100msより長い窓）の、背景0%比
                double worstGateOnly = 1.0e9; // 同じ窓の、ゲートなし（背景50%）比。ゲートが語尾を削らないこと

                for (int k = warmupCycles; k < cycles; ++k)
                {
                    const int from = k * cycle + (int) (0.3 * kFs) + tailStartOffset;
                    const int len = (int) (0.1 * kFs);
                    worst = std::min (worst, energyChangeDb (in, out, from, len));
                    const int longLen = (int) (0.4 * kFs);
                    worstLong = std::min (worstLong, energyChangeDb (in, out, from, longLen));
                    worstGateOnly = std::min (worstGateOnly, dbOf (energyOf (out.data() + from + kNativeD, longLen) / energyOf (ungated.data() + from + kNativeD, longLen)));
                    sumIn += energyOf (in.data() + from, len);
                    sumOut += energyOf (out.data() + from + kNativeD, len);
                }

                const juce::String label = "f0 " + juce::String (f0, 0) + ", background " + juce::String ((int) (c.bg * 100)) + " %, impact "
                                           + juce::String ((int) (c.impact * 100)) + " %";
                logMessage ("N4b " + label + ": worst of 5 utterances " + juce::String (worst, 2) + " dB, total " + juce::String (dbOf (sumOut / sumIn), 2)
                            + " dB; first 400 ms of the tail: vs background 0 % " + juce::String (worstLong, 2) + " dB, vs background 50 % (gate effect only) "
                            + juce::String (worstGateOnly, 2) + " dB");

                if (c.bg > 0.9f)
                {
                    expect (worst >= -3.0, label + ": tail energy changed by " + juce::String (worst, 2) + " dB");
                    expect (worstLong >= -3.0, label + ": first 400 ms of the tail changed by " + juce::String (worstLong, 2) + " dB");

                    if (c.impact < 0.5f)
                        expect (worstGateOnly >= -1.0, label + ": the gate removed " + juce::String (-worstGateOnly, 2) + " dB of the tail");
                }
            }
        }
    }

    // ----- N5a: クリック除去量 -----
    // -50dBFSのピンク雑音の上に、3kHzの1ms・5ms減衰のクリック（ピーク-20dBFS）を3回ずつ。インパクト100%で、クリック区間（出力の時刻で
    // クリックの開始から30ms）のピークがインパクト0%比で-12dB以下（最悪のクリックで判定）。背景ノイズは仕様に指定がなく、既定の70%で判定する。
    // 0%・100%は参考値（記録のみ）: 100%ではゲートが閉じていて0%側のクリックが既に約18dB下がっているため比が縮み、
    // 0%（ゲートなし）はゲートの状態（発話中とみなされると減衰量が半分になる）の影響だけが残る。
    // -12.0dBはゲートが開いたときの減衰量（24dBの半分）にちょうど当たるため、ゲートが開いているクリックがあると判定はぎりぎりになる。
    void runN5a()
    {
        const int n = (int) (8.0 * kFs);
        std::vector<float> in = vc::test::makePinkNoise (n, (float) std::pow (10.0, -50.0 / 20.0), 41);
        const int clickTimes[] = { 1500, 2500, 3500, 4500, 5500, 6500 }; // ms
        const int region = (int) (0.030 * kFs);

        for (int k = 0; k < 6; ++k)
            vc::test::addClick (in, (int) (clickTimes[k] * 1.0e-3 * kFs), kFs, 3000.0, k < 3 ? 1.0 : 5.0, 0.1f);

        for (const float bg : { 0.7f, 0.0f, 1.0f })
        {
            const bool judged = bg > 0.6f && bg < 0.8f;
            beginTest ("N5a: click removal at background " + juce::String ((int) (bg * 100)) + " %" + (judged ? "" : " (reference, not judged)")
                       + ": peak of the click region at impact 100 % is <= -12 dB relative to impact 0 % (1 ms and 5 ms clicks)");
            const auto off = runNoiseReducer (kFs, in, bg, kFrame, 0.0f);
            const auto on = runNoiseReducer (kFs, in, bg, kFrame, 1.0f);
            double worst = -1.0e9;
            juce::String line, baseline;

            for (int k = 0; k < 6; ++k)
            {
                const int from = (int) (clickTimes[k] * 1.0e-3 * kFs) + kNativeD;
                const double db = 20.0 * std::log10 (vc::test::peakAbs (on.data() + from, region) / vc::test::peakAbs (off.data() + from, region));
                worst = std::max (worst, db);
                line << " " << juce::String (db, 1);
                baseline << " " << juce::String (20.0 * std::log10 (vc::test::peakAbs (off.data() + from, region)), 1);
            }

            logMessage ("N5a background " + juce::String ((int) (bg * 100)) + " %: peak change per click [dB]" + line + " (limit -12); impact 0 % peak [dBFS]" + baseline
                        + (judged ? "" : ", reference only"));

            // ゲートが開いているクリックの減衰量は設計値ちょうど（24dBの半分 = 12dB）になり、閾値-12dBと一致する。差は浮動小数点の丸め
            // （実測 -12.000000136dB）だけなので、数値誤差として0.05dBを許容する（設計値そのものを緩めたのではない）。
            if (judged)
                expect (worst <= -12.0 + 0.05, "background " + juce::String ((int) (bg * 100)) + " %: worst click peak change " + juce::String (worst, 4) + " dB (limit -12 dB + 0.05 dB numerical tolerance)");
        }
    }

    // ----- N5b: 誤検出 -----
    // (a) 立ち上がり10msの母音（無音から）、(b) 合成の破裂音（5msの雑音バースト、その30ms後に立ち上がり10msの母音）。
    // インパクト100%でも、母音の立ち上がりから80msの区間のエネルギーの減衰がインパクト0%比で1dB以下。f0 = 100/140Hz、背景70%・100%。
    // (c) 参考（判定なし）: 立ち上がり20msの母音。ホールド3msは10msアタックで決めた値で、20msアタックでは1dBを超えうる（D-023）。
    void runN5b()
    {
        const int n = (int) (6.0 * kFs);
        const int vowelLen = (int) (0.08 * kFs);

        for (const int kind : { 0, 1, 2 })
        {
            const bool judged = kind != 2;
            const double attackMs = kind == 2 ? 20.0 : 10.0;
            beginTest (juce::String ("N5b: ") + (kind == 1 ? "plosive (5 ms noise burst) + vowel 30 ms later" : kind == 2 ? "vowel with a 20 ms onset (reference, not judged)" : "vowel with a 10 ms onset")
                       + " at impact 100 %: attenuation of the vowel (first 80 ms) <= 1 dB relative to impact 0 %");

            for (const double f0 : { 100.0, 140.0 })
            {
                std::vector<float> in ((size_t) n, 0.0f);
                std::vector<int> onsets;

                for (int k = 0; k < 4; ++k)
                {
                    const int t = (int) ((1.0 + 1.2 * k) * kFs);

                    if (kind == 1)
                    {
                        vc::test::addNoiseBurst (in, t, kFs, 5.0, 0.2f, 60 + k);
                        onsets.push_back (t + (int) (0.035 * kFs)); // バースト5ms + 30ms
                    }
                    else
                    {
                        onsets.push_back (t);
                    }

                    vc::test::addEnvelopedVowel (in, onsets.back(), f0, kFs, 0.3f, attackMs, 300.0, 5.0, 30.0);
                }

                addPink (in, (float) std::pow (10.0, -60.0 / 20.0), 51);

                for (const float bg : { 0.7f, 1.0f })
                {
                    const auto off = runNoiseReducer (kFs, in, bg, kFrame, 0.0f);
                    const auto on = runNoiseReducer (kFs, in, bg, kFrame, 1.0f);
                    double worst = 1.0e9;
                    juce::String line;

                    for (const int t : onsets)
                    {
                        const double db = dbOf (energyOf (on.data() + t + kNativeD, vowelLen) / energyOf (off.data() + t + kNativeD, vowelLen));
                        worst = std::min (worst, db);
                        line << " " << juce::String (db, 3);
                    }

                    const juce::String label = juce::String (kind == 1 ? "plosive + vowel" : kind == 2 ? "vowel onset (20 ms attack)" : "vowel onset") + ", f0 " + juce::String (f0, 0)
                                               + ", background " + juce::String ((int) (bg * 100)) + " %";
                    logMessage ("N5b " + label + ": vowel energy change at impact 100 % [dB]" + line + " (limit -1)" + (judged ? "" : ", reference only"));

                    if (judged)
                        expect (worst >= -1.0, label + ": vowel attenuated by " + juce::String (-worst, 3) + " dB");
                }
            }
        }
    }

    // ----- N5b（ビット一致）: インパクト0%では抑制段を通らない -----
    // (a) 背景50%（ゲートなし）・インパクト0%の出力が、RNNoiseを直接480サンプルずつ呼んだ結果（480サンプル遅らせたもの）と一致する。
    // (b) 背景0%・インパクト0%の出力が、原音をDサンプル遅らせたものと一致する。
    // (c) インパクトを100%から0%へ戻した後（平滑化・ホールド・リリースが終わってから）の出力が、ずっと0%だったものと一致する。
    void runN5bBitExact()
    {
        beginTest ("N5b: impact 0 % is bit-identical to the unused stage (direct RNNoise at background 50 %, delayed input at 0 %, and after returning to 0 %)");

        const int n = (int) (6.0 * kFs);
        auto in = vc::test::makeSpeechAndPauses (n, kFs, 0.7, 0.5, 0.3f);
        addPink (in, 0.01f, 71);
        vc::test::addClick (in, (int) (0.9 * kFs), kFs, 3000.0, 1.0, 0.2f);
        vc::test::addClick (in, (int) (4.3 * kFs), kFs, 3000.0, 1.0, 0.2f);

        const auto sameFrom = [&] (const std::vector<float>& a, const std::vector<float>& b, int from)
        {
            for (int i = from; i < n; ++i)
                if (a[(size_t) i] != b[(size_t) i])
                    return i;

            return -1;
        };

        // (a)
        {
            const auto out = runNoiseReducer (kFs, in, 0.5f, kFrame, 0.0f);
            auto* st = rnnoise_create (nullptr);
            expect (st != nullptr, "rnnoise_create failed");

            if (st != nullptr)
            {
                std::vector<float> ref ((size_t) n, 0.0f); // 先頭の480サンプルは出力FIFOの初期充填（無音）
                std::vector<float> frameIn ((size_t) kFrame), frameOut ((size_t) kFrame);

                for (int f = 0; (f + 1) * kFrame <= n; ++f)
                {
                    for (int i = 0; i < kFrame; ++i)
                        frameIn[(size_t) i] = in[(size_t) (f * kFrame + i)] * 32768.0f;

                    rnnoise_process_frame (st, frameOut.data(), frameIn.data());

                    for (int i = 0; i < kFrame && (f + 1) * kFrame + i < n; ++i)
                        ref[(size_t) ((f + 1) * kFrame + i)] = frameOut[(size_t) i] * (1.0f / 32768.0f);
                }

                rnnoise_destroy (st);
                const int diffAt = sameFrom (out, ref, kNativeD + (int) (0.1 * kFs));
                expectEquals (diffAt, -1, "background 50 %, impact 0 %: differs from direct RNNoise output at sample " + juce::String (diffAt));
            }
        }

        // (b)
        {
            const auto out = runNoiseReducer (kFs, in, 0.0f, kFrame, 0.0f);
            const auto delayed = [&]
            {
                std::vector<float> v ((size_t) n, 0.0f);
                std::memcpy (v.data() + kNativeD, in.data(), sizeof (float) * (size_t) (n - kNativeD));
                return v;
            }();
            expectEquals (sameFrom (out, delayed, kNativeD + (int) (0.1 * kFs)), -1, "background 0 %, impact 0 %: not the delayed input");
        }

        // (c) 背景70%（ゲートが働く）。0〜2.5秒だけインパクト100%、その後0%。2.5秒 + 平滑化50ms + ホールド3ms + リリース10ms + 余裕の後は一致する。
        {
            vc::NoiseReducer always0, switched;
            always0.prepare (kFs, 4096);
            switched.prepare (kFs, 4096);
            auto a = in;
            auto b = in;
            const int switchAt = (int) (2.5 * kFs);

            for (int pos = 0; pos < n; pos += kFrame)
            {
                const int len = std::min (kFrame, n - pos);
                always0.setTarget (true, 0.7f, 0.0f);
                switched.setTarget (true, 0.7f, pos < switchAt ? 1.0f : 0.0f);
                always0.process (a.data() + pos, len);
                switched.process (b.data() + pos, len);
            }

            const int diffAt = sameFrom (a, b, switchAt + (int) (0.3 * kFs));
            expectEquals (diffAt, -1, "impact 100 % -> 0 %: output differs from the always-0 run at sample " + juce::String (diffAt));
            expect (sameFrom (a, b, 0) >= 0, "control: the impact stage changed nothing while it was on");
        }
    }

    // ----- N9b: ノイズ除去ON中のプリセット切替（ゲートが働く背景ノイズ100%を含む） -----
    // Engine E9（tests/EngineTests.cpp）と同じ判定器（checkNoClick）・同じ窓（切替の前後の定常区間1秒、切替の直前から20ms + 300ms + 20ms）・
    // 同じ信号（150Hzの合成母音、-80dBFSの雑音床。mode 1は切替の瞬間にデジタル無音から10msの二乗余弦のアタックで発声）で、8x7=56通りの
    // 順序付き切替を判定する。RNNoiseは同じ合成母音を最初の数秒は雑音として強く抑える（慣らし）ため、先頭に2秒の助走（同じ母音）を足す。
    // ゲートで無音に近づいた信号がSignalsmith Stretchの無音モード（エネルギー<1e-15が2ブロック続く）に入らないかの確認も兼ねる。
    struct SwitchResult
    {
        int failures = 0;
        int runs = 0;
        double worstRatio = 0.0;
        juce::String worstName;
    };

    SwitchResult runPresetSwitchWithNr (int mode, int offset, const std::vector<float>& voice, float bg, float impact)
    {
        constexpr int warmup = (int) (kFs * 2.0);
        constexpr int settle = (int) (kFs * 0.8);
        constexpr int steady = (int) kFs;
        constexpr int transition = (int) (kFs * 0.34);
        constexpr int total = settle + steady + 16 + transition + settle + steady;

        SwitchResult result;

        for (int a = 0; a < (int) vc::kPresets.size(); ++a)
        {
            for (int b = 0; b < (int) vc::kPresets.size(); ++b)
            {
                if (a == b)
                    continue;

                const int switchPos = warmup + settle + steady + offset;
                std::vector<float> in (voice);

                if (mode != 0)
                {
                    // 発声がノイズ除去の遅延（D）を経て届く時刻に、E9と同じく切替が重なるよう、入力の発声はDだけ早く始める。
                    const int onsetPos = switchPos - kNativeD;
                    std::fill (in.begin() + warmup, in.begin() + onsetPos, 0.0f);

                    if (mode == 2) // 無音ではなく-80dBFSの雑音床だけの休止。ゲートが閉じた（無音に近い）信号がシフターへ渡る
                    {
                        std::vector<float> pause ((size_t) (onsetPos - warmup), 0.0f);
                        vc::test::addNoiseFloor (pause, -80.0f, 777);
                        std::copy (pause.begin(), pause.end(), in.begin() + warmup);
                    }

                    const int attack = (int) std::lround (0.010 * kFs);

                    for (int i = 0; i < attack; ++i)
                        in[(size_t) (onsetPos + i)] *= (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (double) i / (double) attack));
                }

                vc::Engine engine;
                engine.prepare ({ kFs, 1024 });
                engine.params().preset.store (a);
                engine.params().nrEnabled.store (true);
                engine.params().nrBackground.store (bg);
                engine.params().nrImpact.store (impact);
                std::vector<float> out (in);

                engine.process (out.data(), switchPos);
                engine.params().preset.store (b);
                engine.process (out.data() + switchPos, warmup + total - switchPos);

                // 無音からの発声開始では切替前の出力が基準にならないため、基準は加工前の声の定常区間とする（E9と同じ）。
                const float* before = mode != 0 ? voice.data() + warmup + settle : out.data() + warmup + settle;
                const double beforeDiff = vc::test::maxAdjacentDiff (before, steady);
                const double afterDiff = vc::test::maxAdjacentDiff (out.data() + warmup + total - steady, steady);
                const double transDiff = vc::test::maxAdjacentDiff (out.data() + switchPos - 1, transition + 1);
                const double ratio = transDiff / std::max (beforeDiff, afterDiff);
                ++result.runs;

                if (ratio > result.worstRatio)
                {
                    result.worstRatio = ratio;
                    result.worstName = juce::String (vc::kPresets[(size_t) a].id) + "->" + vc::kPresets[(size_t) b].id;
                }

                if (! vc::test::checkNoClick (before, steady, out.data() + switchPos - 1, transition + 1, out.data() + warmup + total - steady))
                {
                    ++result.failures;
                    expect (false, juce::String (mode == 0 ? "steady" : mode == 1 ? "onset" : "onset after pause") + " " + vc::kPresets[(size_t) a].id + "->" + vc::kPresets[(size_t) b].id
                                       + " (background " + juce::String ((int) (bg * 100)) + " %, impact " + juce::String ((int) (impact * 100)) + " %): ratio "
                                       + juce::String (ratio, 2));
                }

                expect (vc::test::allFinite (out.data(), (int) out.size()), "non-finite output");
            }
        }

        return result;
    }

    // 注意: この判定にゲートの効果は現れていない（背景50%と100%で最悪比が同一）。最悪比1.422（限界1.5との余裕0.078）は、
    // 基準の「切替前後の隣接差」がRNNoise処理後の信号から取られることで上がった値で、ゲートによるものではない。
    void runN9b()
    {
        beginTest ("N9b: 56 preset switches with noise reduction ON (background 100 % / impact 50 %: gate active) pass the E9 click judge");

        constexpr int warmup = (int) (kFs * 2.0);
        constexpr int total = warmup + (int) (kFs * 0.8) + (int) kFs + 16 + (int) (kFs * 0.34) + (int) (kFs * 0.8) + (int) kFs;
        auto voice = vc::test::makeSyntheticVowel (150.0, kFs, total, { 730.0, 1090.0, 2440.0 }, { 80.0, 90.0, 120.0 }, 0.3f);
        vc::test::addNoiseFloor (voice, -80.0f, 333);

        // 対照: 休止（-80dBFSの雑音床だけ）でノイズ除去の出力がどこまで小さくなるか（Stretchの無音モードは入力ブロックのエネルギー < 1e-15。値の記録のみ）。
        for (const float bg : { 0.7f, 1.0f })
        {
            std::vector<float> pause ((size_t) (2.0 * kFs), 0.0f);
            vc::test::addNoiseFloor (pause, -80.0f, 777);
            auto speech = vc::test::makeSpeechLikeVowel (150.0, kFs, (int) (2.0 * kFs), 0.3f);
            speech.insert (speech.end(), pause.begin(), pause.end());
            const auto out = runNoiseReducer (kFs, speech, bg);
            double minBlock = 1.0e30;

            for (int pos = (int) (3.0 * kFs); pos + 480 <= (int) out.size(); pos += 480)
                minBlock = std::min (minBlock, energyOf (out.data() + pos, 480));

            logMessage ("N9b control: noise reduction output in a -80 dBFS pause at background " + juce::String ((int) (bg * 100)) + " %: minimum block (480 samples) energy "
                        + juce::String (dbOf (minBlock), 1) + " dB (Stretch silence mode below -150 dB = 1e-15), RMS "
                        + juce::String (20.0 * std::log10 (vc::test::rms (out.data() + (int) (3.0 * kFs), (int) kFs) + 1.0e-30), 1) + " dBFS");
        }

        struct Config { int mode; float bg; float impact; };

        for (const Config c : { Config { 0, 1.0f, 0.5f }, Config { 1, 1.0f, 0.5f }, Config { 2, 1.0f, 0.5f } })
        {
            for (const int offset : { 7, 11 }) // 切替位相（E9と同じ2通り）
            {
                const auto r = runPresetSwitchWithNr (c.mode, offset, voice, c.bg, c.impact);
                logMessage ("N9b " + juce::String (c.mode == 0 ? "steady" : c.mode == 1 ? "onset(10ms attack) from digital silence" : "onset(10ms attack) after a pause with a -80 dBFS noise floor")
                            + ", offset " + juce::String (offset) + ", background " + juce::String ((int) (c.bg * 100)) + " %, impact " + juce::String ((int) (c.impact * 100)) + " %: "
                            + juce::String (r.runs - r.failures) + "/" + juce::String (r.runs) + " switches passed, worst ratio (limit 1.5) " + juce::String (r.worstRatio, 3)
                            + " at " + r.worstName);
            }
        }
    }

    // ----- N7（ゲート・インパクト） -----
    // 発話と無音・クリックを含む信号で、背景ノイズ・インパクトを100%まで振り、ON/OFF・バイパスも含めてEngine::process中の確保0回。
    // ゲートが実際に閉じていること（陽性対照）も確認する。
    void runN7Gate()
    {
        beginTest ("N7 (gate/impact): no allocations in Engine::process with the gate closing and the impact stage firing (48 kHz and 44.1 kHz)");

        for (const double fs : { 48000.0, 44100.0 })
        {
            constexpr int maxBlock = 512;
            const int n = (int) (4.0 * fs);
            auto signal = vc::test::makeSpeechAndPauses (n, fs, 0.6, 0.6, 0.3f);
            addPink (signal, 0.01f, 81);

            for (int k = 0; k < 8; ++k)
                vc::test::addClick (signal, (int) ((0.7 + 0.5 * k) * fs), fs, 3000.0, 1.0 + k % 3, 0.2f);

            vc::Engine engine;
            engine.prepare ({ fs, maxBlock });
            auto& params = engine.params();
            params.nrEnabled.store (true);
            params.nrBackground.store (1.0f);
            params.nrImpact.store (1.0f);

            std::vector<float> work ((size_t) maxBlock);
            int cursor = 0;
            int blockToggle = 0;

            const auto run = [&] (int samples)
            {
                for (int done = 0; done < samples;)
                {
                    const int len = std::min ((blockToggle++ & 1) ? 137 : 480, samples - done);

                    for (int i = 0; i < len; ++i)
                    {
                        work[(size_t) i] = signal[(size_t) cursor];
                        cursor = (cursor + 1) % n;
                    }

                    engine.process (work.data(), len);
                    done += len;
                }
            };

            std::size_t allocations = 0;
            {
                vc::test::ScopedAllocationGuard guard;

                run (n);                                          // 背景・インパクト100%で1周（ゲートが閉じ、クリックを検出する）

                for (int step = 0; step <= 40; ++step)            // 掃引（ゲートの閉じ量・閾値・減衰量を動かす）
                {
                    params.nrBackground.store (0.4f + 0.6f * (float) (40 - step) / 40.0f);
                    params.nrImpact.store ((float) (step % 10) / 9.0f);
                    run ((int) (0.05 * fs));
                }

                params.nrBackground.store (1.0f);
                params.nrImpact.store (1.0f);
                run (n / 2);
                params.nrEnabled.store (false);                   // Active → FadingOut → Resting
                run ((int) (0.2 * fs));
                params.nrEnabled.store (true);                    // Priming（ゲート・インパクトの状態を消去）から再開
                run (n / 2);
                params.enabled.store (false);                     // バイパス中もゲート・インパクトは動く
                run (n / 2);
                params.enabled.store (true);

                allocations = guard.count();
            }

            expectEquals ((int) allocations, 0, "allocations at " + juce::String (fs, 0) + " Hz");
            expect (engine.debugNoiseReducer().getGateClosedSampleCount() > 0, "control: the gate never closed at " + juce::String (fs, 0) + " Hz");
        }
    }
};

static GateImpactTests gateImpactTests;

// ===== SECTION: Equalizer（T-012） =====
// EQ1〜EQ7・EQ9と、分割処理（ブロック長非依存）、範囲外・非有限値、バイパスとの関係。閾値は暫定で、外れたら緩めずに実測値を報告する。

using EqSettings = std::array<vc::EqBandSettings, vc::kEqBands>;

// kEqDefaultsには依存しないフラット設定（全バンドのゲイン0dB。タイプ・周波数・Qはspecの初期値と同じ並びだが値は独立に書く）。
EqSettings makeFlatEq()
{
    return { { { true, vc::EqType::LowShelf, 100.0f, 0.0f, 0.71f },
               { true, vc::EqType::Peak, 250.0f, 0.0f, 0.71f },
               { true, vc::EqType::Peak, 1000.0f, 0.0f, 0.71f },
               { true, vc::EqType::Peak, 3000.0f, 0.0f, 0.71f },
               { true, vc::EqType::HighShelf, 8000.0f, 0.0f, 0.71f } } };
}

// バンドbandだけ有効にし、残りを無効にした設定。
EqSettings makeSingleBandEq (int band, vc::EqType type, float hz, float gainDb, float q)
{
    auto s = makeFlatEq();

    for (auto& b : s)
        b.on = false;

    s[(size_t) band] = { true, type, hz, gainDb, q };
    return s;
}

// 解析値: 丸め済みの値から、JUCEのCoefficients（make*）の getMagnitudeForFrequency。
// Equalizerは同じ式のArrayCoefficientsを使うため、周波数特性が一致するかは「フィルタの構成・補間・クロスフェードが特性を変えないか」の確認になる。
juce::dsp::IIR::Coefficients<float>::Ptr makeReferenceCoefficients (double fs, const vc::EqBandSettings& b)
{
    using C = juce::dsp::IIR::Coefficients<float>;
    const float hz = std::min (b.hz, (float) (0.45 * fs));
    const float gain = juce::Decibels::decibelsToGain (b.gainDb);

    switch (b.type)
    {
        case vc::EqType::Peak:      return C::makePeakFilter (fs, hz, b.q, gain);
        case vc::EqType::LowShelf:  return C::makeLowShelf (fs, hz, b.q, gain);
        case vc::EqType::HighShelf: return C::makeHighShelf (fs, hz, b.q, gain);
        case vc::EqType::LowCut:    return C::makeHighPass (fs, hz, b.q);
        case vc::EqType::HighCut:   return C::makeLowPass (fs, hz, b.q);
    }

    return nullptr;
}

// 解析値のdB（有効なバンドの和）。
double referenceMagnitudeDb (double fs, const EqSettings& s, double hz)
{
    double db = 0.0;

    for (const auto& b : s)
        if (b.on)
            db += 20.0 * std::log10 (makeReferenceCoefficients (fs, b)->getMagnitudeForFrequency (hz, fs));

    return db;
}

// RBJ Audio EQ Cookbookのdouble実装（JUCEの式・Coefficientsに依存しない独立した参照値）。単一バンドの振幅[dB]。
double rbjMagnitudeDb (double fs, const vc::EqBandSettings& b, double f)
{
    const double pi = 3.14159265358979323846;
    const double hz = std::min ((double) b.hz, 0.45 * fs);
    const double w0 = 2.0 * pi * hz / fs;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double A = std::pow (10.0, (double) b.gainDb / 40.0);
    const double alpha = sw / (2.0 * (double) b.q);
    double b0, b1, b2, a0, a1, a2;

    switch (b.type)
    {
        case vc::EqType::Peak:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case vc::EqType::LowShelf:
        {
            const double t = 2 * std::sqrt (A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + t); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - t);
            a0 = (A + 1) + (A - 1) * cw + t; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - t;
            break;
        }
        case vc::EqType::HighShelf:
        {
            const double t = 2 * std::sqrt (A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + t); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - t);
            a0 = (A + 1) - (A - 1) * cw + t; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - t;
            break;
        }
        case vc::EqType::LowCut: // ハイパス
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        default: // HighCut: ローパス
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
    }

    const double w = 2.0 * pi * f / fs;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    return 20.0 * std::log10 (std::abs ((b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2)));
}

// 設定全体（EQ OFFなら0dB、有効なバンドのdB和）のRBJ参照値。
double rbjTotalDb (double fs, const EqSettings& s, bool run, double f)
{
    double db = 0.0;

    for (const auto& b : s)
        if (run && b.on)
            db += rbjMagnitudeDb (fs, b, f);

    return db;
}

constexpr int kEqFftOrder = 16;

// Active状態のEqualizerのインパルス応答（65536点）をFFTした振幅[dB]（bin 0〜N/2）。
std::vector<double> measureEqMagnitudeDb (double fs, const EqSettings& s)
{
    constexpr int n = 1 << kEqFftOrder;
    vc::Equalizer eq;
    eq.prepare (fs);
    eq.setTarget (true, s);

    std::vector<float> warm (2048, 0.0f); // FadingInを終わらせる（20ms = 960サンプル）
    eq.process (warm.data(), (int) warm.size());

    std::vector<float> data ((size_t) n * 2, 0.0f);
    data[0] = 1.0f;
    eq.setTarget (true, s);
    eq.process (data.data(), n);

    juce::dsp::FFT fft (kEqFftOrder);
    fft.performRealOnlyForwardTransform (data.data(), true);

    std::vector<double> db ((size_t) n / 2 + 1);

    for (size_t k = 0; k < db.size(); ++k)
        db[k] = 20.0 * std::log10 (std::max (1.0e-12, std::hypot ((double) data[2 * k], (double) data[2 * k + 1])));

    return db;
}

// 20Hz〜0.45fsの各binで、測定値と解析値の差の最大[dB]と、その周波数。
struct EqMagnitudeError
{
    double maxAbsDb = 0.0;
    double atHz = 0.0;
};

EqMagnitudeError compareEqMagnitude (double fs, const EqSettings& s)
{
    const auto measured = measureEqMagnitudeDb (fs, s);
    const double binHz = fs / (double) (1 << kEqFftOrder);
    EqMagnitudeError e;

    for (size_t k = (size_t) std::ceil (20.0 / binHz); (double) k * binHz <= 0.45 * fs; ++k)
    {
        const double f = (double) k * binHz;
        const double diff = std::abs (measured[k] - referenceMagnitudeDb (fs, s, f));

        if (diff > e.maxAbsDb)
        {
            e.maxAbsDb = diff;
            e.atHz = f;
        }
    }

    return e;
}

double rmsDb (const float* data, int n)
{
    return 20.0 * std::log10 (std::max (1.0e-12, vc::test::rms (data, n)));
}

class EqualizerTests final : public juce::UnitTest
{
public:
    EqualizerTests() : juce::UnitTest ("Equalizer", "Mic") {}

    void runTest() override
    {
        runEq1();
        runEq2();
        runEq4();
        runEq7();
        runEq6();
        runEq3();
        runSplit();
        runOutOfRange();
        runBypassAndNonFinite();
        runSettled();
        runRestart();
        runEq5();
        runEq9();
        runEq10();
    }

private:
    // ----- EQ10: マイク処理ウィンドウのグラフの曲線（eqCurveDb）-----
    // グラフは、Equalizerと同じ係数の関数（eqBandCoefficients）から曲線を作る。RBJの解析値（独立参照）と、Equalizerの実測（インパルス応答のFFT）に一致し、
    // OFFのバンドは曲線に入らないことを確かめる（表示と音がずれないこと。docs/design.md 10.3節）。
    void runEq10()
    {
        beginTest ("EQ10: graph curve (eqCurveDb) matches the RBJ analytic value and the measured Equalizer response");

        EqSettings s {};
        s[0] = { true, vc::EqType::LowCut, 90.0f, 5.0f, 0.71f };       // ゲインは使わない
        s[1] = { true, vc::EqType::LowShelf, 250.0f, 4.5f, 0.71f };
        s[2] = { false, vc::EqType::Peak, 1000.0f, 12.0f, 2.0f };      // 無効: 曲線に入らない
        s[3] = { true, vc::EqType::Peak, 3000.0f, -6.0f, 1.4f };
        s[4] = { true, vc::EqType::HighShelf, 8000.0f, 3.0f, 0.71f };

        for (const double fs : { 48000.0, 44100.0, 96000.0 })
        {
            double worstRbj = 0.0;

            for (double f = 20.0; f <= 20000.0; f *= 1.05)
            {
                if (f > 0.45 * fs)
                    break;

                worstRbj = std::max (worstRbj, std::abs (vc::eqCurveDb (s, fs, f) - rbjTotalDb (fs, s, true, f)));
            }

            logMessage ("EQ10 " + juce::String (fs, 0) + " Hz: curve vs RBJ max difference " + juce::String (worstRbj, 4) + " dB");
            expect (worstRbj <= 0.15, "curve vs RBJ at " + juce::String (fs, 0) + " Hz: " + juce::String (worstRbj, 4) + " dB");

            // 実測との比較は、EQ1と同じ48k/44.1kだけ（96kは20Hz付近でfloat32係数の量子化が大きく、0.18dB差になる。解析値との差は0.03dB以下）。
            if (fs > 50000.0)
                continue;

            const auto measured = measureEqMagnitudeDb (fs, s);
            const double binHz = fs / (double) (1 << kEqFftOrder);
            double worstMeasured = 0.0;

            for (size_t k = (size_t) std::ceil (20.0 / binHz); (double) k * binHz <= 0.45 * fs; ++k)
                worstMeasured = std::max (worstMeasured, std::abs (measured[k] - vc::eqCurveDb (s, fs, (double) k * binHz)));

            logMessage ("EQ10 " + juce::String (fs, 0) + " Hz: curve vs measured Equalizer max difference " + juce::String (worstMeasured, 4) + " dB");
            expect (worstMeasured <= 0.1, "curve vs measured at " + juce::String (fs, 0) + " Hz: " + juce::String (worstMeasured, 4) + " dB");
        }

        // 周波数の実効上限（0.45 fs）: 44.1kHzで20 kHzのバンドは19845 Hzとして扱う（Equalizerと同じ）。
        EqSettings edge = makeSingleBandEq (0, vc::EqType::Peak, 20000.0f, 12.0f, 2.0f);
        expect (std::abs (vc::eqCurveDb (edge, 44100.0, 19845.0) - rbjTotalDb (44100.0, edge, true, 19845.0)) <= 0.15, "clamped band frequency");

        // すべて無効なら0 dB。
        for (auto& b : s)
            b.on = false;

        expect (std::abs (vc::eqCurveDb (s, 48000.0, 1000.0)) < 1.0e-9, "all bands off is flat");
    }

    // ----- EQ1: 各タイプの周波数特性 -----
    void runEq1()
    {
        beginTest ("EQ1: frequency response of each type (impulse response, 65536-point FFT) matches getMagnitudeForFrequency within 0.1 dB over 20 Hz .. 0.45 fs");

        struct Case { const char* name; vc::EqType type; float hz, gainDb, q; };
        const Case cases[] = {
            { "peak 1 kHz +6 dB Q1", vc::EqType::Peak, 1000.0f, 6.0f, 1.0f },
            { "low shelf 200 Hz +6 dB Q0.71", vc::EqType::LowShelf, 200.0f, 6.0f, 0.71f },
            { "high shelf 5 kHz -4 dB Q0.71", vc::EqType::HighShelf, 5000.0f, -4.0f, 0.71f },
            { "low cut 80 Hz Q0.71", vc::EqType::LowCut, 80.0f, 0.0f, 0.71f },
            { "high cut 12 kHz Q0.71", vc::EqType::HighCut, 12000.0f, 0.0f, 0.71f },
        };

        for (const double fs : { 48000.0, 44100.0 })
        for (const auto& c : cases)
        for (const int band : { 0, 4 }) // バンド位置によらない
        {
            const auto s = makeSingleBandEq (band, c.type, c.hz, c.gainDb, c.q);
            const auto e = compareEqMagnitude (fs, s);
            logMessage ("EQ1 " + juce::String (fs, 0) + " Hz band " + juce::String (band + 1) + " " + c.name + ": max difference "
                        + juce::String (e.maxAbsDb, 4) + " dB at " + juce::String (e.atHz, 1) + " Hz");
            expect (e.maxAbsDb <= 0.1, juce::String (c.name) + " at " + juce::String (fs, 0) + " Hz: difference " + juce::String (e.maxAbsDb, 4)
                                           + " dB at " + juce::String (e.atHz, 1) + " Hz");

            // 独立参照（RBJ Cookbookのdouble実装）との差。float32係数の量子化を含むため、JUCE参照より緩く0.15dB。
            const auto measured = measureEqMagnitudeDb (fs, s);
            const double binHz = fs / (double) (1 << kEqFftOrder);
            double worstRbj = 0.0;

            for (size_t k = (size_t) std::ceil (20.0 / binHz); (double) k * binHz <= 0.45 * fs; ++k)
                worstRbj = std::max (worstRbj, std::abs (measured[k] - rbjTotalDb (fs, s, true, (double) k * binHz)));

            logMessage ("EQ1 " + juce::String (fs, 0) + " Hz band " + juce::String (band + 1) + " " + c.name + ": max difference to RBJ " + juce::String (worstRbj, 4) + " dB");
            expect (worstRbj <= 0.15, juce::String (c.name) + " at " + juce::String (fs, 0) + " Hz: difference to RBJ " + juce::String (worstRbj, 4) + " dB");
        }

        // 解析値そのものの確認（JUCEのmake*が型どおりの特性か。ローカットとハイカットの取り違えなどを防ぐ）。
        {
            const double fs = 48000.0;
            const auto peak = measureEqMagnitudeDb (fs, makeSingleBandEq (2, vc::EqType::Peak, 1000.0f, 6.0f, 1.0f));
            const auto low = measureEqMagnitudeDb (fs, makeSingleBandEq (0, vc::EqType::LowShelf, 200.0f, 6.0f, 0.71f));
            const auto high = measureEqMagnitudeDb (fs, makeSingleBandEq (4, vc::EqType::HighShelf, 5000.0f, -4.0f, 0.71f));
            const auto lowCut = measureEqMagnitudeDb (fs, makeSingleBandEq (0, vc::EqType::LowCut, 80.0f, 0.0f, 0.71f));
            const auto highCut = measureEqMagnitudeDb (fs, makeSingleBandEq (4, vc::EqType::HighCut, 12000.0f, 0.0f, 0.71f));
            const auto bin = [&] (double hz) { return (size_t) std::lround (hz * (double) (1 << kEqFftOrder) / fs); };

            expectWithinAbsoluteError (peak[bin (1000.0)], 6.0, 0.05);
            expectWithinAbsoluteError (peak[bin (60.0)], 0.0, 0.3);
            expectWithinAbsoluteError (low[bin (20.0)], 6.0, 0.3);
            expectWithinAbsoluteError (low[bin (5000.0)], 0.0, 0.3);
            expectWithinAbsoluteError (high[bin (20000.0)], -4.0, 0.5);
            expectWithinAbsoluteError (high[bin (200.0)], 0.0, 0.05);
            expectWithinAbsoluteError (lowCut[bin (80.0)], -3.0, 0.1);
            expectWithinAbsoluteError (lowCut[bin (5000.0)], 0.0, 0.05);
            expect (lowCut[bin (20.0)] < -20.0, "low cut attenuates 20 Hz");
            expectWithinAbsoluteError (highCut[bin (12000.0)], -3.0, 0.1);
            expectWithinAbsoluteError (highCut[bin (200.0)], 0.0, 0.05);
        }
    }

    // ----- EQ2: 直列 -----
    void runEq2()
    {
        beginTest ("EQ2: five bands at once match the sum of the bands' dB within 0.1 dB");

        const EqSettings configs[] = {
            { { { true, vc::EqType::LowShelf, 120.0f, 4.0f, 0.71f },
                { true, vc::EqType::Peak, 400.0f, -5.0f, 1.2f },
                { true, vc::EqType::Peak, 1800.0f, 6.0f, 2.0f },
                { true, vc::EqType::Peak, 4500.0f, -3.0f, 0.8f },
                { true, vc::EqType::HighShelf, 9000.0f, 3.0f, 0.71f } } },
            { { { true, vc::EqType::LowCut, 60.0f, 0.0f, 0.71f },
                { true, vc::EqType::Peak, 300.0f, 3.0f, 1.0f },
                { true, vc::EqType::Peak, 1200.0f, -4.0f, 1.0f },
                { true, vc::EqType::Peak, 3500.0f, 5.0f, 3.0f },
                { true, vc::EqType::HighCut, 14000.0f, 0.0f, 0.71f } } },
            { { { true, vc::EqType::LowShelf, 100.0f, 18.0f, 0.71f },
                { false, vc::EqType::Peak, 250.0f, 12.0f, 0.71f }, // 無効なバンドは和に入らない
                { true, vc::EqType::Peak, 1000.0f, -18.0f, 0.5f },
                { true, vc::EqType::Peak, 3000.0f, 9.0f, 4.0f },
                { true, vc::EqType::HighShelf, 8000.0f, -12.0f, 0.71f } } },
        };

        for (const double fs : { 48000.0, 44100.0 })
            for (size_t c = 0; c < std::size (configs); ++c)
            {
                const auto e = compareEqMagnitude (fs, configs[c]);
                logMessage ("EQ2 " + juce::String (fs, 0) + " Hz config " + juce::String ((int) c + 1) + ": max difference "
                            + juce::String (e.maxAbsDb, 4) + " dB at " + juce::String (e.atHz, 1) + " Hz");
                expect (e.maxAbsDb <= 0.1, "config " + juce::String ((int) c + 1) + " at " + juce::String (fs, 0) + " Hz: difference "
                                               + juce::String (e.maxAbsDb, 4) + " dB at " + juce::String (e.atHz, 1) + " Hz");
            }
    }

    // ----- EQ4: 極端な値 -----
    void runEq4()
    {
        beginTest ("EQ4: extreme values (20 kHz -> 0.45 fs, Q 0.1/10, gain +-18 dB) on 10 s of white noise stay finite and match the analytic RMS (no divergence)");

        double worstDb = 0.0;
        double peakOut = 0.0;

        for (const double fs : { 48000.0, 44100.0 })
        {
            const int n = (int) (10.0 * fs);
            const auto noise = makeWhiteNoise (n, 0.25f, 4242);
            const float hzs[] = { 20.0f, 1000.0f, 20000.0f };
            const float qs[] = { 0.1f, 10.0f };
            const float gains[] = { -18.0f, 18.0f };
            const vc::EqType types[] = { vc::EqType::Peak, vc::EqType::LowShelf, vc::EqType::HighShelf, vc::EqType::LowCut, vc::EqType::HighCut };

            const auto check = [&] (const EqSettings& s, const juce::String& label)
            {
                vc::Equalizer eq;
                eq.prepare (fs);
                auto out = noise;
                int pos = 0;

                while (pos < n) // 480ごと。毎ブロックsetTarget
                {
                    const int len = std::min (480, n - pos);
                    eq.setTarget (true, s);
                    eq.process (out.data() + pos, len);
                    pos += len;
                }

                expect (vc::test::allFinite (out.data(), n), label + ": non-finite output");

                // 白色雑音の出力パワー = 入力パワー × |H|²の平均（解析値）。後半5秒で比べる。
                constexpr int bins = 8192;
                double meanSq = 0.0;

                for (int k = 0; k < bins; ++k)
                {
                    const double f = (0.5 * fs) * ((double) k + 0.5) / (double) bins;
                    const double mag = std::pow (10.0, referenceMagnitudeDb (fs, s, f) / 20.0);
                    meanSq += mag * mag;
                }

                const double expectedDb = 10.0 * std::log10 (meanSq / bins);
                const double measuredDb = rmsDb (out.data() + n / 2, n / 2) - rmsDb (noise.data() + n / 2, n / 2);
                worstDb = std::max (worstDb, std::abs (measuredDb - expectedDb));
                peakOut = std::max (peakOut, vc::test::peakAbs (out.data(), n));
                expect (std::abs (measuredDb - expectedDb) <= 3.0,
                        label + ": RMS gain " + juce::String (measuredDb, 2) + " dB, analytic " + juce::String (expectedDb, 2) + " dB");
            };

            for (const auto type : types)
                for (const float hz : hzs)
                    for (const float q : qs)
                        for (const float g : gains)
                            check (makeSingleBandEq (2, type, hz, g, q),
                                   juce::String (fs, 0) + " Hz type " + juce::String ((int) type) + " hz " + juce::String (hz, 0) + " Q " + juce::String (q, 1)
                                       + " gain " + juce::String (g, 0));

            // 5バンドすべてが極端（直列でゲインが重なる）。
            for (const float q : qs)
                for (const float g : gains)
                    check ({ { { true, vc::EqType::LowShelf, 20.0f, g, q },
                               { true, vc::EqType::Peak, 20.0f, g, q },
                               { true, vc::EqType::Peak, 20000.0f, g, q },
                               { true, vc::EqType::Peak, 1000.0f, g, q },
                               { true, vc::EqType::HighShelf, 20000.0f, g, q } } },
                           juce::String (fs, 0) + " Hz all bands extreme, Q " + juce::String (q, 1) + " gain " + juce::String (g, 0));
        }

        logMessage ("EQ4: worst RMS difference to the analytic value " + juce::String (worstDb, 2) + " dB (threshold 3 dB), largest output peak "
                    + juce::String (peakOut, 1));
    }

    // ----- EQ7: 低い声 -----
    void runEq7()
    {
        beginTest ("EQ7: all bands at 0 dB (flat): an 85 Hz sine changes by <= 0.1 dB");

        for (const double fs : { 48000.0, 44100.0 })
        {
            const int n = (int) (2.0 * fs);
            const auto in = vc::test::makeSine (85.0, fs, n, 0.3f);
            auto out = in;

            vc::Equalizer eq;
            eq.prepare (fs);

            for (int pos = 0; pos < n; pos += 480)
            {
                eq.setTarget (true, makeFlatEq());
                eq.process (out.data() + pos, std::min (480, n - pos));
            }

            const int from = (int) (0.5 * fs);
            const double changeDb = rmsDb (out.data() + from, n - from) - rmsDb (in.data() + from, n - from);
            logMessage ("EQ7 " + juce::String (fs, 0) + " Hz: 85 Hz sine change " + juce::String (changeDb, 5) + " dB");
            expect (std::abs (changeDb) <= 0.1, juce::String (fs, 0) + " Hz: change " + juce::String (changeDb, 5) + " dB");
        }
    }

    // ----- EQ6: OFF時のビット一致 -----
    void runEq6()
    {
        beginTest ("EQ6: Equalizer::process leaves the buffer bit-identical while OFF (from the start, and after ON -> OFF completes); the fade is not");

        juce::Random rng (77);
        const auto makeInput = [&] (int n)
        {
            std::vector<float> v ((size_t) n);

            for (auto& x : v)
                x = (rng.nextFloat() * 2.0f - 1.0f) * 1.7f; // |x| > 1を含む

            return v;
        };

        const auto boosted = makeSingleBandEq (2, vc::EqType::Peak, 500.0f, 12.0f, 1.0f);

        for (const double fs : { 48000.0, 44100.0 })
        {
            vc::Equalizer eq;
            eq.prepare (fs);

            const auto identical = [&] (int blocks, const juce::String& label)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    const auto in = makeInput (b % 3 == 0 ? 480 : 137);
                    auto out = in;
                    eq.setTarget (false, boosted);
                    eq.process (out.data(), (int) out.size());
                    expect (std::memcmp (in.data(), out.data(), sizeof (float) * in.size()) == 0, label + ": block " + juce::String (b) + " modified");
                }
            };

            identical (20, juce::String (fs, 0) + " Hz, from the start");

            // ON: 変化する。
            bool changed = false;

            for (int b = 0; b < 30; ++b)
            {
                const auto in = makeInput (480);
                auto out = in;
                eq.setTarget (true, boosted);
                eq.process (out.data(), 480);
                changed = changed || std::memcmp (in.data(), out.data(), sizeof (float) * 480) != 0;
            }

            expect (changed, "control: ON did not change the signal");
            expect (eq.isRunning(), "control: not running while ON");

            // ON -> OFF: フェード中は変化し、完了後（20ms = 960サンプル）はビット一致。
            {
                const auto in = makeInput (200);
                auto out = in;
                eq.setTarget (false, boosted);
                eq.process (out.data(), 200);
                expect (std::memcmp (in.data(), out.data(), sizeof (float) * 200) != 0, "control: FadingOut left the buffer untouched");
            }

            for (int b = 0; b < 4; ++b)
            {
                auto out = makeInput (480);
                eq.setTarget (false, boosted);
                eq.process (out.data(), 480);
            }

            expect (! eq.isRunning(), "still running after the fade-out");
            identical (20, juce::String (fs, 0) + " Hz, after ON -> OFF");

            // Engine経由（EQ OFF・ノイズ除去OFF・バイパス）でも、既存のE4aと同じくビット一致（|x| <= 1）。
            vc::Engine engine;
            engine.params().enabled.store (false);
            engine.prepare ({ fs, 512 });
            auto in = makeInput (2000);

            for (auto& x : in)
                x = juce::jlimit (-1.0f, 1.0f, x);

            auto out = in;
            engine.process (out.data(), (int) out.size());
            expect (std::memcmp (in.data(), out.data(), sizeof (float) * in.size()) == 0, "Engine bypass with EQ OFF is not bit-identical");
        }
    }

    // ----- EQ3: クリック -----
    // 200Hz/210Hzの正弦（A = 0.3、-80dBFSの雑音付き）の、切り替え前後の定常区間と切り替え区間の隣接差を、N9aと同じ判定器で比べる。
    struct EqState
    {
        bool run = true;
        EqSettings settings = makeFlatEq();
    };

    struct EqStep
    {
        int atSample;
        std::function<void (EqState&)> apply;
    };

    std::vector<float> runEqScenario (EqState state, std::vector<EqStep> steps, int total, const std::vector<float>& signal)
    {
        vc::Equalizer eq;
        eq.prepare (kFs);

        std::sort (steps.begin(), steps.end(), [] (const EqStep& a, const EqStep& b) { return a.atSample < b.atSample; });

        auto out = signal;
        int pos = 0;
        size_t next = 0;

        while (pos < total)
        {
            while (next < steps.size() && steps[next].atSample <= pos)
                steps[next++].apply (state);

            int len = std::min (480, total - pos);

            if (next < steps.size())
                len = std::min (len, steps[next].atSample - pos);

            eq.setTarget (state.run, state.settings);
            eq.process (out.data() + pos, len);
            pos += len;
        }

        return out;
    }

    void runEq3()
    {
        beginTest ("EQ3: no clicks when gain, frequency, Q, type, band on/off or EQ on/off change (click judge as in N9a)");

        const int total = (int) (4.2 * kFs);
        const int steady = (int) (0.5 * kFs);
        const int transLen = (int) (0.4 * kFs);
        const int afterGap = (int) (1.0 * kFs);
        const int base = (int) (2.0 * kFs);

        struct Scenario
        {
            const char* name;
            EqState initial;
            std::function<void (EqState&)> change;
            std::function<void (EqState&)> changeBack; // 反転用（あれば、change後400サンプルで適用）
        };

        const auto oneBand = [] (vc::EqType type, float hz, float gain, float q, bool run = true)
        {
            EqState s;
            s.run = run;
            s.settings = makeSingleBandEq (2, type, hz, gain, q);
            return s;
        };

        const auto withType = [] (vc::EqType t) { return [t] (EqState& s) { s.settings[2].type = t; }; };

        std::vector<Scenario> scenarios = {
            { "gain -12 -> +12 dB", oneBand (vc::EqType::Peak, 200.0f, -12.0f, 1.0f), [] (EqState& s) { s.settings[2].gainDb = 12.0f; }, nullptr },
            { "frequency 100 -> 5000 Hz", oneBand (vc::EqType::Peak, 100.0f, 6.0f, 0.5f), [] (EqState& s) { s.settings[2].hz = 5000.0f; }, nullptr },
            { "Q 0.5 -> 8", oneBand (vc::EqType::Peak, 250.0f, 12.0f, 0.5f), [] (EqState& s) { s.settings[2].q = 8.0f; }, nullptr },
            { "type peak -> low shelf", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 0.71f), withType (vc::EqType::LowShelf), nullptr },
            { "type low shelf -> high shelf", oneBand (vc::EqType::LowShelf, 200.0f, 12.0f, 0.71f), withType (vc::EqType::HighShelf), nullptr },
            { "type high shelf -> low cut", oneBand (vc::EqType::HighShelf, 200.0f, 12.0f, 0.71f), withType (vc::EqType::LowCut), nullptr },
            { "type low cut -> high cut", oneBand (vc::EqType::LowCut, 200.0f, 0.0f, 0.71f), withType (vc::EqType::HighCut), nullptr },
            { "type high cut -> peak", oneBand (vc::EqType::HighCut, 200.0f, 12.0f, 0.71f), withType (vc::EqType::Peak), nullptr },
            { "type change reversed mid-fade (peak -> low shelf -> peak)", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 0.71f), withType (vc::EqType::LowShelf), withType (vc::EqType::Peak) },
            { "band disabled", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 1.0f), [] (EqState& s) { s.settings[2].on = false; }, nullptr },
            { "band enabled", [&] { auto s = oneBand (vc::EqType::Peak, 200.0f, 12.0f, 1.0f); s.settings[2].on = false; return s; }(),
              [] (EqState& s) { s.settings[2].on = true; }, nullptr },
            { "EQ ON -> OFF", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 1.0f), [] (EqState& s) { s.run = false; }, nullptr },
            { "EQ OFF -> ON", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 1.0f, false), [] (EqState& s) { s.run = true; }, nullptr },
            { "EQ reversed mid-fade (ON -> OFF -> ON)", oneBand (vc::EqType::Peak, 200.0f, 12.0f, 1.0f), [] (EqState& s) { s.run = false; }, [] (EqState& s) { s.run = true; } },
            { "EQ reversed mid-fade (OFF -> ON -> OFF, -12 dB)", oneBand (vc::EqType::Peak, 200.0f, -12.0f, 1.0f, false), [] (EqState& s) { s.run = true; }, [] (EqState& s) { s.run = false; } },
        };

        double worstRatio = 0.0;
        juce::String worstName;

        for (const double freq : { 200.0, 210.0 })
        for (const auto& sc : scenarios)
        {
            auto signal = vc::test::makeSine (freq, kFs, total, 0.3f);
            vc::test::addNoiseFloor (signal, -80.0f);

            for (const int offset : { 0, 37, 211 })
            {
                const int t = base + offset;
                std::vector<EqStep> steps { { t, sc.change } };

                if (sc.changeBack)
                    steps.push_back ({ t + 400, sc.changeBack });

                const auto out = runEqScenario (sc.initial, steps, total, signal);

                // 切り替え区間は、切り替えの直前から取る（即時の切り替えの段差［t - 1 → t］も見る）。
                constexpr int lead = 8;
                const bool ok = vc::test::checkNoClick (out.data() + t - steady, steady, out.data() + t - lead, transLen + lead, out.data() + t + afterGap);
                const double before = vc::test::maxAdjacentDiff (out.data() + t - steady, steady);
                const double after = vc::test::maxAdjacentDiff (out.data() + t + afterGap, steady);
                const double trans = vc::test::maxAdjacentDiff (out.data() + t - lead, transLen + lead);
                const double ratio = trans / (1.5 * std::max (before, after));

                if (ratio > worstRatio)
                {
                    worstRatio = ratio;
                    worstName = juce::String (sc.name) + ", " + juce::String (freq, 0) + " Hz, offset " + juce::String (offset);
                }

                expect (ok, juce::String ("click detected: ") + sc.name + ", " + juce::String (freq, 0) + " Hz, offset " + juce::String (offset)
                                + ", transition/threshold " + juce::String (ratio, 3));
                expect (vc::test::allFinite (out.data(), total), juce::String ("non-finite output: ") + sc.name);
            }
        }

        logMessage ("EQ3: worst transition/threshold ratio over all scenarios " + juce::String (worstRatio, 3) + " (must be <= 1): " + worstName);
    }

    // ----- 分割処理（ブロック長非依存） -----
    // 2つの時刻表: パラメータの変更（ゲイン・周波数・Q・タイプ・有効/無効・ON/OFF、フェード中の反転を含む）を、
    // 9600サンプル（200ms）ごとの共通の位置で行う。ブロックの区切りは列ごとに違う（変更の位置では必ず区切る）。
    static std::vector<EqStep> splitSteps()
    {
        std::vector<EqStep> steps;
        const auto add = [&] (int at, std::function<void (EqState&)> f) { steps.push_back ({ at, std::move (f) }); };
        const int u = 9600;

        add (0 * u, [] (EqState& s) { s.run = true; });
        add (1 * u, [] (EqState& s) { s.settings[0].gainDb = 9.0f; });
        add (2 * u, [] (EqState& s) { s.settings[2].hz = 2500.0f; });
        add (3 * u, [] (EqState& s) { s.settings[1].q = 4.0f; s.settings[1].gainDb = -7.0f; });
        add (4 * u, [] (EqState& s) { s.settings[1].type = vc::EqType::HighCut; });
        add (5 * u, [] (EqState& s) { s.settings[3].on = false; });
        add (6 * u, [] (EqState& s) { s.settings[3].on = true; });
        add (7 * u, [] (EqState& s) { s.settings[4].gainDb = -12.0f; s.settings[4].hz = 5000.0f; });
        add (8 * u, [] (EqState& s) { s.settings[0].type = vc::EqType::Peak; s.settings[0].hz = 200.0f; });
        add (9 * u, [] (EqState& s) { s.run = false; });
        add (9 * u + 300, [] (EqState& s) { s.run = true; });                 // FadingOut中の反転
        add (10 * u, [] (EqState& s) { s.settings[2].type = vc::EqType::LowCut; });
        add (10 * u + 301, [] (EqState& s) { s.settings[2].type = vc::EqType::Peak; }); // タイプ変更のフェード中の反転
        add (11 * u, [] (EqState& s) { for (auto& b : s.settings) b.gainDb = -18.0f; });
        add (12 * u, [] (EqState& s) { for (auto& b : s.settings) b.gainDb = 18.0f; });
        add (13 * u, [] (EqState& s) { s.settings[1].type = vc::EqType::LowShelf; s.run = false; });
        add (13 * u + 500, [] (EqState& s) { s.run = true; });
        return steps;
    }

    // sizes列のブロック長で処理する（変更の位置では必ず区切る）。Engine経由（バイパス: EQの出力が見える）。
    std::vector<float> runSplit (double fs, const std::vector<float>& signal, const std::vector<int>& sizes, bool viaEngine)
    {
        auto steps = splitSteps();
        std::sort (steps.begin(), steps.end(), [] (const EqStep& a, const EqStep& b) { return a.atSample < b.atSample; });

        constexpr int maxBlock = 4096;
        vc::Engine engine;
        vc::Equalizer eq;

        if (viaEngine)
        {
            engine.params().enabled.store (false);
            engine.prepare ({ fs, maxBlock });
        }
        else
        {
            eq.prepare (fs);
        }

        EqState state;
        state.run = false;
        auto out = signal;
        const int total = (int) out.size();
        int pos = 0;
        size_t next = 0;
        size_t k = 0;

        while (pos < total)
        {
            bool changed = false;

            while (next < steps.size() && steps[next].atSample <= pos)
            {
                steps[next++].apply (state);
                changed = true;
            }

            if (changed && viaEngine)
            {
                auto& p = engine.params();
                p.eqEnabled.store (state.run);

                for (size_t i = 0; i < state.settings.size(); ++i)
                    p.eqBands[i].store (state.settings[i]);
            }

            int len = std::min (sizes[k++ % sizes.size()], total - pos);

            if (next < steps.size())
                len = std::min (len, steps[next].atSample - pos);

            if (viaEngine)
            {
                engine.process (out.data() + pos, len);
            }
            else
            {
                eq.setTarget (state.run, state.settings);
                eq.process (out.data() + pos, len);
            }

            pos += len;
        }

        return out;
    }

    void runSplit()
    {
        beginTest ("Split: mixed block lengths {1, 7, 128, 441, 480, 4096} and 1-sample / one-shot processing are bit-identical (EQ parameter changes included)");

        for (const double fs : { 48000.0, 44100.0 })
        for (const bool viaEngine : { false, true })
        {
            const int n = 15 * 9600 + 4000;
            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.05f);
            const auto noise = makeWhiteNoise (n, 0.02f, 13);

            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += noise[i];

            const auto ref = runSplit (fs, signal, { 480 }, viaEngine);
            const auto mixed = runSplit (fs, signal, std::vector<int> (std::begin (kMixedBlocks), std::end (kMixedBlocks)), viaEngine);
            const auto oneSample = runSplit (fs, signal, { 1 }, viaEngine);
            const auto odd = runSplit (fs, signal, { 333 }, viaEngine);
            const auto big = runSplit (fs, signal, { 100000 }, viaEngine);

            const juce::String label = juce::String (fs, 0) + " Hz" + (viaEngine ? " (Engine)" : " (Equalizer)");
            expect (std::memcmp (mixed.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": mixed blocks differ from 480-blocks");
            expect (std::memcmp (oneSample.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": 1-sample blocks differ from 480-blocks");
            expect (std::memcmp (odd.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": 333-blocks differ from 480-blocks");
            expect (std::memcmp (big.data(), ref.data(), sizeof (float) * (size_t) n) == 0, label + ": one-shot differs from 480-blocks");
            expect (vc::test::allFinite (ref.data(), n), label + ": non-finite output");

            // 対照: EQがこの信号を実際に変えている（ON区間がある）。
            double diff = 0.0;

            for (int i = 0; i < n; ++i)
                diff = std::max (diff, (double) std::abs (ref[(size_t) i] - signal[(size_t) i]));

            expect (diff > 0.01, label + ": control: the EQ did not change the signal (max change " + juce::String (diff, 5) + ")");
        }
    }

    // ----- Engineの範囲外・非有限値の扱い -----
    void runOutOfRange()
    {
        beginTest ("Out-of-range EQ parameters are rounded to the ends, NaN/Inf and an invalid type go to the initial value (same output as explicit values)");

        const double fs = 44100.0; // 周波数の上限（0.45 fs = 19845Hz）も確認する
        const int n = (int) (1.0 * fs);
        auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.05f);
        const auto noise = makeWhiteNoise (n, 0.02f, 21);

        for (size_t i = 0; i < signal.size(); ++i)
            signal[i] += noise[i];

        const auto runWith = [&] (const std::function<void (vc::AtomicParams&)>& setup)
        {
            vc::Engine engine;
            engine.params().enabled.store (false); // EQの出力がそのまま見える
            engine.params().eqEnabled.store (true);
            setup (engine.params());
            engine.prepare ({ fs, 480 });
            auto out = signal;
            engine.process (out.data(), n);
            return out;
        };

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const auto& d = vc::kEqDefaults;

        // 範囲外 → 端。
        const auto clampedOut = runWith ([&] (vc::AtomicParams& p)
        {
            p.eqBands[0].store ({ true, vc::EqType::Peak, 5.0f, 100.0f, 0.0f });
            p.eqBands[1].store ({ true, vc::EqType::HighShelf, 50000.0f, -100.0f, 100.0f });
            p.eqBands[2].store ({ true, vc::EqType::Peak, -3.0f, 18.4f, -2.0f });
        });
        const auto endsOut = runWith ([&] (vc::AtomicParams& p)
        {
            p.eqBands[0].store ({ true, vc::EqType::Peak, 20.0f, 18.0f, 0.1f });
            p.eqBands[1].store ({ true, vc::EqType::HighShelf, 20000.0f, -18.0f, 10.0f });
            p.eqBands[2].store ({ true, vc::EqType::Peak, 20.0f, 18.0f, 0.1f });
        });
        expect (std::memcmp (clampedOut.data(), endsOut.data(), sizeof (float) * (size_t) n) == 0, "out-of-range values were not rounded to the ends");
        expect (vc::test::allFinite (clampedOut.data(), n), "non-finite output with out-of-range values");

        // NaN/Inf・不正なタイプ → 初期値（周波数・ゲイン・Qは各バンドのkEqDefaults、タイプは範囲外ならkEqDefaultsのタイプ）。
        const auto badOut = runWith ([&] (vc::AtomicParams& p)
        {
            p.eqBands[0].store ({ true, vc::EqType::Peak, nan, 6.0f, 1.0f });
            p.eqBands[1].store ({ true, vc::EqType::Peak, 500.0f, inf, -inf });
            p.eqBands[2].store ({ true, vc::EqType::Peak, 1000.0f, -inf, nan });
            p.eqBands[3].type.store (99);
            p.eqBands[3].gainDb.store (6.0f);
            p.eqBands[4].type.store (-1);
            p.eqBands[4].gainDb.store (-6.0f);
        });
        const auto goodOut = runWith ([&] (vc::AtomicParams& p)
        {
            p.eqBands[0].store ({ true, vc::EqType::Peak, d[0].hz, 6.0f, 1.0f });
            p.eqBands[1].store ({ true, vc::EqType::Peak, 500.0f, d[1].gainDb, d[1].q });
            p.eqBands[2].store ({ true, vc::EqType::Peak, 1000.0f, d[2].gainDb, d[2].q });
            p.eqBands[3].store ({ true, d[3].type, d[3].hz, 6.0f, d[3].q });
            p.eqBands[4].store ({ true, d[4].type, d[4].hz, -6.0f, d[4].q });
        });
        expect (std::memcmp (badOut.data(), goodOut.data(), sizeof (float) * (size_t) n) == 0, "NaN/Inf/invalid type were not replaced by the initial values");
        expect (vc::test::allFinite (badOut.data(), n), "non-finite output with NaN/Inf parameters");

        // Equalizer単体: 0.45 fsを超える周波数は0.45 fsと同じ。
        {
            const auto measureAt = [&] (float hz)
            {
                return measureEqMagnitudeDb (fs, makeSingleBandEq (2, vc::EqType::HighCut, hz, 0.0f, 0.71f));
            };

            const auto a = measureAt (20000.0f);
            const auto b = measureAt ((float) (0.45 * fs));
            expect (a == b, "20 kHz at 44.1 kHz is not rounded to 0.45 fs");
        }
    }

    // ----- バイパスとの関係・NaN -----
    void runBypassAndNonFinite()
    {
        beginTest ("Engine: the bypass passes the EQ output (bit-identical to Equalizer alone) and does not reset the EQ; NaN input silences the block, raises the flag and resets the EQ");

        const double fs = 48000.0;
        const int n = (int) (2.0 * fs);
        const auto signal = vc::test::makeSine (200.0, fs, n, 0.1f);
        const auto boosted = makeSingleBandEq (2, vc::EqType::Peak, 200.0f, 12.0f, 1.0f);

        {
            // Equalizer単体（480ブロック）
            auto ref = signal;
            vc::Equalizer eq;
            eq.prepare (fs);

            for (int pos = 0; pos < n; pos += 480)
            {
                eq.setTarget (true, boosted);
                eq.process (ref.data() + pos, std::min (480, n - pos));
            }

            vc::Engine engine;
            engine.params().eqEnabled.store (true);
            engine.prepare ({ fs, 512 });

            for (size_t i = 0; i < boosted.size(); ++i)
                engine.params().eqBands[i].store (boosted[i]);

            auto out = signal;

            // バイパスの切り替えを繰り返しても、EQはリセットされない（出力はEQ単体と一致しつづける）。
            // 有効時はチェーンを通る（ノーマル）ため、バイパス中の区間だけ比べる。
            engine.params().enabled.store (false);
            for (int pos = 0; pos < n; pos += 480)
            {
                if (pos == 480 * 40)
                    engine.params().enabled.store (true);
                if (pos == 480 * 50)
                    engine.params().enabled.store (false);

                engine.process (out.data() + pos, std::min (480, n - pos));

                if (pos < 480 * 40 || pos >= 480 * 50 + 480 * 2) // バイパス（完全バイパスになった後）の区間
                {
                    const int len = std::min (480, n - pos);
                    expect (std::memcmp (out.data() + pos, ref.data() + pos, sizeof (float) * (size_t) len) == 0,
                            "bypass output differs from the EQ output at sample " + juce::String (pos));
                }
            }
        }

        // NaN入力（EQのみON、ノイズ除去OFF）: そのブロックは無音、フラグ、以後は有限でEQが再び効く。
        {
            vc::Engine engine;
            engine.params().eqEnabled.store (true);
            engine.prepare ({ fs, 512 });

            for (size_t i = 0; i < boosted.size(); ++i)
                engine.params().eqBands[i].store (boosted[i]);

            auto out = signal;
            int pos = 0;

            for (; pos < (int) (0.5 * fs); pos += 480)
                engine.process (out.data() + pos, 480);

            expectEquals ((int) engine.getErrorFlags(), 0);
            out[(size_t) pos + 3] = std::numeric_limits<float>::quiet_NaN();
            engine.process (out.data() + pos, 480);
            expect (vc::test::peakAbs (out.data() + pos, 480) == 0.0, "the block with NaN input is not silent");
            expectEquals ((int) engine.getErrorFlags() & 1, 1, "error flag not raised");
            pos += 480;

            const auto afterNaN = signal; // 以降は正常な入力
            std::copy (afterNaN.begin() + pos, afterNaN.end(), out.begin() + pos);

            for (; pos + 480 <= n; pos += 480)
                engine.process (out.data() + pos, 480);

            expect (vc::test::allFinite (out.data() + (int) (0.6 * fs), (int) (1.0 * fs)), "non-finite output after recovery");
            // 回復後にEQが効いている（+12dB@200Hz。Q=1のピークで、入力0.1の正弦の振幅が約4倍）。
            const double gainDb = rmsDb (out.data() + (int) (1.5 * fs), (int) (0.4 * fs)) - rmsDb (signal.data() + (int) (1.5 * fs), (int) (0.4 * fs));
            expect (std::abs (gainDb - 12.0) < 0.5, "EQ did not recover after the NaN (gain " + juce::String (gainDb, 2) + " dB)");
        }
    }

    // ----- 稼働中の変更が目標値へ到達する（RBJ参照） -----
    // 稼働中に1つの項目を変更して400ms（補間50ms・クロスフェード20msの数倍）待ち、定常の正弦波ゲインをRBJの解析値と比べる。
    // 44.1kは50ms = 2205サンプルで32の倍数にならない（補間の最後のグループが端数になる）。
    void runSettled()
    {
        beginTest ("Settled: after a change while running (gain, frequency, Q, type, band on/off, EQ on/off) the steady sine gain matches the RBJ value within 0.03 dB (48k/44.1k)");

        struct Case { const char* name; EqState from; std::function<void (EqState&)> change; };

        const auto one = [] (vc::EqType t, float hz, float g, float q, bool run = true)
        {
            EqState st;
            st.run = run;
            st.settings = makeSingleBandEq (2, t, hz, g, q);
            return st;
        };

        const std::vector<Case> cases = {
            { "gain -18 -> +18 dB", one (vc::EqType::Peak, 1000.0f, -18.0f, 1.0f), [] (EqState& x) { x.settings[2].gainDb = 18.0f; } },
            { "gain +12 -> -6 dB", one (vc::EqType::LowShelf, 300.0f, 12.0f, 0.71f), [] (EqState& x) { x.settings[2].gainDb = -6.0f; } },
            { "frequency 500 -> 2000 Hz", one (vc::EqType::Peak, 500.0f, 9.0f, 2.0f), [] (EqState& x) { x.settings[2].hz = 2000.0f; } },
            { "frequency 4000 -> 250 Hz", one (vc::EqType::HighShelf, 4000.0f, 12.0f, 0.71f), [] (EqState& x) { x.settings[2].hz = 250.0f; } },
            { "Q 0.5 -> 6", one (vc::EqType::Peak, 1000.0f, 12.0f, 0.5f), [] (EqState& x) { x.settings[2].q = 6.0f; } },
            { "Q 8 -> 0.3", one (vc::EqType::Peak, 1000.0f, -12.0f, 8.0f), [] (EqState& x) { x.settings[2].q = 0.3f; } },
            { "type peak -> high shelf", one (vc::EqType::Peak, 1000.0f, 9.0f, 1.0f), [] (EqState& x) { x.settings[2].type = vc::EqType::HighShelf; } },
            { "type low cut -> high cut", one (vc::EqType::LowCut, 500.0f, 0.0f, 0.71f), [] (EqState& x) { x.settings[2].type = vc::EqType::HighCut; } },
            { "band disabled", one (vc::EqType::Peak, 1000.0f, 12.0f, 1.0f), [] (EqState& x) { x.settings[2].on = false; } },
            { "band enabled", [&] { auto x = one (vc::EqType::Peak, 1000.0f, 12.0f, 1.0f); x.settings[2].on = false; return x; }(),
              [] (EqState& x) { x.settings[2].on = true; } },
            { "EQ OFF", one (vc::EqType::Peak, 1000.0f, 12.0f, 1.0f), [] (EqState& x) { x.run = false; } },
            { "EQ ON", one (vc::EqType::Peak, 1000.0f, 12.0f, 1.0f, false), [] (EqState& x) { x.run = true; } },
        };

        double worst = 0.0;

        for (const double fs : { 48000.0, 44100.0 })
        for (const auto& c : cases)
        for (const double f : { 250.0, 1000.0, 3000.0 })
        {
            vc::Equalizer eq;
            eq.prepare (fs);
            EqState st = c.from;
            const auto run = [&] (const std::vector<float>& in)
            {
                auto out = in;

                for (int pos = 0; pos < (int) out.size(); pos += 480)
                {
                    eq.setTarget (st.run, st.settings);
                    eq.process (out.data() + pos, std::min (480, (int) out.size() - pos));
                }

                return out;
            };

            run (makeWhiteNoise ((int) (0.5 * fs), 0.1f, 3));  // 変更前の状態で稼働
            c.change (st);
            run (makeWhiteNoise ((int) (0.4 * fs), 0.1f, 4));  // 400ms待つ
            const int n = (int) (0.3 * fs);
            const auto in = vc::test::makeSine (f, fs, n, 0.1f);
            const auto out = run (in);
            const int from = n / 2;
            const double gain = rmsDb (out.data() + from, n - from) - rmsDb (in.data() + from, n - from);
            const double expected = rbjTotalDb (fs, st.settings, st.run, f);
            worst = std::max (worst, std::abs (gain - expected));
            expect (std::abs (gain - expected) <= 0.03, juce::String (c.name) + " at " + juce::String (fs, 0) + " Hz, " + juce::String (f, 0)
                                                          + " Hz sine: gain " + juce::String (gain, 4) + " dB, RBJ " + juce::String (expected, 4) + " dB");
        }

        logMessage ("Settled: worst difference to RBJ " + juce::String (worst, 4) + " dB (limit 0.03)");
    }

    // ----- 再開・NaN後の状態が新規インスタンスと同じ -----
    void runRestart()
    {
        beginTest ("Restart: after OFF -> ON, and after a NaN block in Engine, the EQ output is bit-identical to a fresh instance (filter state and Resting are reset)");

        const double fs = 48000.0;
        const auto boosted = makeSingleBandEq (2, vc::EqType::Peak, 500.0f, 12.0f, 1.0f);
        const auto x = makeWhiteNoise (4800, 0.1f, 8);

        // (a) Equalizer: ON → OFF（フェード完了）→ ON の出力 = 新規インスタンスのON直後の出力。
        {
            vc::Equalizer used, fresh;
            used.prepare (fs);
            fresh.prepare (fs);
            const auto feed = [&] (vc::Equalizer& e, bool run, std::vector<float> in)
            {
                for (int pos = 0; pos < (int) in.size(); pos += 480)
                {
                    e.setTarget (run, boosted);
                    e.process (in.data() + pos, std::min (480, (int) in.size() - pos));
                }

                return in;
            };

            feed (used, true, makeWhiteNoise (14400, 0.3f, 9));
            feed (used, false, makeWhiteNoise (4800, 0.3f, 10));
            expect (! used.isRunning(), "not Resting after OFF");
            const auto a = feed (used, true, x);
            const auto b = feed (fresh, true, x);
            expect (std::memcmp (a.data(), b.data(), sizeof (float) * x.size()) == 0, "OFF -> ON output differs from a fresh instance");
        }

        // (b) Engine（EQのみON・ノイズ除去OFF・バイパス）: EQ Activeのままnan入力 → 次ブロック以降が新規Engineの最初のブロックと一致。
        {
            const auto make = [&] (vc::Engine& e)
            {
                e.params().enabled.store (false);
                e.params().eqEnabled.store (true);

                for (size_t i = 0; i < boosted.size(); ++i)
                    e.params().eqBands[i].store (boosted[i]);

                e.prepare ({ fs, 512 });
            };

            vc::Engine used, fresh;
            make (used);
            make (fresh);

            auto warm = makeWhiteNoise (14400, 0.3f, 11);

            for (int pos = 0; pos < (int) warm.size(); pos += 480)
                used.process (warm.data() + pos, 480);

            auto bad = makeWhiteNoise (480, 0.3f, 12);
            bad[5] = std::numeric_limits<float>::quiet_NaN();
            used.process (bad.data(), 480);
            expectEquals ((int) used.getErrorFlags() & 1, 1, "flag not raised");

            auto a = x, b = x;

            for (int pos = 0; pos < (int) x.size(); pos += 480)
            {
                used.process (a.data() + pos, 480);
                fresh.process (b.data() + pos, 480);
            }

            expect (std::memcmp (a.data(), b.data(), sizeof (float) * x.size()) == 0, "output after a NaN block differs from a fresh Engine (EQ not reset)");
        }
    }

    // ----- EQ5: アロケーション -----
    void runEq5()
    {
        beginTest ("EQ5: no allocations in Engine::process for EQ parameter sweeps, type changes, band on/off, EQ on/off and NaN parameters (new and malloc)");

        for (const double fs : { 48000.0, 44100.0 })
        {
            constexpr int maxBlock = 512;
            const int n = (int) (2.0 * fs);
            auto signal = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.05f);
            const auto noise = makeWhiteNoise (n, 0.02f, 5);

            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += noise[i];

            vc::Engine engine;
            engine.prepare ({ fs, maxBlock });
            auto& params = engine.params();

            std::vector<float> work ((size_t) maxBlock);
            int cursor = 0;
            int blockToggle = 0;

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
            const int settle = (int) (0.2 * fs);
            const vc::EqType types[] = { vc::EqType::Peak, vc::EqType::LowShelf, vc::EqType::HighShelf, vc::EqType::LowCut, vc::EqType::HighCut };

            std::size_t allocations = 0;
            {
                vc::test::ScopedAllocationGuard guard;

                run (ms20 * 5);                                        // OFF（Resting）
                params.eqEnabled.store (true);                         // Resting → FadingIn → Active
                run (settle);

                for (int step = 0; step <= 60; ++step)                 // 掃引（周波数・ゲイン・Q）。毎回係数が再計算される
                {
                    for (int b = 0; b < vc::kEqBands; ++b)
                    {
                        params.eqBands[(size_t) b].hz.store (20.0f * std::pow (1000.0f, (float) ((step + b * 10) % 61) / 60.0f));
                        params.eqBands[(size_t) b].gainDb.store (-18.0f + 36.0f * (float) ((step * 7 + b * 13) % 61) / 60.0f);
                        params.eqBands[(size_t) b].q.store (0.1f * std::pow (100.0f, (float) ((step * 3 + b * 5) % 61) / 60.0f));
                    }

                    run (ms20 / 2);
                }

                for (int round = 0; round < 3; ++round)                // タイプ変更（素通しへ → 切替 → フィルタ状態リセット → 戻す）
                    for (const auto type : types)
                    {
                        for (auto& b : params.eqBands)
                            b.type.store ((int) type);

                        run (ms20 * 3);
                    }

                for (int b = 0; b < vc::kEqBands; ++b)                 // バンドの有効/無効（フェード中の反転を含む）
                {
                    params.eqBands[(size_t) b].on.store (false);
                    run (ms20 / 2);
                    params.eqBands[(size_t) b].on.store (true);
                    run (ms20 / 2);
                    params.eqBands[(size_t) b].on.store (false);
                    run (settle);
                    params.eqBands[(size_t) b].on.store (true);
                    run (settle);
                }

                params.eqEnabled.store (false);                        // Active → FadingOut → Resting
                run (ms20 / 2);
                params.eqEnabled.store (true);                         // FadingOut → FadingIn（反転）
                run (ms20 / 2);
                params.eqEnabled.store (false);
                run (settle);
                params.eqEnabled.store (true);
                run (settle);

                params.eqBands[0].hz.store (std::numeric_limits<float>::quiet_NaN()); // 非有限値・不正なタイプも安全に扱う
                params.eqBands[1].gainDb.store (std::numeric_limits<float>::infinity());
                params.eqBands[2].q.store (-std::numeric_limits<float>::infinity());
                params.eqBands[3].type.store (1234);
                run (settle);

                run (ms20, true);                                      // NaN注入 → 全体リセット → 再開
                run (settle);
                params.enabled.store (false);                          // バイパス中もEQは動く
                run (settle);
                run (ms20, true);
                run (settle);

                allocations = guard.count();
            }

            expectEquals ((int) allocations, 0, "allocations at " + juce::String (fs, 0) + " Hz");
        }
    }

    // ----- EQ9: CPU（参考値。失敗判定なし） -----
    // 48kHz・480ブロック・10秒で、全OFF / EQのみON / ノイズ除去のみON（N12と同条件: 背景70%・インパクト50%）/ ノイズ除去＋EQ ON /
    // （参考）EQのみONで、毎ブロック全バンドのパラメータを動かし続けたとき。2回測って小さい方を採る。
    // EQの設定は、ゲインを付けた5バンド（係数が定常のときの通常の使用）。
    void runEq9()
    {
        beginTest ("EQ9 (reference): CPU cost of 10 s of audio at 480-sample blocks: all OFF / EQ ON / noise reduction ON / noise reduction + EQ ON / EQ with parameters sweeping (never fails)");

        struct Case { double fs; vc::Preset preset; };

        for (const Case c : { Case { kFs, vc::Preset::Normal }, Case { kFs, vc::Preset::Talkbox }, Case { kFs, vc::Preset::Minion },
                              Case { 44100.0, vc::Preset::Normal } })
        {
            const int n = (int) (10.0 * c.fs);
            auto signal = vc::test::makeSpeechLikeVowel (140.0, c.fs, n, 0.3f);
            const auto pink = vc::test::makePinkNoise (n, 0.03f);

            for (size_t i = 0; i < signal.size(); ++i)
                signal[i] += pink[i];

            constexpr int modes = 5; // 0: 全OFF, 1: EQ, 2: ノイズ除去, 3: ノイズ除去＋EQ, 4: EQ（パラメータ掃引）
            double percent[modes] = { 1.0e9, 1.0e9, 1.0e9, 1.0e9, 1.0e9 };
            double maxBlockMs[modes] = {};

            for (int repeat = 0; repeat < 2; ++repeat)
            {
                for (int mode = 0; mode < modes; ++mode)
                {
                    vc::Engine engine;
                    engine.prepare ({ c.fs, 480 });
                    engine.params().preset.store ((int) c.preset);
                    engine.params().nrEnabled.store (mode == 2 || mode == 3);
                    engine.params().nrBackground.store (0.7f);
                    engine.params().nrImpact.store (0.5f);
                    engine.params().eqEnabled.store (mode == 1 || mode >= 3);

                    const float gains[vc::kEqBands] = { 3.0f, -2.0f, 4.0f, -3.0f, 2.0f };

                    for (int b = 0; b < vc::kEqBands; ++b)
                        engine.params().eqBands[(size_t) b].gainDb.store (gains[b]);

                    auto data = signal;
                    double totalMs = 0.0;
                    double maxMs = 0.0;
                    int block = 0;

                    for (int pos = 0; pos + 480 <= n; pos += 480, ++block)
                    {
                        if (mode == 4)
                            for (int b = 0; b < vc::kEqBands; ++b)
                            {
                                engine.params().eqBands[(size_t) b].gainDb.store (gains[b] + ((block + b) % 2 == 0 ? 1.0f : -1.0f));
                                engine.params().eqBands[(size_t) b].hz.store (vc::kEqDefaults[(size_t) b].hz * (1.0f + 0.1f * (float) ((block + b) % 3)));
                            }

                        const auto t0 = std::chrono::steady_clock::now();
                        engine.process (data.data() + pos, 480);
                        const auto t1 = std::chrono::steady_clock::now();
                        const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
                        totalMs += ms;
                        maxMs = std::max (maxMs, ms);
                    }

                    percent[mode] = std::min (percent[mode], 100.0 * totalMs / 10000.0);
                    maxBlockMs[mode] = maxMs;
                }
            }

            logMessage ("EQ9 " + juce::String (c.fs, 0) + " Hz " + juce::String (vc::kPresets[(size_t) c.preset].id) + ": OFF "
                        + juce::String (percent[0], 3) + " %, EQ ON " + juce::String (percent[1], 3) + " % (increase " + juce::String (percent[1] - percent[0], 3)
                        + " points), noise reduction ON " + juce::String (percent[2], 3) + " % (increase " + juce::String (percent[2] - percent[0], 3)
                        + "), noise reduction + EQ ON " + juce::String (percent[3], 3) + " % (increase " + juce::String (percent[3] - percent[0], 3)
                        + " points, of which EQ " + juce::String (percent[3] - percent[2], 3) + "), EQ with sweeping parameters " + juce::String (percent[4], 3)
                        + " % (increase " + juce::String (percent[4] - percent[0], 3) + "), max block " + juce::String (maxBlockMs[3], 3) + " ms");
        }
    }
};

static EqualizerTests equalizerTests;

// ===== SECTION: RecordingToolTests（T-014 M1） =====
// 実録音比較ツール（tests/RecordingTool.cpp）の指標を、既知の合成信号（雑音床・発話レベル・打撃音のピーク・EQの形が構成から決まる）で確かめる。
class RecordingToolTests final : public juce::UnitTest
{
public:
    RecordingToolTests() : juce::UnitTest ("RecordingTool", "Mic") {}

    void runTest() override
    {
        constexpr double fs = 48000.0;
        constexpr int n = 10 * 48000;
        constexpr double kTol = 0.5; // M1の許容 [dB]
        constexpr int kSonarLag = 96, kOursLag = 1440;

        // 帯域の中心周波数（1000Hz×2^((k-12)/3)）の3つの正弦（315Hz・1000Hz・3150Hz）に、周期にならない緩やかな包絡を掛けた「発話」。
        auto tone = [&] (int k, double gain, int i0, int len)
        {
            std::vector<float> v ((size_t) n, 0.0f);
            const double hz = 1000.0 * std::pow (2.0, (k - 12) / 3.0);

            for (int i = 0; i < len; ++i)
            {
                const double t = (double) i / fs;
                const double env = 0.8 + 0.12 * std::sin (2.0 * juce::MathConstants<double>::pi * 1.3 * t) + 0.08 * std::sin (2.0 * juce::MathConstants<double>::pi * 2.9 * t + 1.0);
                v[(size_t) (i0 + i)] = (float) (gain * env * std::sin (2.0 * juce::MathConstants<double>::pi * hz * (double) (i0 + i) / fs));
            }

            return v;
        };

        const int speechStart = 3 * 48000, speechLen = 3 * 48000, clickStart = (int) (8.5 * fs);
        std::array<std::vector<float>, 3> parts { tone (7, 1.0, speechStart, speechLen), tone (12, 1.0, speechStart, speechLen), tone (17, 1.0, speechStart, speechLen) };

        // 3つの正弦の和のRMSを-20dBFS（0.1）にする係数。
        std::vector<float> sum ((size_t) n, 0.0f);

        for (const auto& p : parts)
            for (int i = 0; i < n; ++i)
                sum[(size_t) i] += p[(size_t) i];

        const double norm = 0.1 / vc::test::rms (sum.data() + speechStart, speechLen);

        auto build = [&] (double toneGain0, double toneGain1, double toneGain2, double overall, double noiseDb, int seed, int lag)
        {
            std::vector<float> x = makeWhiteNoise (n, 1.0f, seed);
            vc::test::normalizeRms (x, (float) std::pow (10.0, noiseDb / 20.0));

            const double g[3] = { toneGain0, toneGain1, toneGain2 };

            for (int k = 0; k < 3; ++k)
                for (int i = 0; i < n; ++i)
                    x[(size_t) i] += (float) (overall * norm * g[k] * parts[(size_t) k][(size_t) i]);

            std::vector<float> click ((size_t) n, 0.0f);
            vc::test::addClick (click, clickStart, fs, 3000.0, 4.0, (float) overall * 0.316228f);

            for (int i = 0; i < n; ++i)
                x[(size_t) i] += click[(size_t) i];

            x.insert (x.begin(), (size_t) lag, 0.0f); // 遅らせる（先頭に無音を足して長さは保つ）
            x.resize ((size_t) n);
            return std::make_pair (x, vc::test::peakAbs (click.data(), n));
        };

        const auto raw = build (1.0, 1.0, 1.0, 1.0, -60.0, 1, 0);
        const auto sonar = build (1.0, 2.0, 0.5, 1.0, -80.0, 2, kSonarLag);   // 帯域ごとに 0 / +6.02 / -6.02dB、雑音床は別（-80dBFS）
        const auto ours = build (1.0, 1.0, 1.0, 0.5, -60.0 + 20.0 * std::log10 (0.5), 1, kOursLag); // 雑音も発話も-6.02dB、遅れ1440サンプル（ノイズ除去の遅延と同じ）

        const auto rawSpeechOnly = vc::test::rms (sum.data() + speechStart, speechLen) * norm;
        expectWithinAbsoluteError (20.0 * std::log10 (rawSpeechOnly), -20.0, 0.001);

        // 期待値。雑音床とピークは構成から、発話レベルは各正弦の和の実際のRMS（雑音は無視できる大きさ）から求める。
        std::vector<float> sonarSpeech ((size_t) n, 0.0f);

        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < n; ++i)
                sonarSpeech[(size_t) i] += (float) (norm * (k == 0 ? 1.0 : k == 1 ? 2.0 : 0.5) * parts[(size_t) k][(size_t) i]);

        const double rawSpeechDb = -20.0;
        const double sonarSpeechDb = 20.0 * std::log10 (vc::test::rms (sonarSpeech.data() + speechStart, speechLen));
        const double clickDb = 20.0 * std::log10 (raw.second);

        const std::vector<vc::rectool::Segment> script {
            { 0.0, 3.0, vc::rectool::SegKind::Silence },
            { 3.0, 6.0, vc::rectool::SegKind::Speech },
            { 6.0, 8.0, vc::rectool::SegKind::Silence },
            { 8.45, 8.6, vc::rectool::SegKind::Impact },
        };

        for (int mode = 0; mode < 2; ++mode) // 0: 区間ファイル（台本）、1: 自動判定
        {
            const juce::String label = mode == 0 ? "M1 (segments given): " : "M1 (auto segments): ";
            beginTest (mode == 0 ? "M1a: known synthetic recording, metrics within 0.5 dB (segments given)" : "M1b: known synthetic recording, metrics within 0.5 dB (auto segments)");

            const auto r = vc::rectool::compareRecordings (raw.first, sonar.first, ours.first, fs, mode == 0 ? &script : nullptr);
            const auto& fr = r.files[0];
            const auto& fso = r.files[1];
            const auto& fou = r.files[2];

            // 位置合わせ（相互相関）: サンプル単位で一致する。
            expectEquals (fso.alignment.lagSamples, kSonarLag, label + "sonar lag");
            expectEquals (fou.alignment.lagSamples, kOursLag, label + "ours lag");
            expect (fso.alignment.confident && fou.alignment.confident, label + "alignment not confident");
            expect (fso.sharesSegments, label + "sonar did not share the segments");

            // 無声区間の残留雑音（構成: -60 / -80 / -60-6.02）。
            expectWithinAbsoluteError (fr.silenceRmsDb, -60.0, kTol);
            expectWithinAbsoluteError (fso.silenceRmsDb, -80.0, kTol);
            expectWithinAbsoluteError (fou.silenceRmsDb, -60.0 + 20.0 * std::log10 (0.5), kTol);
            expectWithinAbsoluteError (fr.silenceMedianFrameDb, -60.0, kTol);
            expectWithinAbsoluteError (fr.silenceDigitalFraction, 0.0, 1.0e-9);

            // 発話区間のレベル・発話と残留雑音の比。
            expectWithinAbsoluteError (fr.speechRmsDb, rawSpeechDb, kTol);
            expectWithinAbsoluteError (fso.speechRmsDb, sonarSpeechDb, kTol);
            expectWithinAbsoluteError (fou.speechRmsDb, rawSpeechDb + 20.0 * std::log10 (0.5), kTol);
            expectWithinAbsoluteError (fr.snrDb(), 40.0, kTol);
            expectWithinAbsoluteError (fso.snrDb(), sonarSpeechDb + 80.0, kTol);
            expectWithinAbsoluteError (fou.snrDb(), 40.0, kTol);

            // 打撃音のピーク（1つだけ見つかること）。
            int impacts = 0;

            for (size_t i = 0; i < fr.segments.size(); ++i)
                if (fr.segments[i].kind == vc::rectool::SegKind::Impact)
                {
                    ++impacts;
                    expectWithinAbsoluteError (fr.segmentStats[i].peakDb, clickDb, kTol);
                    expectWithinAbsoluteError (fso.segmentStats[i].peakDb, clickDb, kTol);
                    expectWithinAbsoluteError (fou.segmentStats[i].peakDb, clickDb - 6.0206, kTol);
                }

            expectEquals (impacts, 1, label + "number of impact segments");

            // 発話区間の長時間平均スペクトル: 各正弦の帯域（315Hz・1000Hz・3150Hz = 帯域7・12・17）の差。
            expect (fr.speechBandsValid && fso.speechBandsValid && fou.speechBandsValid, label + "no speech spectrum");
            const int bands[3] = { 7, 12, 17 };
            const double sonarExpected[3] = { 0.0, 6.0206, -6.0206 };

            for (int k = 0; k < 3; ++k)
            {
                const auto b = (size_t) bands[k];
                expectWithinAbsoluteError (fso.speechBands[b] - fr.speechBands[b], sonarExpected[k], kTol);
                expectWithinAbsoluteError (fou.speechBands[b] - fr.speechBands[b], -6.0206, kTol);
            }

            // 無声区間のスペクトル: 白色雑音のパワーは帯域幅に比例する。全帯域を足すと（63Hz〜8kHz、22帯域）ほぼ雑音全体の (8000*2^(1/6) - 63/2^(1/6))/24000。
            double total = 0.0;

            for (int b = 0; b < vc::rectool::kNumBands; ++b)
                total += std::pow (10.0, fr.silenceBands[(size_t) b] / 10.0);

            const double expectedFraction = (8000.0 * std::pow (2.0, 1.0 / 6.0) - 63.0 / std::pow (2.0, 1.0 / 6.0)) / 24000.0;
            expectWithinAbsoluteError (10.0 * std::log10 (total), -60.0 + 10.0 * std::log10 (expectedFraction), kTol);

            // K特性の重み付け（発話区間）: oursは-6.02dB。sonarは3つの正弦の帯域ごとのK特性の利得（下の式）で決まるパワー比。
            expectWithinAbsoluteError (fou.speechKDb - fr.speechKDb, -6.0206, kTol);
            expect (fr.speechKDb > fr.speechRmsDb, "K-weighted power below the unweighted RMS for a signal with 1k and 3k content");

            expect (! vc::rectool::formatReport (r).isEmpty(), "report is empty");
        }

        beginTest ("M1c: K-weighting matches the BS.1770 gain (100 Hz -1.13 dB, 1 kHz +0.70 dB, 10 kHz +4.04 dB at 48 kHz)");
        {
            const double gains[3][2] = { { 100.0, -1.1335 }, { 1000.0, 0.6977 }, { 10000.0, 4.0419 } };

            for (auto& g : gains)
            {
                const auto sine = vc::test::makeSine (g[0], fs, 48000, 0.5f);
                const double rmsDb = 20.0 * std::log10 (vc::test::rms (sine.data() + 4800, 48000 - 4800));
                std::vector<float> settled (sine.begin() + 4800, sine.end()); // フィルタの過渡（先頭0.1秒）を除く
                expectWithinAbsoluteError (vc::rectool::kWeightedLevelDb (settled, fs) - rmsDb, g[1], 0.05, juce::String (g[0], 0) + " Hz");
            }
        }

        beginTest ("M1d: an unrelated take is not aligned and gets its own segments; a digital-silence take is reported as such");
        {
            auto unrelated = makeWhiteNoise (n, 1.0f, 77);
            vc::test::normalizeRms (unrelated, (float) std::pow (10.0, -50.0 / 20.0));
            const auto r = vc::rectool::compareRecordings (raw.first, unrelated, ours.first, fs, nullptr);
            expect (! r.files[1].alignment.confident, "unrelated noise was aligned (correlation " + juce::String (r.files[1].alignment.correlation, 3) + ")");
            expect (! r.files[1].sharesSegments, "unrelated take shared the segments");
            expect (r.files[2].alignment.confident, "ours lost its alignment");

            const std::vector<float> zeros ((size_t) n, 0.0f);
            const auto z = vc::rectool::compareRecordings (raw.first, zeros, ours.first, fs, &script);
            expect (z.files[1].silenceRmsDb < vc::rectool::kDigitalSilenceDb, "digital silence not detected");
            expectWithinAbsoluteError (z.files[1].silenceDigitalFraction, 1.0, 1.0e-9);
            expect (! vc::rectool::formatReport (z).contains ("nan"), "report contains nan");
        }

        beginTest ("M1e: segments file parser accepts comments and rejects bad lines");
        {
            std::vector<vc::rectool::Segment> segs;
            juce::String error;
            expect (vc::rectool::parseSegments ("# script\n0 10 silence\n10.5, 40 speech # talking\n\n45 45.2 impact\n50 60 speech+impact\n", segs, error), error);
            expectEquals ((int) segs.size(), 4);
            expect (segs.size() == 4 && segs[1].kind == vc::rectool::SegKind::Speech && std::abs (segs[1].startSec - 10.5) < 1.0e-9 && segs[3].kind == vc::rectool::SegKind::SpeechImpact,
                    "parsed values");
            expect (! vc::rectool::parseSegments ("0 10 noise\n", segs, error), "unknown kind accepted");
            expect (! vc::rectool::parseSegments ("10 5 silence\n", segs, error), "reversed times accepted");
            expect (! vc::rectool::parseSegments ("1 2\n", segs, error), "missing kind accepted");
            expect (! vc::rectool::parseSegments ("# only a comment\n", segs, error), "empty file accepted");
        }

        beginTest ("M1f: 16-bit WAV round trip (sample rate, length, 1 LSB, clipping)");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("vc_m1", ".wav");
            std::vector<float> x (raw.first.begin() + speechStart, raw.first.begin() + speechStart + 4800);
            x[10] = 1.5f;
            x[11] = -1.5f;
            juce::String error;
            expect (vc::rectool::writeWav16 (file, x, 44100.0, error), error);
            vc::rectool::Audio back;
            expect (vc::rectool::readWav (file, back, error), error);
            expectWithinAbsoluteError (back.sampleRate, 44100.0, 1.0e-6);
            expectEquals ((int) back.samples.size(), (int) x.size());

            double worst = 0.0;

            for (size_t i = 0; i < std::min (x.size(), back.samples.size()); ++i)
                if (i != 10 && i != 11)
                    worst = std::max (worst, (double) std::abs (back.samples[i] - x[i]));

            expect (worst <= 1.0 / 32768.0, "round trip error " + juce::String (worst * 32768.0, 3) + " LSB");
            expect (back.samples.size() > 11 && back.samples[10] > 0.9999f && back.samples[10] <= 1.0f && back.samples[11] < -0.9999f && back.samples[11] >= -1.0f, "clipping");

            // float32: 測定用。値が完全に一致し、16bitの量子化雑音を持たない（-120dBFSの信号が残る）。
            std::vector<float> tiny (480, 1.0e-6f);
            expect (vc::rectool::writeWav (file, tiny, 48000.0, true, error), error);
            vc::rectool::Audio tinyBack;
            expect (vc::rectool::readWav (file, tinyBack, error), error);
            expect (tinyBack.samples.size() == tiny.size() && std::abs (tinyBack.samples[100] - 1.0e-6f) < 1.0e-9f, "float32 WAV round trip");

            // 上書き: 短いデータで書き直すと、古い内容が残らない。
            expect (vc::rectool::writeWav16 (file, std::vector<float> (100, 0.25f), 48000.0, error), error);
            vc::rectool::Audio shorter;
            expect (vc::rectool::readWav (file, shorter, error), error);
            expectEquals ((int) shorter.samples.size(), 100);
            file.deleteFile();
        }

        beginTest ("M1g: processAudio reports the noise reduction latency and returns an output aligned with the input");
        {
            const int len = 4 * 48000;
            auto speech = vc::test::makeSyllables (len);
            vc::test::normalizeRms (speech, 0.05f);
            const auto noise = vc::test::makePinkNoise (len, 0.002f, 5);

            for (int i = 0; i < len; ++i)
                speech[(size_t) i] += noise[(size_t) i];

            vc::rectool::ProcessSettings on;
            on.nrBackground = 0.7f;
            const auto r = vc::rectool::processAudio (speech, fs, on);
            expectEquals (r.latencySamples, kOursLag);
            expectEquals ((int) r.output.size(), len);
            expectEquals ((int) r.errorFlags, 0);
            expect (vc::test::allFinite (r.output.data(), len), "non-finite output");
            const auto al = vc::rectool::measureAlignment (speech, r.output, fs);
            expect (std::abs (al.lagSamples) <= 1 && al.correlation > 0.7, "output not aligned: lag " + juce::String (al.lagSamples) + ", correlation " + juce::String (al.correlation, 3));

            vc::rectool::ProcessSettings off;
            off.nrEnabled = false;
            const auto r0 = vc::rectool::processAudio (speech, fs, off);
            expectEquals (r0.latencySamples, 0);
            double worst = 0.0;

            for (int i = 0; i < len; ++i)
                worst = std::max (worst, (double) std::abs (r0.output[(size_t) i] - speech[(size_t) i]));

            expect (worst < 1.0e-6, "noise reduction and EQ off changed the signal by " + juce::String (worst, 8));

            // EQのプリセット: a2・a3は5バンドを置き換えてONにする。
            vc::rectool::ProcessSettings eq;
            vc::rectool::applyEqPreset (eq, vc::rectool::EqPreset::A2);
            expect (eq.eqEnabled && eq.eqBands[4].type == vc::EqType::HighShelf && std::abs (eq.eqBands[4].hz - 5741.0f) < 1.0e-3f, "a2");
            vc::rectool::applyEqPreset (eq, vc::rectool::EqPreset::A3);
            expect (eq.eqEnabled && std::abs (eq.eqBands[2].hz - 400.0f) < 1.0e-3f && std::abs (eq.eqBands[3].gainDb - 5.0f) < 1.0e-3f, "a3");
            vc::rectool::applyEqPreset (eq, vc::rectool::EqPreset::Sonar);
            expect (eq.eqEnabled && std::abs (eq.eqBands[2].hz - 546.0f) < 1.0e-3f && std::abs (eq.eqBands[4].gainDb + 2.3f) < 1.0e-3f, "sonar");
            vc::rectool::applyEqPreset (eq, vc::rectool::EqPreset::Off);
            expect (! eq.eqEnabled, "off");
        }
    }
};

static RecordingToolTests recordingToolTests;

} // namespace
