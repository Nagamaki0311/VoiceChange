#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/Effects.h"
#include "core/Engine.h"
#include "core/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// ===== SECTION: EffectsTests =====
// D1〜D4（ピッチ検出）、F1〜F3（フォルマント）、K1（ケロケロ）、X1〜X3（エコー・ロボット・トークボックス）。
// カテゴリEffects、quick。docs/plan.md 3章T-005参照。

namespace
{

constexpr int kNumFramesWarmupMs = 50;

// トークボックスのフレーズ（G2 G2 Bb2 C3 D3 C3 Bb2 F2）の周波数。MIDIから求める式ではなく、音名のHz（A4 = 440Hz、平均律）を数字で固定する（定数の取り違えを検出するため）。
constexpr std::array<double, 8> kPhraseHz { 98.0, 98.0, 116.541, 130.813, 146.832, 130.813, 116.541, 87.307 };

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
        runF();
        runK1();
        runX3();
        runX3Phrase();
        runX3Engine();
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

            for (const double f : { 75.0, 100.0, 150.0, 250.0 })
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

    // ----- X3（ゲイン決定）: Talkbox単体で母音3種×f0 3種の出力/入力RMS比（音域ごと） -----
    // 出力の声量を入力に近づけるのがユーザーの目標（D-026・D-028）。kOutputGainは実録音（raw_A）のK特性・RMSの差が±0.1〜0.7dBになる値で、
    // 合成母音の中央値は、それより少し高く出る（低 +0.8dB・高 +0.4dB）。最大の母音（/a/・f0=100Hz。高）が+2.7dBで、基準の±3dBに近い。
    void runX3Gain()
    {
        beginTest ("X3a: トークボックス単体の出力/入力RMS比（音域ごと。補正ゲインの根拠）");

        constexpr double fs = 48000.0;
        const int n = (int) fs * 2;
        const std::array<std::array<double, 3>, 3> vowels { { { 730.0, 1090.0, 2440.0 }, { 270.0, 2290.0, 3010.0 }, { 300.0, 870.0, 2240.0 } } };

        for (const auto range : { vc::TalkboxRange::Low, vc::TalkboxRange::High })
        {
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
                        tb.process (in.data() + pos, out.data() + pos, 480, 1.0f, range, 1.0f);

                    const int skip = (int) fs / 2;
                    const double r = 20.0 * std::log10 (vc::test::rms (out.data() + skip, n - skip) / vc::test::rms (in.data() + skip, n - skip));
                    ratiosDb.push_back (r);
                    expect (std::abs (r) <= 3.0, "talkbox level ratio out of +-3dB: " + juce::String (r, 2));
                    logMessage ("X3a: range=" + juce::String (vc::talkboxRangeId (range)) + " f0=" + juce::String (f0) + " F1=" + juce::String (fm[0]) + " ratio " + juce::String (r, 2) + "dB");
                }
            }

            std::sort (ratiosDb.begin(), ratiosDb.end());
            logMessage ("X3a: range=" + juce::String (vc::talkboxRangeId (range)) + " median ratio " + juce::String (ratiosDb[4], 2) + "dB");
            // ゲインの変更（±1dB）を検出する帯。音域ごとの値（低 0.92・高 1.08）の差は、この中央値の差で決まっている（実録音でも同じ向きの差が出た）。
            expect (ratiosDb[4] >= 0.0 && ratiosDb[4] <= 1.2, "median level ratio out of the calibrated band 0..+1.2 dB: " + juce::String (ratiosDb[4], 2) + "dB (range " + vc::talkboxRangeId (range) + ")");
        }
    }
    // Engineをブロック処理する（480サンプルずつ）。
    static void processEngine (vc::Engine& engine, float* data, int n)
    {
        for (int pos = 0; pos < n; pos += 480)
            engine.process (data + pos, std::min (480, n - pos));
    }

    static std::vector<float> concat (const std::vector<std::vector<float>>& parts)
    {
        std::vector<float> all;
        for (const auto& p : parts)
            all.insert (all.end(), p.begin(), p.end());
        return all;
    }

    // ----- K1: ケロケロ -----
    // 信号をケロケロで処理し、最後の1秒の基本波周波数(Hz)を返す。
    double kerokeroOutputHz (const std::vector<float>& signal, double approxOutHz, bool sineMode)
    {
        vc::Engine engine;
        engine.prepare ({ 48000.0, 480 });
        engine.params().preset.store ((int) vc::Preset::Kerokero);
        std::vector<float> out (signal);
        processEngine (engine, out.data(), (int) out.size());
        const int len = 48000;
        const float* seg = out.data() + (int) out.size() - len;
        return sineMode ? vc::test::findFftPeakHz (seg, len, 48000.0)
                        : vc::test::measureFundamentalHz (seg, len, 48000.0, approxOutHz);
    }

    void runK1()
    {
        beginTest ("K1: ケロケロ 450Hz→440Hz、425Hz→415.3Hz、母音150Hz→146.8Hz、無音を挟んでも補正量が不変");

        constexpr double fs = 48000.0;
        const int n = (int) (3.0 * fs);

        const double f450 = kerokeroOutputHz (vc::test::makeSine (450.0, fs, n, 0.3f), 440.0, true);
        const double f425 = kerokeroOutputHz (vc::test::makeSine (425.0, fs, n, 0.3f), 415.3, true);
        const double fVowel = kerokeroOutputHz (vc::test::makeSyntheticVowel (150.0, fs, n), 146.8, false);
        logMessage ("K1: 450Hz -> " + juce::String (f450, 2) + "Hz, 425Hz -> " + juce::String (f425, 2)
                    + "Hz, vowel150 -> " + juce::String (fVowel, 2) + "Hz");

        expectWithinAbsoluteError (f450, 440.0, 4.4);
        expectWithinAbsoluteError (f425, 415.3, 4.153);
        expectWithinAbsoluteError (fVowel, 146.8, 1.468);

        // 100msの無音（デジタル無音）を挟んでも補正量が変わらない。
        {
            vc::Engine engine;
            engine.prepare ({ fs, 480 });
            engine.params().preset.store ((int) vc::Preset::Kerokero);

            auto tone = vc::test::makeSine (450.0, fs, (int) fs, 0.3f);
            processEngine (engine, tone.data(), (int) tone.size());
            const float before = engine.getKerokeroCorrectionSemitones();

            std::vector<float> silence ((size_t) (0.1 * fs), 0.0f);
            processEngine (engine, silence.data(), (int) silence.size());
            const float after = engine.getKerokeroCorrectionSemitones();

            logMessage ("K1: correction before/after silence " + juce::String (before, 4) + " / " + juce::String (after, 4));
            expectWithinAbsoluteError (before, -0.3903f, 0.02f);
            expectWithinAbsoluteError (after, before, 0.01f);
        }
    }

    // ----- X3: トークボックス（Engine経由） -----
    // 現在のステップ（0から数える）の窓（ステップの始まりの2000サンプル後から9000サンプル）で、h次倍音（約1200Hz付近）のピークから求めたキャリアの基本周波数（Hz）。
    // 窓はステップの中（1ステップ約11707サンプル）に収まり、音の切替の過渡を避ける。高い倍音ほど周波数の測定誤差が基本周波数に小さく効く。切替は時刻startOffsetに起きたものとして数える。
    // 倍音のピークがなければ負（呼び出し側が失敗にする。measureFundamentalHzは見つからないと目安を返すため使わない）。
    // checkOctaveがtrueなら、1オクターブ低いキャリアでないことも確かめる。声（母音）の包絡は声のf0（100〜250Hz）の側波帯をキャリアの各倍音の両脇に作り、
    // 倍音の間にも成分が出るため、母音を入力にした測定では使えない（雑音入力で使う）。
    double measuredCarrierHz (const std::vector<float>& out, double fs, int step, long long startOffset, double expectedHz, bool checkOctave = false) const
    {
        vc::PhraseSequencer seq;
        seq.prepare (fs);
        const long long from = startOffset + seq.boundaryOf (step) + 2000;
        const int harmonic = std::max (1, (int) std::lround (1200.0 / expectedHz));
        const auto spec = vc::test::averagedMagnitudeSpectrum (out.data() + from, 9000);
        const auto peak = vc::test::findPeakNear (spec, fs, 1 << 14, expectedHz * harmonic, 0.03);

        // 1オクターブ低いキャリア（基本波が半分）も、期待する倍音の位置に倍音を持つ（偶数次）ため、それだけでは区別できない。
        // 期待する倍音の間（(h+0.5)倍）に倍音があれば基本波は半分以下なので、失敗にする。
        if (checkOctave)
        {
            const auto between = vc::test::findPeakNear (spec, fs, 1 << 14, expectedHz * (harmonic + 0.5), std::min (0.02, 0.25 / (harmonic + 0.5)));

            if (between.freqHz > 0.0 && between.magLinear > 0.5 * peak.magLinear)
                return -1.0;
        }

        return peak.freqHz > 0.0 ? peak.freqHz / harmonic : -1.0;
    }

    // キャリアの下限は40Hz（Talkbox内。層1ピッチ-24で低い音が40Hz未満になるとき）。
    static double expectedCarrierHz (int step, int pitch, vc::TalkboxRange range)
    {
        return std::max (40.0, kPhraseHz[(size_t) (step % 8)] * std::pow (2.0, (double) pitch / 12.0) * (range == vc::TalkboxRange::High ? 2.0 : 1.0));
    }

    void runX3()
    {
        beginTest ("X3: トークボックス（Engine） 出力RMS比±3dB、倍音間隔 = 現在のフレーズの音 × 2^(p/12)（検出f0・声の母音によらない）、雑音入力で-40dB超");

        constexpr double fs = 48000.0;
        const int n = (int) (3.0 * fs);
        const int len = (int) fs;
        const std::array<std::array<double, 3>, 3> vowels { { { 730.0, 1090.0, 2440.0 }, { 270.0, 2290.0, 3010.0 }, { 300.0, 870.0, 2240.0 } } };
        double worstDb = 0.0;
        double worstSpacingPct = 0.0;
        int caseIdx = 0;

        // 音量は層1ピッチ0で比較する。ピッチシフター自体が強い倍音構造の合成母音で音量を最大約9dB
        // 変える（Signalsmithの特性。DBGで確認済み）ため、ピッチ != 0では入力比では測れない。
        // 倍音のピークはデチューン（±10セント = ±0.6%）の2本に分かれるため、測定の許容は1%。
        // 倍音間隔は層1ピッチ（0/+5/-7/+24/-24）・音域（低/高）・ステップ（2〜9）を9通りの母音・f0に割り当てて測る。検出f0（100/150/250Hz）が違っても、
        // キャリアは同じフレーズの音になる。
        for (const auto& fm : vowels)
        {
            for (const double f0 : { 100.0, 150.0, 250.0 })
            {
                const int pitchForSpacing = std::array<int, 5> { 0, 5, -7, 24, -24 }[(size_t) (caseIdx % 5)];
                const auto rangeForSpacing = caseIdx % 2 == 0 ? vc::TalkboxRange::Low : vc::TalkboxRange::High;
                const int step = 2 + caseIdx % 8;
                ++caseIdx;
                const float amp = 0.1f * 1.41421356f * 1.5f;
                const auto in = vc::test::makeSyntheticVowel (f0, fs, n, fm, { 80.0, 90.0, 120.0 }, amp);

                // 声量（層1ピッチ0、音域は低と高の両方）
                for (const auto range : { vc::TalkboxRange::Low, vc::TalkboxRange::High })
                {
                    vc::Engine engine;
                    engine.prepare ({ fs, 480 });
                    engine.params().preset.store ((int) vc::Preset::Talkbox);
                    engine.params().talkboxRange.store ((int) range);
                    std::vector<float> out (in);
                    processEngine (engine, out.data(), n);

                    const double ratioDb = 20.0 * std::log10 (vc::test::rms (out.data() + n - len, len) / vc::test::rms (in.data() + n - len, len));
                    logMessage ("X3: range=" + juce::String (vc::talkboxRangeId (range)) + " F1=" + juce::String (fm[0]) + " f0=" + juce::String (f0) + " ratio " + juce::String (ratioDb, 2) + "dB");
                    worstDb = std::max (worstDb, std::abs (ratioDb));
                }

                // 倍音間隔
                vc::Engine engine;
                engine.prepare ({ fs, 480 });
                engine.params().preset.store ((int) vc::Preset::Talkbox);
                engine.params().talkboxRange.store ((int) rangeForSpacing);
                engine.params().pitch.store (pitchForSpacing);
                std::vector<float> out (in);
                processEngine (engine, out.data(), n);

                const double expected = expectedCarrierHz (step, pitchForSpacing, rangeForSpacing);
                const double measured = measuredCarrierHz (out, fs, step, 0, expected);
                const double errPct = measured > 0.0 ? std::abs (measured - expected) / expected * 100.0 : 100.0;
                logMessage ("X3: F1=" + juce::String (fm[0]) + " f0=" + juce::String (f0) + " pitch=" + juce::String (pitchForSpacing) + " range=" + juce::String (vc::talkboxRangeId (rangeForSpacing))
                            + " step " + juce::String (step) + ": carrier " + juce::String (measured, 2) + "Hz, expected " + juce::String (expected, 2) + "Hz, err " + juce::String (errPct, 3) + "%");
                worstSpacingPct = std::max (worstSpacingPct, errPct);
            }
        }

        expect (worstDb <= 3.0, "talkbox level ratio worst " + juce::String (worstDb, 2) + "dB");
        expect (worstSpacingPct <= 1.0, "harmonic spacing error worst " + juce::String (worstSpacingPct, 3) + "%");

        // 雑音入力（-20dBFS RMS）でも出力が-40dB超（キャリアが雑音になり無音にならない）。
        {
            vc::Engine engine;
            engine.prepare ({ fs, 480 });
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            std::vector<float> in ((size_t) n);
            juce::Random rng (77);
            for (auto& v : in)
                v = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f * 1.7320508f;
            std::vector<float> out (in);
            processEngine (engine, out.data(), n);
            const double ratioDb = 20.0 * std::log10 (vc::test::rms (out.data() + n - len, len) / vc::test::rms (in.data() + n - len, len));
            logMessage ("X3: noise in -> " + juce::String (ratioDb, 2) + "dB");
            expect (ratioDb > -40.0, "noise input output level " + juce::String (ratioDb, 2) + "dB");
        }
    }

    // ----- X3b: フレーズ（PhraseSequencer）。テンポ・音程・巡回・リセット -----
    void runX3Phrase()
    {
        beginTest ("X3b: PhraseSequencer 123BPM・8分音符のステップ境界（整数サンプルの計算でドリフトなし）、フレーズの音程、巡回、reset");

        const auto& phraseHz = kPhraseHz;

        for (const double fs : { 48000.0, 44100.0, 96000.0 })
        {
            vc::PhraseSequencer seq;
            seq.prepare (fs);

            // ステップ長 = 60/123 × 0.5 秒 = fs × 30 / 123 サンプル。k番目のステップの始まりは ceil(k × fs × 30 / 123) 。
            // 10分ぶん（約2460ステップ）を回し、境界ごとに切替のサンプルが理論値ぴったりであることと、音が正しいことを確かめる。
            const long long total = (long long) (600.0 * fs);
            const long long samplesPerStepNumerator = (long long) std::llround (fs) * 30;
            long long observed = 0;
            int mismatches = 0;
            double worstHzErr = 0.0;

            for (long long i = 0; i < total; ++i)
            {
                const float hz = seq.next();

                if (seq.getStepIndex() != observed)
                {
                    ++observed;
                    const long long expectedBoundary = (observed * samplesPerStepNumerator + 122) / 123; // ceil
                    mismatches += (i != expectedBoundary || seq.getStepIndex() != observed) ? 1 : 0;
                }

                if (i % 997 == 0 || seq.getStepIndex() != observed)
                    worstHzErr = std::max (worstHzErr, std::abs ((double) hz - phraseHz[(size_t) (observed % 8)]) / phraseHz[(size_t) (observed % 8)]);
            }

            logMessage ("X3b: fs=" + juce::String (fs, 0) + " mismatched step numbers " + juce::String (mismatches) + ", worst note frequency error " + juce::String (worstHzErr * 100.0, 4) + "%");
            expectEquals (mismatches, 0, "step boundaries are not ceil(k * L) at fs=" + juce::String (fs, 0));
            expect (worstHzErr < 1.0e-4, "phrase note frequency error " + juce::String (worstHzErr));
        }

        // 48kHz: 最初の境界は11708サンプル目（ceil(11707.317)）、8ステップ（1フレーズ）は ceil(8 × 11707.317) = 93659 サンプル目に1周する。
        {
            vc::PhraseSequencer seq;
            seq.prepare (48000.0);
            expectEquals ((int) seq.boundaryOf (1), 11708);
            expectEquals ((int) seq.boundaryOf (2), 23415);
            expectEquals ((int) seq.boundaryOf (8), 93659);
            expectEquals ((int) seq.boundaryOf (41), 480000); // 41ステップ = ちょうど10秒（60/123 × 0.5 × 41 = 10秒の整数サンプル）
            expect (seq.boundaryOf (41000) == 480000000LL && seq.boundaryOf (123000) == 1440000000LL, "boundary drifts over 10000 s"); // 1万秒（約2.8時間）でも小数の丸めがずれない

            // 切替のサンプルで音が変わり、その前のサンプルは変わらない（階段）。
            std::vector<float> hz;
            for (int i = 0; i < 11709; ++i)
                hz.push_back (seq.next());

            expect (std::abs (hz[11706] - hz[0]) < 1.0e-6f && std::abs (hz[11707] - hz[0]) < 1.0e-6f, "note changed before the boundary");
            expectEquals ((double) hz[11707], (double) hz[0]); // サンプル11707（0始まり）はまだ最初のステップ
            expect (std::abs (hz[11708] - hz[0]) < 1.0e-6f, "boundary sample should be the 2nd G2 (the phrase starts with G2 G2)");
        }

        // reset: 先頭のステップ（時刻0）へ戻る。途中でresetしても、その後は先頭から同じ列を再生する。
        {
            vc::PhraseSequencer a, b;
            a.prepare (48000.0);
            b.prepare (48000.0);

            for (int i = 0; i < 100000; ++i)
                a.next();

            a.reset();

            for (int i = 0; i < 100000; ++i)
                expect (std::abs (a.next() - b.next()) < 1.0e-9f, "reset did not restart the phrase from the first note");
        }

        // 1フレーズ（8ステップ）の長さは4拍 = 1.9512秒。フレーズ内で最も低い音（F2 = 87.31Hz）の次に先頭のG2（98.00Hz）へ戻る。
        {
            vc::PhraseSequencer seq;
            seq.prepare (48000.0);
            float last = 0.0f;
            long long lastChange = 0;

            for (long long i = 0; i < 93659 + 20; ++i)
            {
                const float hz = seq.next();

                if (i == 93659 - 1)
                    expectWithinAbsoluteError ((double) hz, 87.307, 0.01); // 8番目の音（F2）の最後のサンプル

                if (i == 93659)
                    expectWithinAbsoluteError ((double) hz, 98.0, 0.01); // 1周して先頭の音

                if (std::abs (hz - last) > 1.0e-6f)
                    lastChange = i;

                last = hz;
            }

            expectEquals ((int) lastChange, 93659);
        }
    }

    // ----- X3c: 開始位相・状態の復帰・音域の切替（Engine） -----
    // キャリアの位置はEngine（時刻0から）の経過で決まり、プリセットの切替・バイパス解除・NaN回復・prepareのたびに先頭の音から始め直す（D-028）。
    void runX3Engine()
    {
        beginTest ("X3c: トークボックスのフレーズは、選択・バイパス解除・NaN回復・prepareのたびに先頭の音から始まり、以後は自走する。音域の切替はフレーズの位置を変えない");

        constexpr double fs = 48000.0;
        constexpr int block = 480;
        // 入力は白色雑音（-20dBFS RMS）。キャリアの周波数を測るため、声の側波帯（キャリアの倍音の両脇）が出ない入力にする。有声度は0（無声）で、キャリアは下限0.8の鋸波が主。
        // 声のf0（100〜250Hz）に依らないことは、母音を入力にしたX3が確かめる。
        std::vector<float> vowel ((size_t) (6.0 * fs));
        juce::Random noiseRng (31);

        for (auto& v : vowel)
            v = (noiseRng.nextFloat() * 2.0f - 1.0f) * 0.1f * 1.7320508f;

        // 開始位置 start（ブロックの境界）からの出力を作り、step番目の音のキャリア周波数を測る。
        auto expectStep = [&] (const std::vector<float>& out, long long start, int step, int pitch, vc::TalkboxRange range, const juce::String& label)
        {
            const double expected = expectedCarrierHz (step, pitch, range);
            const double measured = measuredCarrierHz (out, fs, step, start, expected, true);
            expect (measured > 0.0 && std::abs (measured - expected) / expected < 0.01, label + " step " + juce::String (step) + ": carrier " + juce::String (measured, 2) + "Hz, expected " + juce::String (expected, 2) + "Hz");
        };

        // (1) prepare直後のTalkbox選択: 時刻0から。
        {
            vc::Engine engine;
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            engine.prepare ({ fs, block });
            std::vector<float> out (vowel);
            processEngine (engine, out.data(), (int) out.size());

            for (const int step : { 0, 1, 2, 4, 7, 8, 9, 10 }) // 0〜1のG2、巡回後（8〜10）も
                expectStep (out, 0, step, 0, vc::TalkboxRange::Low, "prepare");
        }

        // (2) 途中でプリセットを切り替えて（ノーマル → トークボックス）選択したとき: 切替の時刻から先頭の音。
        // 2回目（トークボックス → エコー → トークボックス）も、前回の位置から続けず先頭に戻る。
        {
            vc::Engine engine;
            engine.prepare ({ fs, block });
            engine.params().preset.store ((int) vc::Preset::Normal);
            std::vector<float> out (vowel);
            const int switchAt = block * 70; // 0.7秒
            processEngine (engine, out.data(), switchAt);
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            processEngine (engine, out.data() + switchAt, block * 170); // 1.7秒 = 約7ステップ（フレーズの途中で切り替える）

            for (const int step : { 0, 1, 2, 3, 5 })
                expectStep (out, switchAt, step, 0, vc::TalkboxRange::Low, "select");

            const int echoAt = switchAt + block * 170;
            engine.params().preset.store ((int) vc::Preset::Echo);
            processEngine (engine, out.data() + echoAt, block * 30); // 切替の途中経過（フェード）を済ませる
            const int again = echoAt + block * 30;
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            processEngine (engine, out.data() + again, block * 170);

            for (const int step : { 0, 1, 2, 3, 5 })
                expectStep (out, again, step, 0, vc::TalkboxRange::Low, "reselect");
        }

        // (3) バイパス解除: 全体OFFのあいだ止まり、ONへ戻した時刻から先頭の音。
        {
            vc::Engine engine;
            engine.prepare ({ fs, block });
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            std::vector<float> out (vowel);
            processEngine (engine, out.data(), block * 100);
            engine.params().enabled.store (false);
            processEngine (engine, out.data() + block * 100, block * 50); // 完全バイパス（20msのクロスフェード後はチェーンを通さない）
            engine.params().enabled.store (true);
            const int back = block * 150;
            processEngine (engine, out.data() + back, block * 200);

            for (const int step : { 0, 1, 2, 3, 5 })
                expectStep (out, back, step, 0, vc::TalkboxRange::Low, "bypass release");
        }

        // (4) NaN回復: 入力にNaNを入れたブロックはEngineがリセットする。次のブロックから先頭の音。
        {
            vc::Engine engine;
            engine.prepare ({ fs, block });
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            std::vector<float> out (vowel);
            processEngine (engine, out.data(), block * 100);
            out[(size_t) (block * 100 + 10)] = std::numeric_limits<float>::quiet_NaN();
            processEngine (engine, out.data() + block * 100, block); // このブロックは無音になりリセットされる
            expect ((engine.getErrorFlags() & 1u) != 0, "NaN was not detected");
            const int after = block * 101;
            processEngine (engine, out.data() + after, block * 200);

            for (const int step : { 0, 1, 2, 3, 5 })
                expectStep (out, after, step, 0, vc::TalkboxRange::Low, "NaN recovery");
        }

        // (5) 音域の切替: 稼働中に低 → 高にしてもフレーズは先頭へ戻らず、同じステップで1オクターブ上になる。層1ピッチとも合成される。
        {
            vc::Engine engine;
            engine.prepare ({ fs, block });
            engine.params().preset.store ((int) vc::Preset::Talkbox);
            std::vector<float> out (vowel);
            const int toHigh = block * 120; // 1.2秒（ステップ4: 0.976〜1.22秒の途中。ステップ5以降で測る）
            processEngine (engine, out.data(), toHigh);
            engine.params().talkboxRange.store ((int) vc::TalkboxRange::High);
            processEngine (engine, out.data() + toHigh, block * 150);

            for (const int step : { 6, 7, 8, 9 })
                expectStep (out, 0, step, 0, vc::TalkboxRange::High, "switched to high");

            for (const int step : { 2, 3 })
                expectStep (out, 0, step, 0, vc::TalkboxRange::Low, "before the switch");
        }

        // (5b) Engineが読む音域が不正な値のときは低（設定ファイルの破損が直接届いても、鳴らない・NaNにはならない）。低と同じ出力。
        {
            std::vector<float> a (vowel), b (vowel);

            for (const int value : { 0, 99 })
            {
                vc::Engine engine;
                engine.prepare ({ fs, block });
                engine.params().preset.store ((int) vc::Preset::Talkbox);
                engine.params().talkboxRange.store (value);
                auto& out = value == 0 ? a : b;
                processEngine (engine, out.data(), block * 100);
            }

            expect (std::equal (a.begin(), a.begin() + block * 100, b.begin()), "an invalid range value did not behave as low");
        }

        // (6) 音域のオクターブ差: 同じステップで低 → 高は周波数がちょうど2倍（キャリアの周波数の比）。
        for (const int pitch : { 0, 7, -12 })
        {
            double hz[2] {};

            for (const auto range : { vc::TalkboxRange::Low, vc::TalkboxRange::High })
            {
                vc::Engine engine;
                engine.prepare ({ fs, block });
                engine.params().preset.store ((int) vc::Preset::Talkbox);
                engine.params().talkboxRange.store ((int) range);
                engine.params().pitch.store (pitch);
                std::vector<float> out (vowel);
                processEngine (engine, out.data(), block * 150);
                hz[(size_t) range] = measuredCarrierHz (out, fs, 4, 0, expectedCarrierHz (4, pitch, range), true);
            }

            // ±10セントのデチューンで倍音が2本に分かれ（約±0.6%）、どちらに寄ったピークを取るかで測定値が動くため、許容は3%（半音は6%、1オクターブは100%）。
            expect (hz[0] > 0.0 && hz[1] > 0.0 && std::abs (hz[1] / hz[0] - 2.0) < 0.06, "pitch " + juce::String (pitch) + ": high/low carrier ratio " + juce::String (hz[1] / hz[0], 4));
        }
    }
    // ----- F1〜F3: フォルマント移動（Engineのヘリウム・ミニオン・ジャイアント） -----
    // 倍音位置k*f0のピーク3点の放物線補間でフォルマント（loHz〜hiHz内で最大の倍音まわり）を求める。
    static double formantPeakHz (const std::vector<double>& spectrum, double fs, double f0, double loHz, double hiHz)
    {
        int bestK = -1;
        double bestMag = -1.0;

        for (int k = (int) std::ceil (loHz / f0); (double) k * f0 <= hiHz; ++k)
        {
            const auto pk = vc::test::findPeakNear (spectrum, fs, 1 << 14, (double) k * f0, 0.03);
            if (pk.freqHz > 0.0 && pk.magLinear > bestMag)
            {
                bestMag = pk.magLinear;
                bestK = k;
            }
        }

        if (bestK < 2)
            return -1.0;

        const auto a = vc::test::findPeakNear (spectrum, fs, 1 << 14, (double) (bestK - 1) * f0, 0.03);
        const auto b = vc::test::findPeakNear (spectrum, fs, 1 << 14, (double) bestK * f0, 0.03);
        const auto c = vc::test::findPeakNear (spectrum, fs, 1 << 14, (double) (bestK + 1) * f0, 0.03);
        const double la = std::log (a.magLinear), lb = std::log (b.magLinear), lc = std::log (c.magLinear);
        const double denom = la - 2.0 * lb + lc;
        const double delta = std::abs (denom) > 1.0e-12 ? 0.5 * (la - lc) / denom : 0.0;
        return ((double) bestK + juce::jlimit (-1.0, 1.0, delta)) * f0;
    }

    void runFormantCase (const juce::String& label, vc::Preset preset, double f0, double semis, double expectedS,
                         bool enforce, double f0Tol, double sTol)
    {
        constexpr double fs = 48000.0;
        constexpr int analysisLen = (int) (fs * 1.5);
        const int totalLen = (int) (fs * 3.0) + analysisLen;

        auto vowel = vc::test::makeSyntheticVowel (f0, fs, totalLen, { 730.0, 1090.0, 2440.0 }, { 80.0, 90.0, 120.0 }, 0.3f);
        vc::test::addNoiseFloor (vowel, -80.0f, 222);

        vc::Engine engine;
        engine.prepare ({ fs, 480 });
        engine.params().preset.store ((int) preset);
        std::vector<float> out (vowel);
        processEngine (engine, out.data(), totalLen);

        const float* steadyIn = vowel.data() + (totalLen - analysisLen);
        const float* steadyOut = out.data() + (totalLen - analysisLen);

        const double f0In = vc::test::measureFundamentalHz (steadyIn, analysisLen, fs, f0);
        const double expectedRatio = std::pow (2.0, semis / 12.0);
        const double f0Out = vc::test::measureFundamentalHz (steadyOut, analysisLen, fs, f0In * expectedRatio);
        const double f0Ratio = f0Out / f0In;

        const auto envIn = vc::test::measureFormantEnvelope (steadyIn, analysisLen, fs, f0In);
        const auto envOut = vc::test::measureFormantEnvelope (steadyOut, analysisLen, fs, f0Out);
        const double s = vc::test::measureEnvelopeScale (envIn, envOut);

        const auto specIn = vc::test::averagedMagnitudeSpectrum (steadyIn, analysisLen);
        const auto specOut = vc::test::averagedMagnitudeSpectrum (steadyOut, analysisLen);
        const double f1In = formantPeakHz (specIn, fs, f0In, 450.0, 900.0);
        const double f2In = formantPeakHz (specIn, fs, f0In, 950.0, 1600.0);
        const double f1Out = formantPeakHz (specOut, fs, f0Out, 450.0 * expectedS, 900.0 * expectedS);
        const double f2Out = formantPeakHz (specOut, fs, f0Out, 950.0 * expectedS, 1600.0 * expectedS);

        logMessage (label + ": f0 " + juce::String (f0In, 2) + " -> " + juce::String (f0Out, 2) + "Hz (ratio " + juce::String (f0Ratio, 4)
                    + ", expected " + juce::String (expectedRatio, 4) + "), s=" + juce::String (s, 3) + " (expected " + juce::String (expectedS, 2)
                    + "), F1 " + juce::String (f1In, 0) + " -> " + juce::String (f1Out, 0) + "Hz, F2 " + juce::String (f2In, 0) + " -> " + juce::String (f2Out, 0) + "Hz");

        if (enforce)
        {
            expect (std::abs (f0Ratio - expectedRatio) / expectedRatio <= f0Tol, label + ": f0 ratio " + juce::String (f0Ratio, 4));
            expect (std::abs (s - expectedS) / expectedS <= sTol, label + ": envelope scale s=" + juce::String (s, 3));
        }
    }

    void runF()
    {
        struct Case { const char* name; vc::Preset preset; double semis; double s; };
        const std::array<Case, 3> cases { { { "F1 ヘリウム", vc::Preset::Helium, 9.0, 1.6 },
                                             { "F2 ミニオン", vc::Preset::Minion, 12.0, 1.6 },
                                             { "F3 ジャイアント", vc::Preset::Giant, -6.0, 0.75 } } };

        // f0=150Hz: 合格基準（f0比±1%、s=期待値±10%）。
        for (const auto& c : cases)
        {
            beginTest (juce::String (c.name) + " f0=150Hz");
            runFormantCase (c.name, c.preset, 150.0, c.semis, c.s, true, 0.01, 0.1);
        }

        // f0=100Hz: 低い声の特性確認（D-015。失敗判定なし、実測値をログへ）。
        for (const auto& c : cases)
        {
            beginTest (juce::String (c.name) + " f0=100Hz（特性確認）");
            runFormantCase (juce::String (c.name) + " f0=100", c.preset, 100.0, c.semis, c.s, false, 0.0, 0.0);
        }
    }
};

static EffectsTests effectsTests;

} // namespace
