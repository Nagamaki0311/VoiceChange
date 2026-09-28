#pragma once

#include <juce_core/juce_core.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include <vector>

// ===== SECTION: PitchShifter =====
// Signalsmith Stretchラッパ + 休止⇔稼働の状態遷移。
// docs/spec.md「音声処理チェーン」「ピッチシフターの休止」「ブロック長の決定」、
// docs/decisions.md D-002・D-014、docs/plan.md 2.5節「PitchShifter」参照。
//
// D-014: ブロック長は120ms・インターバル30ms（`stretch.presetDefault(1, fs)`相当）に固定する。
// デバイス遅延から毎回計算する方式（D-003・decideStretchBlock）は廃止した。50msの遅延目標は
// シフター休止時の経路にのみ適用し、稼働中は約120ms + デバイス・リングバッファ分になる。
//
// 状態遷移（20msクロスフェード。gainは0=dry/1=wetの連続値で、途中で向きが反転しても
// 現在値から続けて動くため不連続にならない）:
//   Resting  --稼働要求-->  Priming（Stretchをreset()）
//   Priming  --休止要求-->  Resting
//   Priming  --送り込み量 >= inputLatency+outputLatency-->  FadingIn
//   FadingIn --休止要求-->  FadingOut（現在のgainから反転）
//   FadingIn --ランプ完了(gain>=1)-->  Active
//   Active   --休止要求-->  FadingOut
//   FadingOut--稼働要求-->  FadingIn（resetしない）
//   FadingOut--ランプ完了(gain<=0)-->  Resting（Stretchへの入力を止める）

namespace vc
{

class PitchShifter
{
public:
    enum class State
    {
        Resting,
        Priming,
        FadingIn,
        Active,
        FadingOut
    };

    // メッセージスレッドのみ。デバイス停止中に呼ぶ。presetDefault(1, fs)相当の設定 + ウォームアップ。
    void prepare (double sampleRate, int maxBlockSamples);

    // 音声スレッドのみ。稼働判定・移調量・フォルマント係数の目標値を設定する。
    void setTarget (bool run, float semitones, float formantFactor) noexcept;

    // 音声スレッドのみ。n <= prepare()時のmaxBlockSamples。in == out でもよい。
    void process (const float* in, float* out, int n) noexcept;

    // 音声スレッドのみ。Restingへ戻す（NaN検査・バイパス解除の準備で使う）。
    void reset() noexcept;

    // FadingIn / Active / FadingOutのとき inputLatency()+outputLatency()、それ以外0。
    int getLatencySamples() const noexcept;

    State getState() const noexcept { return state; }

private:
    void warmUp();

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    std::vector<float> wetScratch;

    State state = State::Resting;
    double gain = 0.0; // 0=dry, 1=wet。反転してもここから続けて動く。

    bool runTarget = false;
    float targetSemitones = 0.0f;
    float targetFormant = 1.0f;

    int fadeLenSamples = 1;
    int requiredPrimeSamples = 0;
    int primedSamples = 0;

    double sampleRate = 48000.0;
    int maxBlockSamples = 0;
};

} // namespace vc
