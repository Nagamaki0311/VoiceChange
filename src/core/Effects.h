#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "Params.h"

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

// トークボックスのキャリアの音程を決める固定フレーズ（D-028）。声の高さには一切触れない。
// 8音のリフ G2 G2 Bb2 C3 D3 C3 Bb2 F2、123 BPM・8分音符・階段（グライドなし）で、ステップを巡回して自走する。
// k番目のステップの始まりは、ステップ長（60/123 × 0.5 秒 = 小数のサンプル数）のk倍を切り上げたサンプル（整数の計算なので、小数部の丸めでテンポがずれない）。
// 確保なし。reset()で先頭の音（1つめのG2）から始め直す。
class PhraseSequencer
{
public:
    static constexpr int kNumSteps = 8;
    static constexpr std::array<int, kNumSteps> kMidiNotes { 43, 43, 46, 48, 50, 48, 46, 41 }; // G2 G2 Bb2 C3 D3 C3 Bb2 F2（C4 = 60、A4 = 440Hz = 69）
    static constexpr int kBpm = 123;
    static constexpr int kStepsPerBeat = 2; // 8分音符

    void prepare (double sampleRate);

    // 先頭のステップから始める（時刻0）。
    void reset() noexcept;

    // 1サンプル進めて、そのサンプルのキャリア周波数（Hz。音域・層1ピッチを掛ける前）を返す。
    float next() noexcept;

    // k番目のステップが始まるサンプル（時刻0からの通し番号。ceil(k × 60 × fs / (kBpm × kStepsPerBeat))）。テスト用。
    long long boundaryOf (long long step) const noexcept;

    // 現在のステップ番号（0 = 先頭からの通し番号。音は kMidiNotes[番号 % kNumSteps]）。テスト用。
    long long getStepIndex() const noexcept { return stepIndex; }

    static float midiToHz (int midiNote) noexcept;

private:
    long long sampleRateInt = 48000;
    long long nextBoundary = 1;
    long long clock = 0;
    long long stepIndex = 0;
    float hz = 0.0f;
};

// チャンネルボコーダー。docs/spec.md「トークボックス（チャンネルボコーダー）」。
// キャリアは、PhraseSequencerの現在の音 × デチューン2本（±10セント）の鋸波の和と白色雑音を有声度で混ぜたもの。
class Talkbox
{
public:
    static constexpr int kNumBands = 20;

    // 出力音量を入力と揃える固定の補正ゲイン（音域ごと。X3で母音3種×f0 3種の中央値から決めた。docs/spec.mdに記録）。添字はTalkboxRange。
    static constexpr std::array<float, 2> kOutputGain { 0.92f, 1.06f };

    void prepare (double sampleRate, int maxBlockSamples);

    // バンドの状態・シーケンサー・鋸波の位相を初期化し、フレーズを先頭の音から始め直す。
    void reset() noexcept;

    // modulator（ピッチシフター後の声）の包絡を、固定フレーズ・キャリアへ掛けて出力する。modulator == out でもよい。
    // pitchRatio = 2^(層1ピッチ/12)（キャリアにもかかる）、rangeがHighならさらに1オクターブ上。
    // voicing 0..1（検出器の値）は、キャリアを雑音（0）から鋸波（1）へ混ぜる。下限0.8を適用して、低域・中域は鋸波を主にする。
    // 2.5kHz以上のバンドは、下限を適用した有声度から検出器の値そのままへ子音ぶん近づけ、無声の摩擦音を雑音で鳴らす。
    void process (const float* modulator, float* out, int n, float pitchRatio, TalkboxRange range, float voicing) noexcept;

private:
    struct Band
    {
        std::array<juce::dsp::IIR::Filter<float>, 2> mod;
        std::array<juce::dsp::IIR::Filter<float>, 2> carrier;
        float envelope = 0.0f;
        float carrierEnvelope = 0.0f;
        bool useHighCarrier = false; // 子音用のキャリア（2.5kHz以上のバンド）
    };

    std::array<Band, kNumBands> bands;
    std::vector<float> carrierLowScratch;
    std::vector<float> carrierHighScratch;
    std::vector<float> modulatorScratch;

    PhraseSequencer sequencer;
    juce::SmoothedValue<float> voicingLow;
    juce::SmoothedValue<float> voicingHigh;
    juce::Random rng { 0x5eed }; // 固定シード（テストの再現性のため）

    double sampleRate = 48000.0;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    std::array<double, 2> phase { 0.0, 0.0 }; // デチューン2本の鋸波の位相
    std::array<float, 2> detuneRatio { 1.0f, 1.0f }; // デチューンの周波数比（prepareで計算）
};

} // namespace vc
