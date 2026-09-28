#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"
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
};

static EffectsTests effectsTests;

} // namespace
