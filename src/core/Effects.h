#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

// ===== SECTION: Effects =====
// 層2のピッチ以外の効果（エコー / ロボット / トークボックス）。
// docs/spec.md「層2: 特殊効果プリセット」、docs/plan.md 2.5節「Effects」参照。
// バッファ・IIR係数・DelayLineの確保はすべてprepare()で行い、process()はアロケーションしない。
// GUI・デバイスに依存しない（vc_core）。

namespace vc
{

// ディレイ300ms・フィードバック45%・帰還路3.5kHzローパス・dry 1.0 + wet 0.6。
class Echo
{
public:
    void prepare (double sampleRate, int maxBlockSamples);

    // バッファを消去し、その後20msはディレイへの書き込みにフェードインをかける
    // （消去直後の最初のサンプルが300ms後に段差として出るのを防ぐ）。
    void reset() noexcept;

    // in == out でもよい。
    void process (const float* in, float* out, int n) noexcept;

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> delay { 1 };
    juce::dsp::IIR::Filter<float> feedbackLowPass;
    int fadeLenSamples = 1;
    int fadePos = 0; // fadeLenSamples以上でフェード完了
};

// 40Hz正弦波の乗算（リングモジュレーション）。
class RingModulator
{
public:
    void prepare (double sampleRate, int maxBlockSamples);
    void reset() noexcept { phase = 0.0; }
    void process (const float* in, float* out, int n) noexcept; // in == out でもよい

private:
    double phaseInc = 0.0;
    double phase = 0.0;
};

// チャンネルボコーダー。docs/spec.md「トークボックス（チャンネルボコーダー）」。
class Talkbox
{
public:
    static constexpr int kNumBands = 20;

    void prepare (double sampleRate, int maxBlockSamples);
    void reset() noexcept;

    // modulator（ピッチシフター後の声）の包絡を、carrierHzの鋸波/雑音キャリアへ掛けて出力する。
    // voicing 0..1でキャリアを雑音（0）から鋸波（1）へ混ぜる。modulator == out でもよい。
    void process (const float* modulator, float* out, int n, float carrierHz, float voicing) noexcept;

private:
    struct Band
    {
        std::array<juce::dsp::IIR::Filter<float>, 2> mod;
        std::array<juce::dsp::IIR::Filter<float>, 2> carrier;
        float envelope = 0.0f;
        float carrierEnvelope = 0.0f;
    };

    std::array<Band, kNumBands> bands;
    std::vector<float> carrierScratch;
    std::vector<float> modulatorScratch;

    juce::SmoothedValue<float> carrierFreq;
    juce::SmoothedValue<float> voicingSmoothed;
    juce::Random rng;

    double sampleRate = 48000.0;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    double phase = 0.0;
};

} // namespace vc
