#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "AllocationGuard.h"
#include "TestSignals.h"

#include "core/Engine.h"
#include "core/MicProcessing.h"
#include "RecordingTool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// ===== SECTION: DeEsserTests =====
// ディエッサー（T-019、D-029。docs/spec.md「マイク処理: ディエッサー」）のテスト（カテゴリMic、quick）。
// 合成信号: 母音（makeSpeechLikeVowel）と、歯擦音の模擬（中心周波数・Q 1.5の帯域雑音のバースト）。
// 閾値は暫定の値ではなく仕様の値（既定 3400 Hz・6 dB、閾値+3 dB・幅6 dB、アタック1.5 ms・リリース50 ms）で、外れたら緩めずに実測値を報告する。

namespace
{

constexpr double kTwoPi = 6.283185307179586476925286766559;

double rmsDbOf (const std::vector<float>& x, int from, int to)
{
    return 20.0 * std::log10 (std::max (1.0e-12, vc::test::rms (x.data() + from, to - from)));
}

bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    return std::memcmp (a.data() + from, b.data() + from, sizeof (float) * (size_t) (to - from)) == 0;
}

// 中心fcHz・Q 1.5の帯域雑音。全体のRMSをrmsTargetへそろえる（フィルタの立ち上がりは読み捨てた後のRMS）。
std::vector<float> makeBandNoise (double fs, int n, double fcHz, float rmsTarget, int seed, double q = 1.5)
{
    juce::Random rng (seed);
    juce::dsp::IIR::Filter<float> filter;
    filter.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, (float) fcHz, (float) q);

    std::vector<float> out ((size_t) n);

    for (auto& s : out)
        s = filter.processSample (rng.nextFloat() * 2.0f - 1.0f);

    const int skip = std::min (n / 2, 2000);
    const double r = vc::test::rms (out.data() + skip, n - skip);

    for (auto& s : out)
        s = (float) ((double) s * (double) rmsTarget / r);

    return out;
}

// [from, to) に3 msの余弦の立ち上がり・立ち下がりを掛ける（端のクリックが広帯域のエネルギーを作らないように）。
void applyEdges (std::vector<float>& x, int from, int to, double fs)
{
    const int edge = std::max (1, (int) std::lround (0.003 * fs));

    for (int i = from; i < to; ++i)
    {
        const double up = std::min (1.0, (double) (i - from) / (double) edge);
        const double down = std::min (1.0, (double) (to - 1 - i) / (double) edge);
        const double w = std::min (up, down);
        x[(size_t) i] = (float) ((double) x[(size_t) i] * (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * w)));
    }
}

// 先頭のrampMsを余弦で立ち上げる。合成の母音は先頭が非0から始まり、ステップがフィルタの過渡応答（広帯域のパルス）を作って比が一瞬跳ねるため。
void fadeIn (std::vector<float>& x, double fs, double rampMs = 20.0)
{
    const int len = std::max (1, (int) std::lround (rampMs * 1.0e-3 * fs));

    for (int i = 0; i < len && i < (int) x.size(); ++i)
        x[(size_t) i] = (float) ((double) x[(size_t) i] * (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (double) i / (double) len)));
}

// 母音 [0, 0.5) → 歯擦音のバースト [0.5, 0.7)（-20 dBFS RMS、中心burstHz）→ 母音 [0.7, 1.6)。母音と歯擦音は重ねない（サ行の発音に近い）。
struct Scene
{
    std::vector<float> in;
    int burstStart = 0, burstEnd = 0;
};

// burstQは歯擦音の帯域雑音のQ（大きいほど狭い。既定20は、ほぼ中心の周波数だけ＝中心の減衰量を測るための狭い帯域。1.5は実録音に近い広い帯域）。
Scene makeScene (double fs, double burstHz = 3400.0, float burstRmsDb = -20.0f, float vowelPeak = 0.3f, double burstQ = 20.0)
{
    Scene s;
    const int total = (int) std::lround (1.6 * fs);
    s.burstStart = (int) std::lround (0.5 * fs);
    s.burstEnd = (int) std::lround (0.7 * fs);

    s.in = vc::test::makeSpeechLikeVowel (140.0, fs, total, vowelPeak);
    fadeIn (s.in, fs);

    // 母音を burst の区間だけ無音にする（3 msの余弦で）。
    std::vector<float> gate ((size_t) total, 1.0f);
    for (int i = s.burstStart; i < s.burstEnd; ++i)
        gate[(size_t) i] = 0.0f;

    const int edge = std::max (1, (int) std::lround (0.003 * fs));
    for (int k = 0; k < edge; ++k)
    {
        const float w = (float) (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * (double) (k + 1) / (double) edge));
        gate[(size_t) (s.burstStart - edge + k)] = w;                 // 0.5 sの手前で下がる
        gate[(size_t) (s.burstEnd + edge - 1 - k)] = w;               // 0.7 sの後ろで上がる
    }

    for (int i = 0; i < total; ++i)
        s.in[(size_t) i] *= gate[(size_t) i];

    auto noise = makeBandNoise (fs, s.burstEnd - s.burstStart, burstHz, (float) std::pow (10.0, (double) burstRmsDb / 20.0), 4711, burstQ);
    applyEdges (noise, 0, (int) noise.size(), fs);

    for (size_t i = 0; i < noise.size(); ++i)
        s.in[(size_t) s.burstStart + i] += noise[i];

    return s;
}

// 一定の設定で、blockずつ処理した出力。traceにはブロックの終わりの減衰量[dB]を入れる。
std::vector<float> runDeEsser (vc::DeEsser& d, const std::vector<float>& in, bool on, float hz, float db, int block = 480, std::vector<float>* trace = nullptr)
{
    auto out = in;

    for (int pos = 0; pos < (int) out.size(); pos += block)
    {
        d.setTarget (on, hz, db);
        d.process (out.data() + pos, std::min (block, (int) out.size() - pos));

        if (trace != nullptr)
            trace->push_back (d.getReductionDb());
    }

    return out;
}

std::vector<float> runFresh (double fs, const std::vector<float>& in, bool on, float hz, float db, int block = 480, std::vector<float>* trace = nullptr)
{
    vc::DeEsser d;
    d.prepare (fs);
    return runDeEsser (d, in, on, hz, db, block, trace);
}

// 1サンプルずつ処理して、サンプルごとの減衰量[dB]を返す。
std::vector<float> traceReduction (double fs, const std::vector<float>& in, float hz, float db)
{
    vc::DeEsser d;
    d.prepare (fs);
    d.setTarget (true, hz, db);

    auto buf = in;
    std::vector<float> red (in.size());

    for (size_t i = 0; i < buf.size(); ++i)
    {
        d.process (buf.data() + i, 1);
        red[i] = d.getReductionDb();
    }

    return red;
}

// 周波数fHz（1 sの窓に整数周期入る値）の成分の振幅。
double toneAmplitude (const std::vector<float>& x, int from, int len, double fs, double fHz)
{
    double re = 0.0, im = 0.0;

    for (int i = 0; i < len; ++i)
    {
        const double ph = kTwoPi * fHz * (double) (from + i) / fs;
        re += (double) x[(size_t) (from + i)] * std::cos (ph);
        im += (double) x[(size_t) (from + i)] * std::sin (ph);
    }

    return 2.0 * std::hypot (re, im) / (double) len;
}

} // namespace

class DeEsserTests final : public juce::UnitTest
{
public:
    DeEsserTests() : juce::UnitTest ("DeEsser", "Mic") {}

    void runTest() override
    {
        runBurst();
        runThreshold();
        runReference();
        runFrequency();
        runDynamics();
        runOnOff();
        runOutOfBand();
        runBlockIndependence();
        runNonFiniteAndReset();
        runEngine();
        runTool();
        runCpu();
    }

private:
    // ----- DE1: 歯擦音のバーストは最大減衰付近まで下がり、母音は変わらない（全サンプルレート・最大減衰6 dBと12 dB） -----
    void runBurst()
    {
        beginTest ("DE1: the sibilant burst is reduced to about the maximum attenuation (6 / 12 dB), vowels are bit-identical (44.1-192 kHz)");

        juce::String table = "DE1 burst (RMS change of the narrow burst [dB] / peak reduction [dB]):";

        for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const auto scene = makeScene (fs);

            for (const float maxDb : { 6.0f, 12.0f })
            {
                std::vector<float> trace;
                const auto out = runFresh (fs, scene.in, true, 3400.0f, maxDb, 480, &trace);
                const juce::String label = juce::String (fs, 0) + " Hz, max " + juce::String (maxDb, 0) + " dB: ";

                const int from = scene.burstStart + (int) (0.04 * fs), to = scene.burstEnd - (int) (0.002 * fs);
                const double change = rmsDbOf (out, from, to) - rmsDbOf (scene.in, from, to);
                // 帯域雑音の包絡の揺れで、ブロックの終わりの瞬間の減衰量は最大を少し下回りうる。バースト中（立ち上がり後）の最大が最大減衰に届き、超えないこと。
                const int firstBlock = (scene.burstStart + (int) (0.04 * fs)) / 480, lastBlock = (scene.burstEnd - 1) / 480;
                const double steady = (double) *std::max_element (trace.begin() + firstBlock, trace.begin() + lastBlock + 1);

                table << "\n  " << (juce::String (fs, 0) + " Hz max " + juce::String (maxDb, 0)).paddedRight (' ', 20) << juce::String (change, 2) << " / " << juce::String (steady, 2);

                expect (steady >= (double) maxDb - 0.05 && steady <= (double) maxDb + 1.0e-3, label + "peak reduction " + juce::String (steady, 3) + " dB is not the maximum");
                expect (change <= -0.65 * (double) maxDb && change >= -(double) maxDb - 0.3, label + "burst RMS change " + juce::String (change, 2) + " dB");
                expect (bitIdentical (out, scene.in, 0, scene.burstStart - (int) (0.01 * fs)), label + "the vowel before the burst is modified");
                expect (bitIdentical (out, scene.in, (int) (1.3 * fs), (int) scene.in.size()), label + "the vowel long after the burst is modified (gain did not return to exactly 1)");

                // 歯擦音の後の母音（リリース中）。帯域外（1 kHz以下）は変わらず、全体のレベルもほぼ変わらない。
                const int afterFrom = scene.burstEnd + (int) (0.06 * fs), afterTo = scene.burstEnd + (int) (0.3 * fs);
                expect (std::abs (rmsDbOf (out, afterFrom, afterTo) - rmsDbOf (scene.in, afterFrom, afterTo)) < 0.35,
                        label + "the vowel after the burst changed by " + juce::String (rmsDbOf (out, afterFrom, afterTo) - rmsDbOf (scene.in, afterFrom, afterTo), 3) + " dB");
            }
        }

        logMessage (table);

        // 参考（判定なし）: 実録音に近い広い帯域（Q 1.5）の歯擦音。中心の1/3オクターブは最大減衰近くまで下がるが、帯域全体のRMSの変化は小さい。
        {
            const auto wide = makeScene (48000.0, 3400.0, -20.0f, 0.3f, 1.5);
            const auto out = runFresh (48000.0, wide.in, true, 3400.0f, 6.0f);
            const int from = wide.burstStart + 1920, to = wide.burstEnd - 96;
            logMessage ("DE1 reference: a wide (Q 1.5) burst changes by " + juce::String (rmsDbOf (out, from, to) - rmsDbOf (wide.in, from, to), 2) + " dB in RMS at max 6 dB");
        }
    }

    // ----- DE2: 比の閾値の境界（定常の2音: 3.4 kHz = 歯擦音の帯域、300 Hz = 基準の帯域） -----
    void runThreshold()
    {
        beginTest ("DE2: ratio threshold - no reduction up to +2 dB, partial inside the 6 dB knee, full above +9 dB, independent of the absolute level");

        constexpr double fs = 48000.0;
        const int n = (int) fs;

        const auto reductionFor = [&] (double ratioDb, double refAmp, float maxDb, bool* identical = nullptr)
        {
            const double sibAmp = refAmp * std::pow (10.0, ratioDb / 20.0);
            std::vector<float> in ((size_t) n);

            for (int i = 0; i < n; ++i)
                in[(size_t) i] = (float) (refAmp * std::sin (kTwoPi * 300.0 * (double) i / fs) + sibAmp * std::sin (kTwoPi * 3400.0 * (double) i / fs));

            vc::DeEsser d;
            d.prepare (fs);
            const auto out = runDeEsser (d, in, true, 3400.0f, maxDb);

            if (identical != nullptr)
                *identical = bitIdentical (out, in, n / 2, n);

            return (double) d.getReductionDb();
        };

        double previous = -1.0;
        juce::String table = "DE2 steady reduction [dB] (max 6 dB, 300 Hz tone 0.05 + 3.4 kHz tone at the ratio):";

        for (const double ratio : { -20.0, -10.0, 0.0, 2.0, 4.0, 6.0, 8.0, 9.5, 12.0, 20.0 })
        {
            bool identical = false;
            const double red = reductionFor (ratio, 0.05, 6.0f, &identical);
            table << "\n  " << juce::String (ratio, 1).paddedLeft (' ', 6) << " dB -> " << juce::String (red, 3);

            if (ratio <= 2.0)
            {
                expect (red < 1.0e-9, "ratio " + juce::String (ratio, 1) + " dB must not reduce (reduction " + juce::String (red, 4) + " dB)");
                expect (identical, "ratio " + juce::String (ratio, 1) + " dB: the output is not bit-identical to the input");
            }

            expect (red >= previous - 1.0e-6, "the reduction is not monotonic in the ratio (" + juce::String (ratio, 1) + " dB)");
            previous = red;

            if (ratio >= 9.5)
                expect (std::abs (red - 6.0) < 0.1, "ratio " + juce::String (ratio, 1) + " dB must reach the maximum, got " + juce::String (red, 3));
        }

        logMessage (table);

        const double half = reductionFor (6.0, 0.05, 6.0f);
        expect (half > 0.3 * 6.0 && half < 0.65 * 6.0, "ratio +6 dB is not about half way through the knee: " + juce::String (half, 3) + " dB");

        // 最大減衰に比例する（強さ0 dBでは何も下げない）。
        expect (reductionFor (20.0, 0.05, 0.0f) < 1.0e-9, "max 0 dB must not reduce");
        expect (std::abs (reductionFor (20.0, 0.05, 12.0f) - 12.0) < 0.1, "max 12 dB");
        expect (std::abs (reductionFor (20.0, 0.05, 3.5f) - 3.5) < 0.1, "max 3.5 dB");

        // 絶対レベルに依存しない: 基準の振幅を12 dB違えても、同じ比なら同じ減衰量。
        const double loud = reductionFor (6.0, 0.2, 6.0f);
        const double mid = reductionFor (6.0, 0.05, 6.0f);
        expect (std::abs (loud - mid) < 0.3, "the reduction depends on the absolute level: " + juce::String (loud, 3) + " vs " + juce::String (mid, 3) + " dB");
    }

    // ----- DE3: 検出の基準（低域との比）。比ベースであること: 小さな歯擦音は効く、大きな母音・低域が強い信号は効かない -----
    void runReference()
    {
        beginTest ("DE3: detection is a ratio to the low band - a quiet sibilant is reduced, loud vowels and low-heavy signals are untouched");

        constexpr double fs = 48000.0;

        // (a) 小さな歯擦音（-38 dBFS RMS）だけでも最大減衰付近まで下がる（絶対レベルの閾値ではない）。
        for (const float levelDb : { -20.0f, -38.0f })
        {
            const auto scene = makeScene (fs, 3400.0, levelDb, 0.05f);
            const auto out = runFresh (fs, scene.in, true, 3400.0f, 6.0f);
            const int from = scene.burstStart + (int) (0.04 * fs), to = scene.burstEnd - (int) (0.002 * fs);
            const double change = rmsDbOf (out, from, to) - rmsDbOf (scene.in, from, to);
            logMessage ("DE3 burst at " + juce::String (levelDb, 0) + " dBFS (narrow, max 6 dB): RMS change " + juce::String (change, 2) + " dB");
            expect (change <= -4.5, "burst at " + juce::String (levelDb, 0) + " dBFS: RMS change " + juce::String (change, 2) + " dB");
        }

        // (b) 大きな母音（ピーク0.9）。F3を3.4 kHzに置いた母音も、帯域/基準の比は閾値より十分低く、ビット一致。
        {
            const int n = (int) (1.5 * fs);
            auto loudVowel = vc::test::makeSpeechLikeVowel (140.0, fs, n, 0.9f);
            auto f3Vowel = vc::test::makeSyntheticVowel (140.0, fs, n, { 700.0, 1800.0, 3400.0 }, { 80.0, 100.0, 150.0 }, 0.5f);
            fadeIn (loudVowel, fs);
            fadeIn (f3Vowel, fs);

            for (const auto* v : { &loudVowel, &f3Vowel })
            {
                std::vector<float> trace;
                const auto out = runFresh (fs, *v, true, 3400.0f, 12.0f, 480, &trace);
                expect (bitIdentical (out, *v, 0, n), "a vowel was modified (max reduction " + juce::String (*std::max_element (trace.begin(), trace.end()), 3) + " dB)");
            }
        }

        // (c) 歯擦音の帯域雑音（-20 dBFS）に、それより10 dB強い200 Hzの音が重なる（有声の摩擦音・低域の強い信号）: 比が負なので何もしない。
        {
            const int n = (int) fs;
            auto in = makeBandNoise (fs, n, 3400.0, 0.1f, 99, 20.0);

            for (int i = 0; i < n; ++i)
                in[(size_t) i] += (float) (0.45 * std::sin (kTwoPi * 200.0 * (double) i / fs)); // RMS 0.32 = 約10 dB強い

            fadeIn (in, fs);
            vc::DeEsser d;
            d.prepare (fs);
            const auto out = runDeEsser (d, in, true, 3400.0f, 12.0f);
            expect (bitIdentical (out, in, 0, n), "band noise under a strong 200 Hz tone was reduced");
        }
    }

    // ----- DE4: 中心周波数への追従と範囲外の値 -----
    void runFrequency()
    {
        beginTest ("DE4: the reduction follows the centre frequency (2-9 kHz), is selective, and out-of-range / non-finite values are rounded");

        constexpr double fs = 48000.0;
        juce::String table = "DE4 burst RMS change [dB] (max 6 dB; rows: burst centre, columns: de-esser centre 2000 / 3400 / 6000 / 9000):";

        for (const double burstHz : { 2000.0, 3400.0, 6000.0, 9000.0 })
        {
            const auto scene = makeScene (fs, burstHz);
            const int from = scene.burstStart + (int) (0.04 * fs), to = scene.burstEnd - (int) (0.002 * fs);
            table << "\n  burst " + juce::String (burstHz, 0).paddedLeft (' ', 5) + " Hz:";

            for (const float centre : { 2000.0f, 3400.0f, 6000.0f, 9000.0f })
            {
                const auto out = runFresh (fs, scene.in, true, centre, 6.0f);
                const double change = rmsDbOf (out, from, to) - rmsDbOf (scene.in, from, to);
                table << " " << juce::String (change, 2).paddedLeft (' ', 6);

                if (std::abs ((double) centre - burstHz) < 1.0)
                    expect (change <= -4.5, "centre " + juce::String (centre, 0) + " Hz on a burst at the same frequency: change " + juce::String (change, 2) + " dB");
                else if (std::abs (std::log2 ((double) centre / burstHz)) >= 1.0)
                    expect (std::abs (change) < 1.0, "centre " + juce::String (centre, 0) + " Hz on a burst at " + juce::String (burstHz, 0) + " Hz (an octave or more away) changed by " + juce::String (change, 2) + " dB");
            }
        }

        logMessage (table);

        // 範囲外は端へ、非有限は既定へ（出力がビット一致）。
        const auto scene = makeScene (fs);
        const auto render = [&] (float hz, float db) { return runFresh (fs, scene.in, true, hz, db); };
        const int n = (int) scene.in.size();
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();

        expect (bitIdentical (render (100.0f, 6.0f), render (2000.0f, 6.0f), 0, n), "100 Hz is not rounded to 2000 Hz");
        expect (bitIdentical (render (50000.0f, 6.0f), render (9000.0f, 6.0f), 0, n), "50000 Hz is not rounded to 9000 Hz");
        expect (bitIdentical (render (nan, 6.0f), render (3400.0f, 6.0f), 0, n), "NaN centre is not replaced by 3400 Hz");
        expect (bitIdentical (render (-inf, 6.0f), render (3400.0f, 6.0f), 0, n), "-Inf centre is not replaced by 3400 Hz");
        expect (bitIdentical (render (3400.0f, 100.0f), render (3400.0f, 12.0f), 0, n), "100 dB is not rounded to 12 dB");
        expect (bitIdentical (render (3400.0f, -5.0f), render (3400.0f, 0.0f), 0, n), "-5 dB is not rounded to 0 dB");
        expect (bitIdentical (render (3400.0f, nan), render (3400.0f, 6.0f), 0, n), "NaN strength is not replaced by 6 dB");
        expect (! bitIdentical (render (3400.0f, 12.0f), render (3400.0f, 6.0f), 0, n), "control: 12 dB made no difference from 6 dB");
        expect (! bitIdentical (render (6000.0f, 6.0f), render (3400.0f, 6.0f), 0, n), "control: 6000 Hz made no difference from 3400 Hz");
    }

    // ----- DE5: アタック・リリース・スムージング（サンプルごとの減衰量の軌跡） -----
    void runDynamics()
    {
        beginTest ("DE5: attack (50 % within 6 ms and 90 % within 10 ms of the burst onset, including its 3 ms ramp), release about 50 ms, no jumps in the gain, strength change ramps over 50 ms");

        for (const double fs : { 48000.0, 192000.0 })
        {
            const auto scene = makeScene (fs);
            const auto red = traceReduction (fs, scene.in, 3400.0f, 6.0f);
            const auto at = [&] (int start, double ms) { return (double) red[(size_t) (start + (int) std::lround (ms * 1.0e-3 * fs))]; };

            // 立ち上がり: バーストの開始から、減衰量が最大の50 %・90 %になるまでの時間[ms]。
            double t50 = -1.0, t90 = -1.0;

            for (int i = scene.burstStart; i < scene.burstEnd; ++i)
            {
                const double ms = (double) (i - scene.burstStart) / fs * 1000.0;

                if (t50 < 0.0 && red[(size_t) i] >= 3.0f)
                    t50 = ms;

                if (t90 < 0.0 && red[(size_t) i] >= 5.4f)
                    t90 = ms;
            }

            // 立ち下がり: バーストの終わりから、減衰量が最大の50 %・10 %を下回るまでの時間[ms]。
            double r50 = -1.0, r10 = -1.0;

            for (int i = scene.burstEnd; i < (int) red.size(); ++i)
            {
                const double ms = (double) (i - scene.burstEnd) / fs * 1000.0;

                if (r50 < 0.0 && red[(size_t) i] <= 3.0f)
                    r50 = ms;

                if (r10 < 0.0 && red[(size_t) i] <= 0.6f)
                    r10 = ms;
            }

            // 隣り合うサンプルの減衰量の差（線形のゲインで）。
            double maxGainStep = 0.0;

            for (size_t i = 1; i < red.size(); ++i)
                maxGainStep = std::max (maxGainStep, std::abs (std::pow (10.0, -(double) red[i] / 20.0) - std::pow (10.0, -(double) red[i - 1] / 20.0)));

            logMessage ("DE5 " + juce::String (fs, 0) + " Hz: attack 50 % at " + juce::String (t50, 2) + " ms, 90 % at " + juce::String (t90, 2) + " ms; release to 50 % at " + juce::String (r50, 1)
                        + " ms, to 10 % at " + juce::String (r10, 1) + " ms; max gain step " + juce::String (maxGainStep, 5));

            expect (t50 > 0.0 && t50 <= 6.0, "attack to 50 % took " + juce::String (t50, 2) + " ms");
            expect (t90 > 0.0 && t90 <= 10.0, "attack to 90 % took " + juce::String (t90, 2) + " ms");
            expect (r50 >= 15.0 && r50 <= 60.0, "release to 50 % took " + juce::String (r50, 1) + " ms (about 50 ms)");
            expect (r10 >= 60.0 && r10 <= 220.0, "release to 10 % took " + juce::String (r10, 1) + " ms");
            expect (maxGainStep < 0.02, "the gain jumps by " + juce::String (maxGainStep, 5) + " in one sample");
            expect (at (scene.burstEnd, 400.0) < 1.0e-6, "the reduction did not return to exactly 0 dB 400 ms after the burst");
        }

        // 中心周波数の変更も50 msの補間（対数で直線）: 8 kHzの強い音の途中で 3400 → 8000 Hz。変更の約10 ms後は、まだ最終の減衰の6割に届かない。
        {
            constexpr double fs = 48000.0;
            const int n = (int) fs;
            std::vector<float> in ((size_t) n);

            for (int i = 0; i < n; ++i)
                in[(size_t) i] = (float) (0.1 * std::sin (kTwoPi * 8000.0 * (double) i / fs));

            fadeIn (in, fs);
            vc::DeEsser d;
            d.prepare (fs);
            auto out = in;

            for (int pos = 0; pos < n; pos += 48)
            {
                d.setTarget (true, pos < n / 2 ? 3400.0f : 8000.0f, 6.0f);
                d.process (out.data() + pos, 48);
            }

            const auto reductionAt = [&] (double fromSeconds)
            {
                const int from = (int) std::lround (fromSeconds * fs);
                return -20.0 * std::log10 (toneAmplitude (out, from, 192, fs, 8000.0) / toneAmplitude (in, from, 192, fs, 8000.0)); // 4 ms = 32周期
            };

            const double before = reductionAt (0.45), mid = reductionAt (0.5 + 0.008), after = reductionAt (0.95);
            logMessage ("DE5 centre change 3400 -> 8000 Hz on an 8 kHz tone: reduction before " + juce::String (before, 2) + " dB, 8-12 ms after " + juce::String (mid, 2) + " dB, final " + juce::String (after, 2) + " dB");
            expect (before < 1.0, "before the change the 8 kHz tone is already reduced: " + juce::String (before, 2) + " dB");
            expect (after > 5.5 && after < 6.2, "final reduction after the centre change: " + juce::String (after, 2) + " dB");
            expect (mid < 0.6 * after, "the centre change is not smoothed: " + juce::String (mid, 2) + " dB 8-12 ms after the change");
        }

        // 強さの変更は50 msの補間（クリックなし）: 歯擦音の最中に 0 → 12 dB。
        {
            constexpr double fs = 48000.0;
            const int n = (int) fs;
            auto in = makeBandNoise (fs, n, 3400.0, 0.1f, 5);
            vc::DeEsser d;
            d.prepare (fs);
            d.setTarget (true, 3400.0f, 0.0f);
            std::vector<float> red;
            auto buf = in;

            for (int pos = 0; pos < n; pos += 48)
            {
                d.setTarget (true, 3400.0f, pos < n / 2 ? 0.0f : 12.0f);
                d.process (buf.data() + pos, 48);
                red.push_back (d.getReductionDb());
            }

            const int change = n / 2 / 48; // 変更のブロック
            expect (red[(size_t) change - 1] < 1.0e-6, "reduction before the change");
            expect (red[(size_t) change + 10] > 1.0 && red[(size_t) change + 10] < 6.0, "about 10 ms after the change the strength must be partway: " + juce::String (red[(size_t) change + 10], 2) + " dB");
            expect (std::abs (red.back() - 12.0) < 0.1, "final reduction after the strength change");
        }
    }

    // ----- DE6: ON/OFF（OFFはビット一致、切り替えはクロスフェードでクリックなし）、パラメータ変更のクリックなし -----
    void runOnOff()
    {
        beginTest ("DE6: OFF is bit-identical, ON/OFF toggles and parameter changes in the middle of a sibilant do not click");

        for (const double fs : { 48000.0, 44100.0, 192000.0 })
        {
            // (a) OFFのまま: 入力（±1を超える値を含む）とビット一致、処理中ではない。
            {
                juce::Random rng (3);
                std::vector<float> in ((size_t) fs);

                for (auto& x : in)
                    x = (rng.nextFloat() * 2.0f - 1.0f) * 1.7f;

                vc::DeEsser d;
                d.prepare (fs);
                const auto out = runDeEsser (d, in, false, 3400.0f, 12.0f);
                expect (bitIdentical (out, in, 0, (int) in.size()), "OFF modified the signal");
                expect (! d.isRunning(), "OFF is running");
            }

            // 長い歯擦音（-20 dBFS RMS、1.5 s）の途中で切り替える。除去される成分 d = 出力 - 入力 の隣り合うサンプルの差を、ONのままの定常状態と比べる。
            const int n = (int) (1.5 * fs);
            auto in = makeBandNoise (fs, n, 3400.0, 0.1f, 8);
            applyEdges (in, 0, n, fs);

            const auto stepOf = [&] (const std::vector<float>& out, int from, int to)
            {
                double m = 0.0;

                for (int i = from + 1; i < to; ++i)
                    m = std::max (m, (double) std::abs ((out[(size_t) i] - in[(size_t) i]) - (out[(size_t) i - 1] - in[(size_t) i - 1])));

                return m;
            };

            vc::DeEsser steady;
            steady.prepare (fs);
            const auto steadyOut = runDeEsser (steady, in, true, 3400.0f, 12.0f);
            const double steadyStep = stepOf (steadyOut, (int) (0.4 * fs), (int) (1.2 * fs));
            expect (steadyStep > 1.0e-3, "control: the steady removed component is too small to compare against");

            // (b) OFF -> ON（0.5 s）、ON -> OFF（1.0 s）
            {
                vc::DeEsser d;
                d.prepare (fs);
                auto out = in;

                for (int pos = 0; pos < n; pos += 480)
                {
                    const double t = (double) pos / fs;
                    d.setTarget (t >= 0.5 && t < 1.0, 3400.0f, 12.0f);
                    d.process (out.data() + pos, std::min (480, n - pos));
                }

                const double onStep = stepOf (out, (int) (0.45 * fs), (int) (0.7 * fs));
                const double offStep = stepOf (out, (int) (0.95 * fs), (int) (1.25 * fs));
                logMessage ("DE6 " + juce::String (fs, 0) + " Hz: max step of the removed component: steady " + juce::String (steadyStep, 5) + ", OFF->ON " + juce::String (onStep, 5) + ", ON->OFF " + juce::String (offStep, 5));
                expect (onStep <= steadyStep * 1.1, "OFF -> ON clicks: " + juce::String (onStep, 5) + " vs steady " + juce::String (steadyStep, 5));
                expect (offStep <= steadyStep * 1.1, "ON -> OFF clicks: " + juce::String (offStep, 5) + " vs steady " + juce::String (steadyStep, 5));
                expect (bitIdentical (out, in, 0, (int) (0.499 * fs)), "OFF before the toggle modified the signal");
                expect (bitIdentical (out, in, (int) (1.1 * fs), n), "OFF after the fade-out modified the signal");
                expect (! d.isRunning(), "still running after the fade-out");
            }

            // (c) 中心周波数 3400 -> 8000 Hz（0.6 s）と、強さ 12 -> 3 dB（1.0 s）をONのまま変える。
            {
                vc::DeEsser d;
                d.prepare (fs);
                auto out = in;

                for (int pos = 0; pos < n; pos += 480)
                {
                    const double t = (double) pos / fs;
                    d.setTarget (true, t < 0.6 ? 3400.0f : 8000.0f, t < 1.0 ? 12.0f : 3.0f);
                    d.process (out.data() + pos, std::min (480, n - pos));
                }

                const double hzStep = stepOf (out, (int) (0.55 * fs), (int) (0.8 * fs));
                const double dbStep = stepOf (out, (int) (0.95 * fs), (int) (1.2 * fs));
                logMessage ("DE6 " + juce::String (fs, 0) + " Hz: max step: centre change " + juce::String (hzStep, 5) + ", strength change " + juce::String (dbStep, 5));
                expect (hzStep <= steadyStep * 1.2, "a centre frequency change clicks: " + juce::String (hzStep, 5) + " vs steady " + juce::String (steadyStep, 5));
                expect (dbStep <= steadyStep * 1.2, "a strength change clicks: " + juce::String (dbStep, 5) + " vs steady " + juce::String (steadyStep, 5));
            }

            // (d) ON -> OFF -> ON を速く往復しても（フェードの途中で反転）、出力は有限で、除去される成分の隣り合うサンプルの差は定常を超えない。
            {
                vc::DeEsser d;
                d.prepare (fs);
                auto out = in;

                for (int pos = 0; pos < n; pos += 128)
                {
                    d.setTarget ((pos / 128) % 7 < 4, 3400.0f, 12.0f); // 約12 msごとに反転（フェード20 msの途中で戻る）
                    d.process (out.data() + pos, std::min (128, n - pos));
                }

                expect (vc::test::allFinite (out.data(), n), "toggling quickly produced a non-finite value");
                expect (stepOf (out, (int) (0.1 * fs), n) <= steadyStep * 1.3, "toggling quickly clicks");
            }
        }
    }

    // ----- DE7: 帯域外の周波数応答（最大減衰でも、低域・高域は±0.3 dB以内） -----
    void runOutOfBand()
    {
        beginTest ("DE7: frequency response outside the band stays within +-0.3 dB of OFF even at 12 dB maximum attenuation");

        for (const double fs : { 48000.0, 96000.0 })
        {
            // 3.4 kHzの強い音（歯擦音として最大減衰を保つ）に、小さなプローブ音を重ねる。プローブの振幅の変化が、帯域外の応答。
            const int n = (int) (2.0 * fs);
            const std::array<double, 6> probes { 100.0, 300.0, 600.0, 1000.0, 12000.0, 16000.0 };
            std::vector<float> in ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                double v = 0.3 * std::sin (kTwoPi * 3400.0 * (double) i / fs);

                for (const double f : probes)
                    v += 0.003 * std::sin (kTwoPi * f * (double) i / fs + f);

                in[(size_t) i] = (float) v;
            }

            vc::DeEsser d;
            d.prepare (fs);
            const auto out = runDeEsser (d, in, true, 3400.0f, 12.0f);
            expect (std::abs ((double) d.getReductionDb() - 12.0) < 0.1, "control: the 3.4 kHz tone must hold the maximum attenuation");

            const int from = n / 2, len = (int) fs; // 1 s（プローブは整数周期）
            const double centre0 = toneAmplitude (in, from, len, fs, 3400.0), centre1 = toneAmplitude (out, from, len, fs, 3400.0);
            expect (20.0 * std::log10 (centre1 / centre0) < -11.0, "the centre tone is not reduced by about 12 dB");

            juce::String table = "DE7 " + juce::String (fs, 0) + " Hz (centre 3400, 12 dB): probe response [dB]";

            for (const double f : probes)
            {
                const double change = 20.0 * std::log10 (toneAmplitude (out, from, len, fs, f) / toneAmplitude (in, from, len, fs, f));
                table << "  " << juce::String (f, 0) << " Hz " << juce::String (change, 3);
                expect (std::abs (change) <= 0.3, juce::String (f, 0) + " Hz changed by " + juce::String (change, 3) + " dB");
            }

            logMessage (table);
        }
    }

    // ----- DE8: ブロック長に依存しない -----
    void runBlockIndependence()
    {
        beginTest ("DE8: the output does not depend on the block length");

        constexpr double fs = 48000.0;
        const auto scene = makeScene (fs);
        const int n = (int) scene.in.size();
        const auto reference = runFresh (fs, scene.in, true, 3400.0f, 8.0f, 480);

        for (const int block : { 1, 7, 32, 64, 1000, 4096 })
            expect (bitIdentical (runFresh (fs, scene.in, true, 3400.0f, 8.0f, block), reference, 0, n), "block " + juce::String (block) + " differs from block 480");
    }

    // ----- DE9: NaN/Inf・reset・prepare -----
    void runNonFiniteAndReset()
    {
        beginTest ("DE9: reset() and prepare() clear the state completely; NaN/Inf input is recovered by reset()");

        constexpr double fs = 48000.0;
        const auto scene = makeScene (fs);
        const int n = (int) scene.in.size();
        const auto reference = runFresh (fs, scene.in, true, 3400.0f, 6.0f);

        // 途中まで処理して（歯擦音の最中を含む）reset → 同じ入力を最初から処理すると、新しいインスタンスとビット一致。
        {
            vc::DeEsser d;
            d.prepare (fs);
            auto warm = scene.in;
            runDeEsser (d, warm, true, 6000.0f, 12.0f);
            d.reset();
            expect (! d.isRunning(), "running after reset");
            expect (d.getReductionDb() < 1.0e-9, "reduction after reset");
            expect (bitIdentical (runDeEsser (d, scene.in, true, 3400.0f, 6.0f), reference, 0, n), "state remains after reset()");
        }

        // prepare()でも同様。
        {
            vc::DeEsser d;
            d.prepare (fs);
            runDeEsser (d, scene.in, true, 6000.0f, 12.0f);
            d.prepare (fs);
            expect (bitIdentical (runDeEsser (d, scene.in, true, 3400.0f, 6.0f), reference, 0, n), "state remains after prepare()");
        }

        // NaN・Infが入ったブロックは、処理後の出力に非有限値が残る（EngineがhandleNonFiniteで無音にしてresetする）。reset()後は何も残らない。
        for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() })
        {
            vc::DeEsser d;
            d.prepare (fs);
            auto in = scene.in;
            in[(size_t) (0.6 * fs)] = bad;
            auto out = in;

            for (int pos = 0; pos < n; pos += 480)
            {
                d.setTarget (true, 3400.0f, 6.0f);
                d.process (out.data() + pos, std::min (480, n - pos));

                if (! vc::test::allFinite (out.data() + pos, std::min (480, n - pos)))
                {
                    d.reset(); // Engineと同じ回復
                    break;
                }
            }

            expect (bitIdentical (runDeEsser (d, scene.in, true, 3400.0f, 6.0f), reference, 0, n), "reset() did not recover from a non-finite input");
        }
    }

    // ----- DE10: Engine: 位置・独立性・バイパス・回復・確保なし -----
    static constexpr int kEngineBlock = 480;

    static void prepareEngine (vc::Engine& e, double fs)
    {
        e.prepare ({ fs, kEngineBlock });
        e.params().nrEnabled.store (false);
        e.params().eqEnabled.store (false);
    }

    static std::vector<float> runEngineOn (vc::Engine& e, const std::vector<float>& in, int block = kEngineBlock)
    {
        auto out = in;

        for (int pos = 0; pos < (int) out.size(); pos += block)
            e.process (out.data() + pos, std::min (block, (int) out.size() - pos));

        return out;
    }

    void runEngine()
    {
        beginTest ("DE10: Engine - runs without NR/EQ, independent of the global bypass, placed right after the EQ and before layer 1, recovers from NaN, no allocation");

        constexpr double fs = 48000.0;
        const auto scene = makeScene (fs);
        const int n = (int) scene.in.size();
        const int from = scene.burstStart + (int) (0.04 * fs), to = scene.burstEnd - (int) (0.002 * fs);

        const auto burstChange = [&] (const std::vector<float>& out, const std::vector<float>& reference) { return rmsDbOf (out, from, to) - rmsDbOf (reference, from, to); };

        // (a) NR OFF・EQ OFF（既定）でも、全体ON（層1・層2）でも、バイパスでも動く。OFFにすると何もしない。
        for (const bool chainOn : { true, false })
        {
            vc::Engine off;
            prepareEngine (off, fs);
            off.params().enabled.store (chainOn);
            const auto reference = runEngineOn (off, scene.in);

            vc::Engine on;
            prepareEngine (on, fs);
            on.params().enabled.store (chainOn);
            on.params().deEssEnabled.store (true);
            const auto out = runEngineOn (on, scene.in);

            const double change = burstChange (out, reference);
            logMessage (juce::String ("DE10a: chain ") + (chainOn ? "ON" : "bypassed") + ": burst RMS change " + juce::String (change, 2) + " dB");
            expect (change <= -4.5, juce::String ("chain ") + (chainOn ? "ON" : "bypassed") + ": the burst is not reduced (change " + juce::String (change, 2) + " dB)");
            expect (bitIdentical (out, reference, 0, scene.burstStart - (int) (0.01 * fs)), "the vowel before the burst differs from the OFF output");
        }

        // (b) 既定はOFF（AtomicParamsの初期値）。何も設定しなければ、ディエッサーなしの出力とビット一致。
        {
            vc::Engine a, b;
            prepareEngine (a, fs);
            prepareEngine (b, fs);
            b.params().deEssEnabled.store (false);
            b.params().deEssHz.store (6000.0f);   // OFFでも値は保持される。音には影響しない
            b.params().deEssMaxDb.store (12.0f);
            expect (bitIdentical (runEngineOn (a, scene.in), runEngineOn (b, scene.in), 0, n), "OFF with other values differs from the default");
        }

        // (c) NR ON・EQ ONでも動く（その場合の出力と、ディエッサーOFFの出力を比べる）。
        {
            vc::Engine off, on;

            for (auto* e : { &off, &on })
            {
                prepareEngine (*e, fs);
                e->params().nrEnabled.store (true);
                e->params().nrBackground.store (0.0f); // 原音を通す（RNNoiseが人工の雑音バーストを消さないように。遅延と経路だけを確かめる）
                e->params().nrImpact.store (0.0f);
                e->params().eqEnabled.store (true);
                e->params().eqOutputGainDb.store (0.0f);
            }

            on.params().deEssEnabled.store (true);
            // NR ONは遅延（30 ms）がある。どちらも同じ遅延なので同じ位置で比べる。バースト区間より後ろまで流す。
            auto longer = scene.in;
            longer.resize (longer.size() + 4800, 0.0f);
            const auto reference = runEngineOn (off, longer);
            const auto out = runEngineOn (on, longer);
            const int latency = off.getNoiseReducerLatencySamples();
            const double change = rmsDbOf (out, from + latency, to + latency) - rmsDbOf (reference, from + latency, to + latency);
            logMessage ("DE10c: NR ON + EQ ON: burst RMS change " + juce::String (change, 2) + " dB (NR delay " + juce::String (latency) + " samples)");
            expect (change < -1.0, "with NR and EQ ON the burst is not reduced further: " + juce::String (change, 2) + " dB");
        }

        // (d) 位置（EQの直後）: EQが3.4 kHzを-18 dB下げる設定。バースト（-38 dBFS）はEQの後で約-51 dBFSになり、基準の下駄に埋もれて何もしない
        //     → ディエッサーONでもEQだけの出力とビット一致。ディエッサーがEQの前にあれば（-38 dBFSの歯擦音が見える）減衰が入って一致しない。
        {
            const auto quiet = makeScene (fs, 3400.0, -38.0f, 0.05f);

            const auto configure = [] (vc::Engine& e, bool deEss, bool eq)
            {
                e.params().eqEnabled.store (eq);
                e.params().eqOutputGainDb.store (0.0f);

                for (int i = 0; i < vc::kEqBands; ++i)
                    e.params().eqBands[(size_t) i].store ({ i == 3, vc::EqType::Peak, 3400.0f, -18.0f, 1.0f });

                e.params().deEssEnabled.store (deEss);
            };

            vc::Engine eqOnly, eqAndDeEss, deEssOnly;
            prepareEngine (eqOnly, fs);
            prepareEngine (eqAndDeEss, fs);
            prepareEngine (deEssOnly, fs);
            configure (eqOnly, false, true);
            configure (eqAndDeEss, true, true);
            configure (deEssOnly, true, false);

            const auto q = quiet.in;
            const auto a = runEngineOn (eqOnly, q);
            const auto b = runEngineOn (eqAndDeEss, q);
            const auto c = runEngineOn (deEssOnly, q);
            vc::Engine plain;
            prepareEngine (plain, fs);
            const auto p = runEngineOn (plain, q);

            expect (bitIdentical (a, b, 0, (int) q.size()), "the de-esser reacted to a burst that the EQ had already reduced: it is placed before the EQ");
            expect (rmsDbOf (c, from, to) - rmsDbOf (p, from, to) <= -4.5, "control: without the EQ the -38 dBFS burst must be reduced");
        }

        // (e) 位置（層1の前）: 層1のゲイン-20 dBの前にあれば、歯擦音は元のレベルで検出されて最大減衰近くまで下がる（ゲインの後なら-20 dBFS分小さく見えて効かない）。
        {
            const auto quiet = makeScene (fs, 3400.0, -35.0f, 0.05f);
            vc::Engine off, on;
            prepareEngine (off, fs);
            prepareEngine (on, fs);
            off.params().gainDb.store (-20.0f);
            on.params().gainDb.store (-20.0f);
            on.params().deEssEnabled.store (true);
            const auto change = burstChange (runEngineOn (on, quiet.in), runEngineOn (off, quiet.in));
            logMessage ("DE10e: layer 1 gain -20 dB, burst -35 dBFS: burst RMS change " + juce::String (change, 2) + " dB");
            expect (change <= -4.5, "the de-esser sees the signal after the layer 1 gain: change " + juce::String (change, 2) + " dB");
        }

        // (f) NaN回復: ONのまま途中のブロックにNaNを入れる。そのブロックは無音・フラグが立ち、その後の出力は新しいEngineと一致する（状態が完全にリセットされる）。
        {
            vc::Engine e;
            prepareEngine (e, fs);
            e.params().deEssEnabled.store (true);

            auto in = scene.in;
            const int badBlock = (int) (0.6 * fs) / kEngineBlock;
            in[(size_t) (badBlock * kEngineBlock + 5)] = std::numeric_limits<float>::quiet_NaN();
            const auto out = runEngineOn (e, in);

            expect ((e.getErrorFlags() & 1u) != 0, "the error flag was not raised");
            expect (juce::exactlyEqual (vc::test::peakAbs (out.data() + badBlock * kEngineBlock, kEngineBlock), 0.0), "the block with NaN input is not silent");
            expect (vc::test::allFinite (out.data(), n), "non-finite output");

            const int resume = (badBlock + 1) * kEngineBlock;
            std::vector<float> rest (in.begin() + resume, in.end());
            vc::Engine fresh;
            prepareEngine (fresh, fs);
            fresh.params().deEssEnabled.store (true);
            const auto expected = runEngineOn (fresh, rest);
            expect (std::memcmp (out.data() + resume, expected.data(), sizeof (float) * expected.size()) == 0,
                    "after the NaN block the output differs from a fresh engine (the de-esser state was not reset)");
        }

        // (g) 確保なし: 処理中にパラメータを変えながら。
        {
            vc::Engine e;
            prepareEngine (e, fs);
            e.params().deEssEnabled.store (true);
            auto buf = scene.in;
            std::size_t allocations = 0;

            {
                const vc::test::ScopedAllocationGuard guard;

                for (int pos = 0; pos + kEngineBlock <= n; pos += kEngineBlock)
                {
                    e.params().deEssHz.store (2000.0f + (float) ((pos / kEngineBlock) % 8) * 1000.0f);
                    e.params().deEssMaxDb.store ((float) ((pos / kEngineBlock) % 13));
                    e.params().deEssEnabled.store ((pos / kEngineBlock) % 11 != 5);
                    e.process (buf.data() + pos, kEngineBlock);
                }

                allocations = guard.count();
            }

            expect (allocations == 0, "the audio thread allocated " + juce::String ((int) allocations) + " times");
        }
    }

    // ----- DE10b: 試聴ツール（--process-wav の --deess / --deess-hz / --deess-db。T-019） -----
    void runTool()
    {
        beginTest ("DE10b: RecordingTool - --deess on|off, --deess-hz, --deess-db, the settings file, and rejection of invalid values");

        constexpr double fs = 48000.0;
        const auto scene = makeScene (fs);
        const int from = scene.burstStart + (int) (0.04 * fs), to = scene.burstEnd - (int) (0.002 * fs);

        // processAudio（ツールの本体）: 既定はOFFで、Engineに通した出力と同じ。ONにするとバーストが下がる。
        {
            vc::rectool::ProcessSettings off;
            off.nrEnabled = false;
            auto on = off;
            on.deEssEnabled = true;
            on.deEssHz = 3400.0f;
            on.deEssMaxDb = 6.0f;
            expect (! off.deEssEnabled && juce::approximatelyEqual (off.deEssHz, 3400.0f) && juce::approximatelyEqual (off.deEssMaxDb, 6.0f), "ProcessSettings defaults");

            const auto a = vc::rectool::processAudio (scene.in, fs, off);
            const auto b = vc::rectool::processAudio (scene.in, fs, on);
            expect (rmsDbOf (b.output, from, to) - rmsDbOf (a.output, from, to) <= -4.0, "the tool's de-esser does not reduce the burst");
            expect (bitIdentical (a.output, scene.in, 0, (int) scene.in.size()), "the tool's OFF output differs from the input (NR off, EQ off)");
        }

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("vc_deess", "");
        expect (dir.createDirectory().wasOk(), "temp dir");
        const auto inFile = dir.getChildFile ("in.wav");
        const auto outFile = dir.getChildFile ("out.wav");
        juce::String error;
        expect (vc::rectool::writeWav16 (inFile, scene.in, fs, error), error);

        const auto run = [&] (std::initializer_list<const char*> extra)
        {
            juce::StringArray a { inFile.getFullPathName(), outFile.getFullPathName(), "--nr", "off" };

            for (const auto* e : extra)
                a.add (e);

            return vc::rectool::runProcessWav (a);
        };

        const auto burstRms = [&]
        {
            vc::rectool::Audio back;
            expect (vc::rectool::readWav (outFile, back, error), error);
            return rmsDbOf (back.samples, from, to);
        };

        expectEquals (run ({ "--deess", "off" }), 0);
        const double offLevel = burstRms();
        expectEquals (run ({}), 0);
        expect (std::abs (burstRms() - offLevel) < 0.01, "the default is not OFF");
        expectEquals (run ({ "--deess", "on" }), 0);
        const double onLevel = burstRms();
        expect (onLevel - offLevel <= -4.0, "--deess on: burst " + juce::String (onLevel - offLevel, 2) + " dB");
        expectEquals (run ({ "--deess", "ON", "--deess-db", "12" }), 0);
        expect (burstRms() - onLevel <= -2.0, "--deess-db 12 is not stronger than the default 6");
        expectEquals (run ({ "--deess", "on", "--deess-db", "0" }), 0);
        expect (std::abs (burstRms() - offLevel) < 0.01, "--deess-db 0 must not reduce");
        expectEquals (run ({ "--deess", "on", "--deess-hz", "9000" }), 0);
        expect (std::abs (burstRms() - offLevel) < 1.0, "--deess-hz 9000 must not reduce the 3.4 kHz burst");
        expectEquals (run ({ "--deess-hz", "3400", "--deess-db", "6" }), 0); // --deessなしの値だけの指定はOFFのまま
        expect (std::abs (burstRms() - offLevel) < 0.01, "--deess-hz / --deess-db without --deess turned it on");

        // 設定ファイル（--settings）: deEssEnabled=1 でON。--deess offで上書きできる。
        {
            const auto settingsFile = dir.getChildFile ("VoiceChange.settings");
            expect (settingsFile.replaceWithText ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<PROPERTIES><VALUE name=\"deEssEnabled\" val=\"1\"/><VALUE name=\"deEssMaxDb\" val=\"9\"/></PROPERTIES>\n"), "settings file");
            expectEquals (run ({ "--settings", settingsFile.getFullPathName().toRawUTF8() }), 0);
            const double fromFile = burstRms();
            expect (fromFile - onLevel <= -1.0, "the settings file did not turn the de-esser on with 9 dB");
            expectEquals (run ({ "--settings", settingsFile.getFullPathName().toRawUTF8(), "--deess", "off" }), 0);
            expect (std::abs (burstRms() - offLevel) < 0.01, "--deess off did not override the settings file");
        }

        const std::vector<std::vector<const char*>> bad { { "--deess", "maybe" }, { "--deess", "" }, { "--deess-hz", "1999" }, { "--deess-hz", "9001" }, { "--deess-hz", "abc" },
                                                           { "--deess-db", "-1" }, { "--deess-db", "12.5" }, { "--deess-db", "x" }, { "--deess-db", "" } };

        for (const auto& b : bad)
        {
            juce::StringArray a { inFile.getFullPathName(), outFile.getFullPathName(), "--nr", "off" };

            for (const auto* e : b)
                a.add (e);

            expectEquals (vc::rectool::runProcessWav (a), 2, "should be rejected: " + a.joinIntoString (" "));
        }

        dir.deleteRecursively();
    }

    // ----- DE11: CPU（E10と同じ条件: 48 kHz・480ブロック・10秒の母音。ノーマル・層1ピッチ0）。増分が小さいこと -----
    void runCpu()
    {
        beginTest ("DE11: CPU increase of the de-esser (processing time / audio time, 48 kHz and 192 kHz, 10 s)");

        for (const double fs : { 48000.0, 192000.0 })
        {
            const int total = (int) fs * 10;
            const int block = (int) std::lround (fs * 0.01);
            auto voice = vc::test::makeSyntheticVowel (150.0, fs, total, { 730.0, 1090.0, 2440.0 }, { 80.0, 90.0, 120.0 }, 0.1f);
            vc::test::addNoiseFloor (voice, -60.0f, 444);

            const auto measure = [&] (bool deEss)
            {
                double best = 1.0e9;

                for (int run = 0; run < 5; ++run)
                {
                    vc::Engine e;
                    e.prepare ({ fs, block });
                    e.params().nrEnabled.store (false);
                    e.params().deEssEnabled.store (deEss);
                    auto buf = voice;

                    const auto t0 = juce::Time::getHighResolutionTicks();

                    for (int pos = 0; pos + block <= total; pos += block)
                        e.process (buf.data() + pos, block);

                    best = std::min (best, juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0));
                }

                return 100.0 * best / 10.0;
            };

            const double off = measure (false), on = measure (true);
            logMessage ("DE11 " + juce::String (fs, 0) + " Hz: normal preset " + juce::String (off, 3) + "% -> with de-esser " + juce::String (on, 3) + "% (+" + juce::String (on - off, 3) + " points)");
            expect (on - off <= 1.0 * fs / 48000.0, "the de-esser adds " + juce::String (on - off, 3) + " points of CPU");
        }
    }
};

static DeEsserTests deEsserTests;
