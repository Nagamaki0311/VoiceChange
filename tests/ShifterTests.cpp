#include <juce_core/juce_core.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include "TestSignals.h"
#include "core/PitchShifter.h"

#include <cmath>
#include <functional>
#include <vector>

// ===== SECTION: ShifterTests =====
// P1〜P5（カテゴリShifter、quick）。docs/plan.md 3章T-004参照。
// クリック判定器の自己テストもここに含める（docs/plan.md「クリック判定器」節）。

namespace
{

// posからpredがtrueになるかバッファ終端に達するまでmaxBlockずつshifterへ供給する。
// predには各ブロック処理後の現在位置(pos)が渡される。戻り値は最終的なpos。
int feedUntil (vc::PitchShifter& shifter, const float* signal, float* out, int maxBlock,
               int pos, int bufferLen, const std::function<bool (int)>& pred)
{
    while (pos + maxBlock <= bufferLen)
    {
        shifter.process (signal + pos, out + pos, maxBlock);
        pos += maxBlock;

        if (pred (pos))
            break;
    }

    return pos;
}

class ShifterSmokeTests final : public juce::UnitTest
{
public:
    ShifterSmokeTests() : juce::UnitTest ("ShifterSmoke", "Smoke") {}

    void runTest() override
    {
        beginTest ("configure(1, 960, 240) at 48kHz reports ~960 samples of latency");
        {
            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.configure (1, 960, 240);

            const int latency = stretch.inputLatency() + stretch.outputLatency();
            expect (latency >= static_cast<int> (960 * 0.95) && latency <= static_cast<int> (960 * 1.05),
                    "latency " + juce::String (latency) + " out of expected range");
        }

        beginTest ("processing 1 second of a sine wave produces finite output");
        {
            constexpr double sampleRate = 48000.0;
            constexpr int blockSamples = 960;

            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.configure (1, blockSamples, blockSamples / 4);
            stretch.setTransposeSemitones (0.0f);
            stretch.setFormantFactor (1.0f, true);

            const int totalSamples = static_cast<int> (sampleRate); // 1秒分
            const auto input = vc::test::makeSine (220.0, sampleRate, totalSamples, 0.5f);
            std::vector<float> output (static_cast<size_t> (totalSamples), 0.0f);

            const int chunk = blockSamples / 4;

            for (int pos = 0; pos + chunk <= totalSamples; pos += chunk)
            {
                const float* inChannels[1] = { input.data() + pos };
                float* outChannels[1] = { output.data() + pos };
                stretch.process (inChannels, chunk, outChannels, chunk);
            }

            const bool allFinite = vc::test::allFinite (output.data(), totalSamples);
            expect (allFinite, "shifter output contained non-finite values");
        }
    }
};

static ShifterSmokeTests shifterSmokeTests;

class ShifterTests final : public juce::UnitTest
{
public:
    ShifterTests() : juce::UnitTest ("Shifter", "Shifter") {}

    void runTest() override
    {
        runClickDetectorSelfTests();
        runP1();
        runP2();
        runP3();
        runP4();
        runP5();
    }

private:
    // ----- クリック判定器の自己テスト -----
    void runClickDetectorSelfTests()
    {
        constexpr double fs = 48000.0;
        constexpr double freq = 200.0;
        constexpr int steadyLen = (int) fs; // 1秒
        constexpr int transitionLen = 8;    // 境界の前後ごく少数

        beginTest ("クリック判定器の自己テスト: 位相が跳ぶ単純連結は検出する");
        {
            auto a = vc::test::makeSine (freq, fs, steadyLen + transitionLen / 2, 0.3f);
            auto b = vc::test::makeSine (freq, fs, steadyLen + transitionLen / 2, 0.3f);

            // bの位相をπずらして単純連結する（境界で不連続）。
            for (auto& s : b)
                s = -s;

            std::vector<float> buf;
            buf.insert (buf.end(), a.begin(), a.end());
            buf.insert (buf.end(), b.begin(), b.end());

            const int boundary = (int) a.size();
            const float* steadyBefore = buf.data() + (boundary - steadyLen);
            const float* transition = buf.data() + (boundary - transitionLen / 2);
            const float* steadyAfter = buf.data() + (boundary + transitionLen / 2);

            const bool noClick = vc::test::checkNoClick (steadyBefore, steadyLen, transition, transitionLen, steadyAfter);
            expect (! noClick, "phase-jump splice was not detected as a click");
        }

        beginTest ("クリック判定器の自己テスト: 20msの二乗余弦クロスフェードは合格する");
        {
            const int fadeLen = (int) std::lround (0.020 * fs);
            const int totalLen = steadyLen * 2; // 前後とも定常1秒分を確保する

            auto a = vc::test::makeSine (freq, fs, totalLen, 0.3f);
            auto b = vc::test::makeSine (freq, fs, totalLen, 0.3f);

            for (auto& s : b)
                s = -s;

            std::vector<float> buf (a);

            const int boundary = steadyLen;

            for (int i = 0; i < fadeLen; ++i)
            {
                const double t = (double) i / (double) fadeLen;
                const double gA = std::cos (juce::MathConstants<double>::halfPi * t);
                const double gB = std::sin (juce::MathConstants<double>::halfPi * t);
                const double gA2 = gA * gA;
                const double gB2 = gB * gB;
                buf[(size_t) (boundary + i)] = (float) (gA2 * (double) a[(size_t) (boundary + i)] + gB2 * (double) b[(size_t) (boundary + i)]);
            }

            for (int i = boundary + fadeLen; i < totalLen; ++i)
                buf[(size_t) i] = b[(size_t) i];

            const float* steadyBefore = buf.data() + (boundary - steadyLen);
            const float* transition = buf.data() + boundary;
            const float* steadyAfter = buf.data() + (boundary + fadeLen);

            const bool noClick = vc::test::checkNoClick (steadyBefore, steadyLen, transition, fadeLen, steadyAfter);
            expect (noClick, "raised-cosine crossfade was flagged as a click");
        }
    }

    // ----- P1: 状態遷移 -----
    void runP1()
    {
        constexpr double fs = 48000.0;
        constexpr int stretchBlock = 960; // 20ms

        signalsmith::stretch::SignalsmithStretch<float> ref;
        ref.configure (1, stretchBlock, stretchBlock / 4);
        const int expectedLatency = ref.inputLatency() + ref.outputLatency();
        const int expectedFadeLen = (int) std::lround (0.020 * fs);

        beginTest ("P1a: Priming→FadingInの送り込み量とレイテンシ表示");
        {
            constexpr int maxBlock = 480;

            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            expect (shifter.getState() == vc::PitchShifter::State::Resting);
            expect (shifter.getLatencySamples() == 0);

            auto signal = vc::test::makeSine (220.0, fs, maxBlock * 400, 0.3f);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (true, 8.0f, 1.4f);

            const int primingToFadingInAt = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::FadingIn; });

            expect (primingToFadingInAt > 0, "did not reach FadingIn");
            expectWithinAbsoluteError ((double) primingToFadingInAt, (double) expectedLatency, (double) maxBlock);
            expect (shifter.getLatencySamples() == expectedLatency, "FadingIn中のレイテンシ表示がLと一致しない");

            const int reachedActiveAt = feedUntil (shifter, signal.data(), out.data(), maxBlock, primingToFadingInAt, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            juce::ignoreUnused (reachedActiveAt);

            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");
            expect (shifter.getLatencySamples() == expectedLatency, "Active中のレイテンシ表示がLと一致しない");
        }

        beginTest ("P1b: FadingInの長さは20ms±1サンプル");
        {
            constexpr int maxBlock = 1; // サンプル精度で測るためブロック長を1にする

            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);
            shifter.setTarget (true, 8.0f, 1.4f);

            auto signal = vc::test::makeSine (220.0, fs, expectedLatency + expectedFadeLen + 4000, 0.3f);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::FadingIn; });

            expect (shifter.getState() == vc::PitchShifter::State::FadingIn, "did not enter FadingIn");

            int fadeLenMeasured = 0;

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int)
                {
                    ++fadeLenMeasured;
                    return shifter.getState() != vc::PitchShifter::State::FadingIn;
                });
            juce::ignoreUnused (pos);

            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");
            expectWithinAbsoluteError ((double) fadeLenMeasured, (double) expectedFadeLen, 1.0);
        }

        beginTest ("P1c: Resting/PrimingではL=0、稼働要求でResting→Priming");
        {
            constexpr int maxBlock = 480;
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (220.0, fs, maxBlock * 4, 0.3f);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (false, 0.0f, 1.0f);
            shifter.process (signal.data(), out.data(), maxBlock);
            expect (shifter.getState() == vc::PitchShifter::State::Resting);
            expect (shifter.getLatencySamples() == 0);

            shifter.setTarget (true, 8.0f, 1.4f);
            shifter.process (signal.data() + maxBlock, out.data() + maxBlock, maxBlock);
            expect (shifter.getState() == vc::PitchShifter::State::Priming);
            expect (shifter.getLatencySamples() == 0);
        }
    }

    // 遷移区間の前後1秒(定常)＋遷移区間でクリック判定する共通処理。
    // transitionStartからtransitionEndの間が「切替区間」、[transitionEnd, transitionEnd+steadyLen)を
    // steadyAfterとして使う。呼び出し前にtransitionEnd+steadyLenまでshifterへの供給が済んでいること。
    bool checkTransitionNoClick (const std::vector<float>& out, int transitionStart, int transitionEnd, int steadyLen)
    {
        return vc::test::checkNoClick (out.data() + (transitionStart - steadyLen), steadyLen,
                                        out.data() + transitionStart, transitionEnd - transitionStart,
                                        out.data() + transitionEnd);
    }

    // ----- P2: 全遷移のクリック -----
    void runP2()
    {
        constexpr double fs = 48000.0;
        constexpr int maxBlock = 480;
        constexpr int stretchBlock = 960;
        constexpr float semis = 8.0f;
        constexpr float formant = 1.4f;
        constexpr double toneFreq = 200.0;
        constexpr float toneAmp = 0.3f;
        constexpr int steadyLen = (int) fs; // 1秒
        constexpr int margin = 6 * maxBlock;
        constexpr int totalLen = (int) fs * 4 + 20000; // 4秒 + 余裕

        beginTest ("P2a: Resting→Active");
        {
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, toneAmp);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (false, semis, formant);
            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, steadyLen, [] (int) { return false; });

            const int transitionStart = pos;
            shifter.setTarget (true, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");

            const int transitionEnd = pos + margin;
            const int finalTarget = transitionEnd + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            expect (checkTransitionNoClick (out, transitionStart, transitionEnd, steadyLen), "click detected: Resting->Active");
        }

        beginTest ("P2b: Active→Resting");
        {
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, toneAmp);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (true, semis, formant);
            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");

            int held = 0;
            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { held += maxBlock; return held >= steadyLen; });

            const int transitionStart = pos;
            shifter.setTarget (false, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Resting; });
            expect (shifter.getState() == vc::PitchShifter::State::Resting, "did not return to Resting");

            const int transitionEnd = pos + margin;
            const int finalTarget = transitionEnd + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            expect (checkTransitionNoClick (out, transitionStart, transitionEnd, steadyLen), "click detected: Active->Resting");
        }

        beginTest ("P2c: Priming中の中止");
        {
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, toneAmp);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (false, semis, formant);
            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, steadyLen, [] (int) { return false; });

            const int transitionStart = pos;
            shifter.setTarget (true, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Priming; });
            expect (shifter.getState() == vc::PitchShifter::State::Priming, "did not enter Priming");

            shifter.setTarget (false, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Resting; });
            expect (shifter.getState() == vc::PitchShifter::State::Resting, "did not return to Resting");

            const int transitionEnd = pos + margin;
            const int finalTarget = transitionEnd + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            expect (checkTransitionNoClick (out, transitionStart, transitionEnd, steadyLen), "click detected: Priming abort");
        }

        beginTest ("P2d: FadingIn中の反転");
        {
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, toneAmp);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (false, semis, formant);
            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, steadyLen, [] (int) { return false; });

            const int transitionStart = pos;
            shifter.setTarget (true, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::FadingIn; });
            expect (shifter.getState() == vc::PitchShifter::State::FadingIn, "did not enter FadingIn");

            // ランプの半分程度まで進める。
            int held = 0;
            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int)
                {
                    held += maxBlock;
                    return held >= 480 || shifter.getState() != vc::PitchShifter::State::FadingIn;
                });

            shifter.setTarget (false, semis, formant); // 反転 → FadingOut

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Resting; });
            expect (shifter.getState() == vc::PitchShifter::State::Resting, "did not return to Resting");

            const int transitionEnd = pos + margin;
            const int finalTarget = transitionEnd + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            expect (checkTransitionNoClick (out, transitionStart, transitionEnd, steadyLen), "click detected: FadingIn reversal");
        }

        beginTest ("P2e: FadingOut中の反転");
        {
            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, toneAmp);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (true, semis, formant);
            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");

            int held = 0;
            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { held += maxBlock; return held >= steadyLen; });

            const int transitionStart = pos;
            shifter.setTarget (false, semis, formant);

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::FadingOut; });
            expect (shifter.getState() == vc::PitchShifter::State::FadingOut, "did not enter FadingOut");

            int heldOut = 0;
            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int)
                {
                    heldOut += maxBlock;
                    return heldOut >= 480 || shifter.getState() != vc::PitchShifter::State::FadingOut;
                });

            shifter.setTarget (true, semis, formant); // 反転 → FadingIn

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not return to Active");

            const int transitionEnd = pos + margin;
            const int finalTarget = transitionEnd + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            expect (checkTransitionNoClick (out, transitionStart, transitionEnd, steadyLen), "click detected: FadingOut reversal");
        }
    }

    // ----- P3: 移調精度 -----
    void runP3()
    {
        constexpr double fs = 48000.0;
        constexpr int maxBlock = 480;
        constexpr int stretchBlock = 960;
        constexpr double toneFreq = 220.0;
        constexpr int steadyLen = (int) fs; // 1秒
        constexpr int guard = 4000;

        const float semitoneSet[] = { 12.0f, -12.0f, 5.0f, -5.0f, 8.0f, -6.0f };

        for (const float semis : semitoneSet)
        {
            beginTest ("P3: 移調精度 " + juce::String (semis) + "半音");

            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            const int totalLen = (int) fs * 2 + guard + steadyLen;
            auto signal = vc::test::makeSine (toneFreq, fs, totalLen, 0.3f);
            vc::test::addNoiseFloor (signal);
            std::vector<float> out (signal.size(), 0.0f);

            shifter.setTarget (true, semis, 1.0f);

            int pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, 0, (int) signal.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");

            const int finalTarget = pos + guard + steadyLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, signal.data(), out.data(), maxBlock, pos, (int) signal.size(),
                [&] (int p) { return p >= finalTarget; });

            const double measuredHz = vc::test::findFftPeakHz (out.data() + (finalTarget - steadyLen), steadyLen, fs);
            const double expectedHz = toneFreq * std::pow (2.0, (double) semis / 12.0);
            const double relError = std::abs (measuredHz - expectedHz) / expectedHz;

            expect (relError <= 0.01,
                    "measured " + juce::String (measuredHz) + "Hz, expected " + juce::String (expectedHz) + "Hz");
        }
    }

    // ----- P4: 層1ピッチでフォルマント保持 -----
    void runP4()
    {
        constexpr double fs = 48000.0;
        constexpr int maxBlock = 480;
        constexpr int stretchBlock = 960;
        constexpr double f0 = 100.0;
        constexpr int analysisLen = (int) (fs * 1.5); // 1.5秒（1秒以上）
        constexpr int guard = 4000;

        const float semitoneSet[] = { 5.0f, -5.0f };

        // 入力側の包絡・f0は移調によらず共通(定常な合成音のため、どの区間を切り出しても同じ)。
        auto rawVowel = vc::test::makeSyntheticVowel (f0, fs, analysisLen, { 730.0, 1090.0, 2440.0 }, { 80.0, 90.0, 120.0 }, 0.3f);
        vc::test::addNoiseFloor (rawVowel, -80.0f, 111);
        const double f0In = vc::test::measureFundamentalHz (rawVowel.data(), (int) rawVowel.size(), fs, f0);
        const auto envIn = vc::test::measureFormantEnvelope (rawVowel.data(), (int) rawVowel.size(), fs, f0In);

        for (const float semis : semitoneSet)
        {
            beginTest ("P4: フォルマント保持 " + juce::String (semis) + "半音");

            vc::PitchShifter shifter;
            shifter.prepare (fs, maxBlock, stretchBlock);

            const int totalLen = 20000 + analysisLen * 2;
            auto vowel = vc::test::makeSyntheticVowel (f0, fs, totalLen, { 730.0, 1090.0, 2440.0 }, { 80.0, 90.0, 120.0 }, 0.3f);
            vc::test::addNoiseFloor (vowel, -80.0f, 222);
            std::vector<float> out (vowel.size(), 0.0f);

            shifter.setTarget (true, semis, 1.0f); // フォルマント係数1.0 = 保持

            int pos = feedUntil (shifter, vowel.data(), out.data(), maxBlock, 0, (int) vowel.size(),
                [&] (int) { return shifter.getState() == vc::PitchShifter::State::Active; });
            expect (shifter.getState() == vc::PitchShifter::State::Active, "did not reach Active");

            const int finalTarget = pos + guard + analysisLen;
            expect (finalTarget <= (int) out.size(), "buffer too short");

            pos = feedUntil (shifter, vowel.data(), out.data(), maxBlock, pos, (int) vowel.size(),
                [&] (int p) { return p >= finalTarget; });

            const float* steadyOut = out.data() + (finalTarget - analysisLen);

            const double expectedF0 = f0In * std::pow (2.0, (double) semis / 12.0);
            const double f0Out = vc::test::measureFundamentalHz (steadyOut, analysisLen, fs, expectedF0);

            const double f0Ratio = f0Out / f0In;
            const double expectedRatio = std::pow (2.0, (double) semis / 12.0);
            expect (std::abs (f0Ratio - expectedRatio) / expectedRatio <= 0.01,
                    "f0 ratio " + juce::String (f0Ratio) + ", expected " + juce::String (expectedRatio));

            const auto envOut = vc::test::measureFormantEnvelope (steadyOut, analysisLen, fs, f0Out);
            const double s = vc::test::measureEnvelopeScale (envIn, envOut);

            expect (std::abs (s - 1.0) <= 0.1, "envelope scale s=" + juce::String (s) + ", expected ~1.0");
        }
    }

    // ----- P5: ブロック長 -----
    void runP5()
    {
        beginTest ("P5: ブロック長の決定");
        {
            const auto a = vc::PitchShifter::decideStretchBlock (10.0, 12.0);
            expect (a.blockMs == 26, "expected 26ms, got " + juce::String (a.blockMs));
            expect (! a.overBudget);

            const auto b = vc::PitchShifter::decideStretchBlock (30.0, 15.0);
            expect (b.blockMs == 20, "expected 20ms, got " + juce::String (b.blockMs));
            expect (b.overBudget);

            const auto c = vc::PitchShifter::decideStretchBlock (0.0, 2.0);
            expect (c.blockMs == 40, "expected 40ms, got " + juce::String (c.blockMs));
            expect (! c.overBudget);
        }
    }
};

static ShifterTests shifterTests;

} // namespace
