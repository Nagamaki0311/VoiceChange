#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/Effects.h"
#include "core/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <vector>

// ===== SECTION: EffectsTests =====
// D1〜D4（ピッチ検出）、F1〜F3（フォルマント）、K1（ケロケロ）、X1〜X3（エコー・ロボット・トークボックス）。
// カテゴリEffects、quick。docs/plan.md 3章T-005参照。

namespace
{

constexpr int kNumFramesWarmupMs = 50;

struct DetectorFrame
{
    float hz;
    float voicing;
};

// ホップ約5msずつdetectorへ供給し、各ブロック後の検出結果を集める。
std::vector<DetectorFrame> runDetector (vc::PitchDetector& det, const std::vector<float>& signal, double fs)
{
    const int block = (int) std::lround (0.005 * fs);
    std::vector<DetectorFrame> frames;

    for (int pos = 0; pos + block <= (int) signal.size(); pos += block)
    {
        det.process (signal.data() + pos, block);
        frames.push_back ({ det.getFrequencyHz(), det.getVoicing() });
    }

    return frames;
}

// 帯域制限鋸波（倍音を5kHz以下・ナイキスト未満に制限した加算合成）。
std::vector<float> makeBandlimitedSaw (double f0, double fs, int n, float amp)
{
    std::vector<float> out ((size_t) n, 0.0f);
    const double limit = std::min (5000.0, fs * 0.45);
    const int maxK = (int) std::floor (limit / f0);
    constexpr double twoPi = 6.283185307179586476925286766559;

    for (int k = 1; k <= maxK; ++k)
    {
        const double inc = twoPi * f0 * (double) k / fs;
        double ph = 0.0;
        for (int i = 0; i < n; ++i, ph += inc)
            out[(size_t) i] += (float) (std::sin (ph) / (double) k);
    }

    const double peak = vc::test::peakAbs (out.data(), n);
    for (auto& s : out)
        s *= (float) (amp / peak);

    return out;
}

class EffectsTests final : public juce::UnitTest
{
public:
    EffectsTests() : juce::UnitTest ("Effects", "Effects") {}

    void runTest() override
    {
        runD1D3();
        runD2();
        runD4();
        runX1();
        runX2();
        runX3Gain();
    }

private:
    struct Accuracy
    {
        int voicedCount = 0;
        int totalCount = 0;
        double fractionWithin1Pct = 0.0;
        double medianErrPct = 0.0;
    };

    static Accuracy measureAccuracy (const std::vector<float>& signal, double fs, double trueHz)
    {
        vc::PitchDetector det;
        det.prepare (fs, 512);
        const auto frames = runDetector (det, signal, fs);
        const int skip = (int) std::ceil ((double) kNumFramesWarmupMs / 5.0);

        Accuracy a;
        std::vector<double> errs;
        int within = 0;

        for (size_t i = (size_t) skip; i < frames.size(); ++i)
        {
            ++a.totalCount;

            if (frames[i].voicing < 0.5f)
                continue;

            const double err = std::abs ((double) frames[i].hz - trueHz) / trueHz * 100.0;
            errs.push_back (err);
            ++a.voicedCount;
            if (err <= 1.0)
                ++within;
        }

        if (! errs.empty())
        {
            std::sort (errs.begin(), errs.end());
            a.medianErrPct = errs[errs.size() / 2];
            a.fractionWithin1Pct = (double) within / (double) errs.size();
        }

        return a;
    }

    void checkAccuracy (const juce::String& label, const std::vector<float>& signal, double fs, double trueHz)
    {
        const auto a = measureAccuracy (signal, fs, trueHz);
        logMessage (label + ": voiced " + juce::String (a.voicedCount) + "/" + juce::String (a.totalCount)
                    + ", within1% " + juce::String (a.fractionWithin1Pct * 100.0, 1) + "%, median err "
                    + juce::String (a.medianErrPct, 3) + "%");
        expect (a.voicedCount >= (int) (0.95 * a.totalCount), label + ": too few voiced frames");
        expect (a.fractionWithin1Pct >= 0.95, label + ": within1% fraction too low");
        expect (a.medianErrPct <= 0.3, label + ": median error too large");
    }

    // ----- D1（48kHz）とD3（44.1k/96k）: 検出精度 -----
    void runD1D3()
    {
        for (const double fs : { 48000.0, 44100.0, 96000.0 })
        {
            beginTest (juce::String (fs < 48001.0 && fs > 47999.0 ? "D1" : "D3") + ": ピッチ検出の精度 fs=" + juce::String (fs));
            const int n = (int) fs; // 1秒
            const juce::String tag = juce::String (fs, 0) + " ";

            for (const double f : { 80.0, 110.0, 220.0, 440.0, 880.0 })
                checkAccuracy (tag + "sine " + juce::String (f), vc::test::makeSine (f, fs, n, 0.3f), fs, f);

            for (const double f : { 100.0, 200.0, 400.0 })
                checkAccuracy (tag + "saw " + juce::String (f), makeBandlimitedSaw (f, fs, n, 0.3f), fs, f);

            for (const double f : { 100.0, 150.0, 250.0 })
                checkAccuracy (tag + "vowel " + juce::String (f), vc::test::makeSyntheticVowel (f, fs, n), fs, f);
        }
    }

    // ----- D2: 無声の判定 -----
    void runD2()
    {
        beginTest ("D2: 無音は全フレーム無声、白色雑音-20dBFSは90%以上で有声度<0.5");

        constexpr double fs = 48000.0;
        const int n = (int) fs;

        {
            vc::PitchDetector det;
            det.prepare (fs, 512);
            const auto frames = runDetector (det, std::vector<float> ((size_t) n, 0.0f), fs);
            int voiced = 0;
            for (const auto& f : frames)
                voiced += f.voicing > 0.0f ? 1 : 0;
            expect (voiced == 0, "silence produced voiced frames: " + juce::String (voiced));
        }

        {
            vc::PitchDetector det;
            det.prepare (fs, 512);
            std::vector<float> noise ((size_t) n);
            juce::Random rng (1234);
            // 一様乱数のRMS = amp/sqrt(3)。-20dBFS RMS = 0.1
            for (auto& s : noise)
                s = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f * std::sqrt (3.0f);

            const auto frames = runDetector (det, noise, fs);
            int low = 0;
            for (const auto& f : frames)
                low += f.voicing < 0.5f ? 1 : 0;
            const double frac = (double) low / (double) frames.size();
            logMessage ("D2 noise: voicing<0.5 fraction " + juce::String (frac * 100.0, 1) + "%");
            expect (frac >= 0.9, "noise voicing<0.5 fraction " + juce::String (frac));
        }
    }

    // ----- D4: アロケーション -----
    void runD4()
    {
        beginTest ("D4: PitchDetector::processのアロケーション0回");

        constexpr double fs = 48000.0;
        vc::PitchDetector det;
        det.prepare (fs, 512);
        const auto signal = vc::test::makeSyntheticVowel (150.0, fs, (int) fs);
        std::size_t count = 0;

        {
            vc::test::ScopedAllocationGuard guard;
            for (int pos = 0; pos + 480 <= (int) signal.size(); pos += 480)
                det.process (signal.data() + pos, 480);
            det.reset();
            det.process (signal.data(), 480);
            count = guard.count();
        }

        expect (count == 0, "allocations: " + juce::String ((int) count));
    }
    // ----- X1: エコー -----
    // テンプレートとの相互相関で、[expectedLag ± halfWindow]内の最大相関の位置と振幅（テンプレート比）を返す。
    struct EchoHit
    {
        int lag;
        double amplitude;
    };

    static EchoHit findEcho (const std::vector<float>& out, const std::vector<float>& tmpl, int expectedStart, int halfWindow)
    {
        const int len = (int) tmpl.size();
        double tmplEnergy = 0.0;
        for (const float t : tmpl)
            tmplEnergy += (double) t * t;

        EchoHit best { expectedStart, 0.0 };
        double bestCorr = -1.0e300;

        for (int start = expectedStart - halfWindow; start <= expectedStart + halfWindow; ++start)
        {
            if (start < 0 || start + len > (int) out.size())
                continue;

            double c = 0.0;
            for (int i = 0; i < len; ++i)
                c += (double) out[(size_t) (start + i)] * tmpl[(size_t) i];

            if (c > bestCorr)
            {
                bestCorr = c;
                best = { start, c / tmplEnergy };
            }
        }

        return best;
    }

    void runX1()
    {
        beginTest ("X1: エコー 第1エコー300ms・0.6、第2エコー600ms・第1比0.45、再選択で残響0");

        constexpr double fs = 48000.0;
        const int burstStart = (int) (0.1 * fs);
        const int burstLen = (int) (0.025 * fs); // 200Hz×5周期、Hann窓
        const int total = (int) (1.5 * fs);

        std::vector<float> burst ((size_t) burstLen);
        for (int i = 0; i < burstLen; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) (burstLen - 1));
            burst[(size_t) i] = (float) (0.5 * w * std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * (double) i / fs));
        }

        std::vector<float> in ((size_t) total, 0.0f);
        std::copy (burst.begin(), burst.end(), in.begin() + burstStart);

        vc::Echo echo;
        echo.prepare (fs, 480);
        std::vector<float> out ((size_t) total);
        for (int pos = 0; pos < total; pos += 480)
            echo.process (in.data() + pos, out.data() + pos, std::min (480, total - pos));

        const int halfWin = (int) (0.01 * fs);
        const auto e1 = findEcho (out, burst, burstStart + (int) (0.3 * fs), halfWin);
        const auto e2 = findEcho (out, burst, burstStart + (int) (0.6 * fs), halfWin);
        const double delayMs = (double) (e1.lag - burstStart) / fs * 1000.0;
        const double delay2Ms = (double) (e2.lag - burstStart) / fs * 1000.0;
        logMessage ("X1: echo1 " + juce::String (delayMs, 2) + "ms ratio " + juce::String (e1.amplitude, 4)
                    + ", echo2 " + juce::String (delay2Ms, 2) + "ms ratio-to-echo1 " + juce::String (e2.amplitude / e1.amplitude, 4));

        expectWithinAbsoluteError (delayMs, 300.0, 1.0);
        expectWithinAbsoluteError (e1.amplitude, 0.6, 0.05);
        expectWithinAbsoluteError (delay2Ms, 600.0, 1.0);
        expectWithinAbsoluteError (e2.amplitude / e1.amplitude, 0.45, 0.05);

        // 再選択（reset）で以前の残響が0になる。
        echo.process (in.data() + burstStart, out.data(), burstLen);
        echo.reset();
        std::vector<float> zeros ((size_t) fs, 0.0f), tail ((size_t) fs, 1.0f);
        echo.process (zeros.data(), tail.data(), (int) zeros.size());
        expect (vc::test::peakAbs (tail.data(), (int) tail.size()) == 0.0, "residual echo after reset");

        // reset直後の入力は20msかけてフェードインしてから書き込まれ、300ms後に段差が出ない。
        echo.reset();
        std::vector<float> dc ((size_t) (0.6 * fs), 0.5f), dcOut (dc.size());
        echo.process (dc.data(), dcOut.data(), (int) dc.size());
        const int w0 = (int) (0.29 * fs), w1 = (int) (0.33 * fs);
        const double step = vc::test::maxAdjacentDiff (dcOut.data() + w0, w1 - w0);
        expect (step < 0.002, "step at 300ms after reset: " + juce::String (step, 5));
    }

    // ----- X2: ロボット -----
    void runX2()
    {
        beginTest ("X2: ロボット 1kHz → 960/1040Hzにピーク、1000Hzは30dB以上低下");

        constexpr double fs = 48000.0;
        const int n = (int) fs;
        const auto in = vc::test::makeSine (1000.0, fs, n, 0.5f);
        std::vector<float> out ((size_t) n);

        vc::RingModulator rm;
        rm.prepare (fs, 480);
        for (int pos = 0; pos < n; pos += 480)
            rm.process (in.data() + pos, out.data() + pos, std::min (480, n - pos));

        const auto specIn = vc::test::averagedMagnitudeSpectrum (in.data(), n);
        const auto specOut = vc::test::averagedMagnitudeSpectrum (out.data(), n);
        const auto inPeak = vc::test::findPeakNear (specIn, fs, 1 << 14, 1000.0, 0.01);
        const auto lo = vc::test::findPeakNear (specOut, fs, 1 << 14, 960.0, 0.01);
        const auto hi = vc::test::findPeakNear (specOut, fs, 1 << 14, 1040.0, 0.01);
        const auto mid = vc::test::findPeakNear (specOut, fs, 1 << 14, 1000.0, 0.003);

        const auto db = [] (double a, double b) { return 20.0 * std::log10 (a / b); };
        logMessage ("X2: lo " + juce::String (lo.freqHz, 2) + "Hz " + juce::String (db (lo.magLinear, inPeak.magLinear), 2)
                    + "dB, hi " + juce::String (hi.freqHz, 2) + "Hz " + juce::String (db (hi.magLinear, inPeak.magLinear), 2)
                    + "dB, 1000Hz " + juce::String (db (mid.magLinear, inPeak.magLinear), 2) + "dB");

        expectWithinAbsoluteError (lo.freqHz, 960.0, 2.0);
        expectWithinAbsoluteError (hi.freqHz, 1040.0, 2.0);
        expect (db (mid.magLinear, inPeak.magLinear) <= -30.0, "1000Hz component not suppressed");
    }

    // ----- X3（ゲイン決定）: Talkbox単体で母音3種×f0 3種の出力/入力RMS比 -----
    void runX3Gain()
    {
        beginTest ("X3a: トークボックス単体の出力/入力RMS比（補正ゲイン決定用）");

        constexpr double fs = 48000.0;
        const int n = (int) fs * 2;
        const std::array<std::array<double, 3>, 3> vowels { { { 730.0, 1090.0, 2440.0 }, { 270.0, 2290.0, 3010.0 }, { 300.0, 870.0, 2240.0 } } };
        std::vector<double> ratiosDb;

        for (const auto& fm : vowels)
        {
            for (const double f0 : { 100.0, 150.0, 250.0 })
            {
                const float amp = 0.1f * 1.41421356f * 1.5f; // ピーク振幅（RMSは概ね-20dBFS付近になる）
                auto in = vc::test::makeSyntheticVowel (f0, fs, n, fm, { 80.0, 90.0, 120.0 }, amp);
                std::vector<float> out (in.size());
                vc::Talkbox tb;
                tb.prepare (fs, 480);
                for (int pos = 0; pos + 480 <= n; pos += 480)
                    tb.process (in.data() + pos, out.data() + pos, 480, (float) f0, 1.0f);

                const int skip = (int) fs / 2;
                const double r = 20.0 * std::log10 (vc::test::rms (out.data() + skip, n - skip) / vc::test::rms (in.data() + skip, n - skip));
                ratiosDb.push_back (r);
                expect (std::abs (r) <= 3.0, "talkbox level ratio out of +-3dB: " + juce::String (r, 2));
                logMessage ("X3a: f0=" + juce::String (f0) + " F1=" + juce::String (fm[0]) + " ratio " + juce::String (r, 2) + "dB");
            }
        }

        std::sort (ratiosDb.begin(), ratiosDb.end());
        logMessage ("X3a: median ratio " + juce::String (ratiosDb[4], 2) + "dB");
    }
};

static EffectsTests effectsTests;

} // namespace
