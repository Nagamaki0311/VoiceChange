#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

// ===== SECTION: PitchDetector =====
// YIN法のピッチ検出（ケロケロ・トークボックス共通）。docs/spec.md「ピッチ検出モジュール」、
// docs/plan.md 2.5節「PitchDetector」参照。
// 入力を約12kHzへ間引き（前段にIIRローパス）、検出範囲70〜1000Hz、積分窓約21ms、ホップ約5ms、
// 累積平均正規化差分関数の閾値0.15、放物線補間で小数周期を推定する。
// GUI・デバイスに依存しない（vc_core）。

namespace vc
{

class PitchDetector
{
public:
    // メッセージスレッドのみ。バッファ・IIR係数はすべてここで確保・生成する。
    void prepare (double sampleRate, int maxBlockSamples);

    // 音声スレッドのみ。履歴・フィルタ・出力値を初期化する（周波数は0=未検出に戻る）。
    void reset() noexcept;

    // 音声スレッドのみ。ホップ約5msごとに推定を更新する。アロケーションなし。
    void process (const float* in, int n) noexcept;

    // 無声のときは直前の有声時の値を保持する。一度も有声にならなければ0。
    float getFrequencyHz() const noexcept { return frequencyHz; }

    // 0..1。RMSが閾値未満なら0。閾値0.15を満たさない（無声）フレームは0.5未満になる。
    float getVoicing() const noexcept { return voicing; }

private:
    void estimate() noexcept;

    static constexpr int kNumBiquads = 3; // 6次Butterworthローパス
    std::array<juce::dsp::IIR::Filter<float>, kNumBiquads> antiAlias;

    int decimation = 4;
    int decimCounter = 0;

    int windowLen = 0;   // 積分窓 W
    int tauMin = 0;
    int tauMax = 0;
    int hopLen = 0;      // 間引き後のサンプル数
    int hopCounter = 0;
    double decimatedRate = 12000.0;

    std::vector<float> ring;   // 間引き後の履歴（2の累乗長）
    int ringMask = 0;
    int ringPos = 0;           // 次に書く位置
    int ringFilled = 0;

    std::vector<float> frame;  // 履歴を時間順に並べたもの（長さ W + tauMax + 1）
    std::vector<float> cmnd;   // 累積平均正規化差分関数（index = tau）

    float frequencyHz = 0.0f;
    float voicing = 0.0f;
};

} // namespace vc
