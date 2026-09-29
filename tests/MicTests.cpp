#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"

#include <rnnoise.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

// ===== SECTION: MicTests =====
// マイク処理のテスト（カテゴリMic、quick）。現在はT-009のN0a〜N0e（RNNoiseそのものの確認）のみ。
// N1以降とEQはT-010以降で追加する（docs/plan.md 8.3）。
// RNNoiseの入出力はint16の値域のfloat（±32768）、1フレーム480サンプル（48kHzで10ms）。

namespace
{

constexpr double kFs = 48000.0;
constexpr int kFrame = 480;

// 話し声に近い合成信号（N0c・N0e用）: f0 110〜140Hz・ビブラート±2%（5Hz）の母音（フォルマント700/1200/2600Hz）を、
// 長さの異なる音節（半波の包絡）と無音を交互に並べる。包絡が不規則でf0が揺れるため、相互相関のピークが
// 真の遅延の1点にだけ立つ（f0の周期ずれや包絡の周期性による誤ったピークを避ける）。値はint16の値域。
std::vector<float> makeSyllables (int n)
{
    static constexpr int kSyllableLengths[] = { 7200, 4800, 9600, 3600, 8400, 6000, 10800, 5400, 7800, 4200, 9000, 6600 };
    constexpr double twoPi = 6.283185307179586476925286766559;

    std::vector<float> out (static_cast<size_t> (n), 0.0f);
    double phase = 0.0;
    int pos = 0;
    int syllable = 0;

    while (pos < n)
    {
        const int len = kSyllableLengths[syllable % 12];
        const double f0Base = 110.0 + 15.0 * (syllable % 3);
        ++syllable;

        for (int i = 0; i < len && pos + i < n; ++i)
        {
            const double t = (pos + i) / kFs;
            const double f0 = f0Base * (1.0 + 0.02 * std::sin (twoPi * 5.0 * t));
            phase += twoPi * f0 / kFs;

            double s = 0.0;

            for (int h = 1; h <= 30; ++h)
            {
                const double f = h * f0;
                const double g = std::exp (-std::pow ((f - 700.0) / 300.0, 2.0))
                                 + 0.6 * std::exp (-std::pow ((f - 1200.0) / 400.0, 2.0))
                                 + 0.2 * std::exp (-std::pow ((f - 2600.0) / 600.0, 2.0));
                s += g * std::sin (h * phase);
            }

            out[static_cast<size_t> (pos + i)] = static_cast<float> (4000.0 * s * std::sin (3.14159265358979323846 * i / len));
        }

        pos += len + 2400; // 音節の後に50msの無音
    }

    return out;
}

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
            const auto in = makeSyllables (n);
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
                const auto in = makeSyllables (kFrame * 50);
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
            // 陽性対照: Cのmallocがガード中に数えられること（数えられないと上の0回が意味を持たない）。
            {
                std::size_t mallocCount = 0;

                {
                    vc::test::ScopedAllocationGuard guard;
                    // 関数ポインタ経由にして、コンパイラがmalloc/freeの対を除去できないようにする。
                    void* (*volatile allocate) (std::size_t) = std::malloc;
                    void (*volatile release) (void*) = std::free;
                    release (allocate (64));
                    mallocCount = guard.count();
                }

                expect (mallocCount >= 2, "C malloc/free were not counted by the guard: " + juce::String ((int) mallocCount));
            }
#endif
        }

        beginTest ("N0e: CPU cost of 10 s of 48 kHz audio (reference only, never fails)");
        {
            const int n = static_cast<int> (10.0 * kFs);
            auto in = makeSyllables (n);
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

} // namespace
