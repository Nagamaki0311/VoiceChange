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

            if (judged)
                expect (worst <= -12.0, "background " + juce::String ((int) (bg * 100)) + " %: worst click peak change " + juce::String (worst, 2) + " dB");
        }
    }

    // ----- N5b: 誤検出 -----
    // (a) 立ち上がり10msの母音（無音から）、(b) 合成の破裂音（5msの雑音バースト、その30ms後に立ち上がり10msの母音）。
    // インパクト100%でも、母音の立ち上がりから80msの区間のエネルギーの減衰がインパクト0%比で1dB以下。f0 = 100/140Hz、背景70%・100%。
    void runN5b()
    {
        const int n = (int) (6.0 * kFs);
        const int vowelLen = (int) (0.08 * kFs);

        for (const int kind : { 0, 1 })
        {
            beginTest (juce::String ("N5b: ") + (kind == 1 ? "plosive (5 ms noise burst) + vowel 30 ms later" : "vowel with a 10 ms onset")
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

                    vc::test::addEnvelopedVowel (in, onsets.back(), f0, kFs, 0.3f, 10.0, 300.0, 5.0, 30.0);
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

                    const juce::String label = juce::String (kind == 1 ? "plosive + vowel" : "vowel onset") + ", f0 " + juce::String (f0, 0)
                                               + ", background " + juce::String ((int) (bg * 100)) + " %";
                    logMessage ("N5b " + label + ": vowel energy change at impact 100 % [dB]" + line + " (limit -1)");
                    expect (worst >= -1.0, label + ": vowel attenuated by " + juce::String (-worst, 2) + " dB");
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
            const auto r = runPresetSwitchWithNr (c.mode, 7, voice, c.bg, c.impact);
            logMessage ("N9b " + juce::String (c.mode == 0 ? "steady" : c.mode == 1 ? "onset(10ms attack) from digital silence" : "onset(10ms attack) after a pause with a -80 dBFS noise floor") + ", background " + juce::String ((int) (c.bg * 100)) + " %, impact "
                        + juce::String ((int) (c.impact * 100)) + " %: " + juce::String (r.runs - r.failures) + "/" + juce::String (r.runs)
                        + " switches passed, worst ratio (limit 1.5) " + juce::String (r.worstRatio, 3) + " at " + r.worstName);
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

} // namespace
