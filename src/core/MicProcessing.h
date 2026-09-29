#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

// RNNoiseの不透明型（rnnoise.hのtypedef struct DenoiseStateと同じ型。ヘッダーにCのヘッダーを持ち込まない）。
struct DenoiseState;

// ===== SECTION: NoiseReducer =====
// ノイズ除去: フレーミング・48kHz変換・RNNoise・原音との混合・状態遷移・遅延報告。
// docs/spec.md「マイク処理: ノイズ除去」、docs/decisions.md D-019〜D-022・D-025、docs/plan.md 8.2節参照。
// 今回はゲート（VAD連動）とインパクト抑制を含まない（T-011）。EQはT-012。
//
// 状態遷移はPitchShifterと同じ表（20msのクロスフェード。gainは0=原音/1=処理後の連続値）。
// 同じ表の2か所目のため共通化しない（3か所目が出たら検討する。plan.md 8.2）:
//   Resting  --稼働要求-->  Priming（パイプラインを消去。以後、原音を出しながらRNNoiseへ送り込む）
//   Priming  --休止要求-->  Resting
//   Priming  --送り込み量 >= D-->  FadingIn（サンプル単位で切り替える。ブロック長に依存しない）
//   FadingIn --休止要求-->  FadingOut（現在のgainから反転）
//   FadingIn --ランプ完了(gain>=1)-->  Active
//   Active   --休止要求-->  FadingOut
//   FadingOut--稼働要求-->  FadingIn（消去しない）
//   FadingOut--ランプ完了(gain<=0)-->  Resting
//
// 遅延D（出力レートのサンプル数）= 出力FIFOの初期充填P + RNNoise固有の遅延（960サンプル@48kHz。T-009の実測）
// + 48kHz以外では変換の遅延（上げ側のLagrange 2サンプル + 下げ側のLagrange 2サンプル@48kHz）。
// 48kHzではP = 480（フレーム収集）なので D = 1440（30ms）。混合する原音もDサンプル遅らせる。

namespace vc
{

class NoiseReducer
{
public:
    // メッセージスレッドのみ。デバイス停止中に呼ぶ。初回はrnnoise_create、以後（デバイスの開き直し）はrnnoise_init。
    // FIFO・遅延線・補間器をここで確保する。音声スレッドでは確保しない。
    void prepare (double sampleRate, int maxBlockSamples);

    // 音声スレッドのみ。稼働要求・背景ノイズ（0〜1）・インパクト（0〜1）。各ブロックの先頭で呼ぶ。
    void setTarget (bool run, float background, float impact) noexcept;

    // 音声スレッドのみ。その場で処理する（n <= prepare()時のmaxBlockSamples）。Restingなら何もしない（bufに触れない）。
    // 入力に非有限値があればfalseを返す（bufは触れず、パイプラインも進めない）。呼び出し側がreset()する。
    bool process (float* buf, int n) noexcept;

    // 音声スレッドのみ。Restingへ戻す。rnnoise_init（確保なし）、FIFO・遅延線・補間器・平滑化を消去する。
    void reset() noexcept;

    // FadingIn / Active / FadingOutのときD、それ以外0（PitchShifterと同じ規則）。
    // UIスレッド（AudioIO::getLatency()経由）から読むため、音声スレッドがprocess()/reset()内でstoreしたatomicを返す。
    int getLatencySamples() const noexcept { return latencySamplesForUi.load (std::memory_order_relaxed); }

    // 現在の状態にかかわらず、prepare()で決まったD。テスト用。
    int getDelaySamples() const noexcept { return delaySamples; }

    // 出力FIFOが枯れて無音で埋めた回数（0でなければならない）。テスト用。
    int getUnderflowCount() const noexcept { return underflowCount; }

private:
    enum class State
    {
        Resting,
        Priming,
        FadingIn,
        Active,
        FadingOut
    };

    // 先頭から読み、末尾へ書く単純なFIFO。確保はallocate()のみ。あふれる分は古い方を捨てる（構成上は起きない）。
    class SampleQueue
    {
    public:
        void allocate (int capacity) { data.assign ((size_t) capacity, 0.0f); clear(); }
        void clear() noexcept { head = 0; tail = 0; }
        int size() const noexcept { return tail - head; }
        const float* readPtr() const noexcept { return data.data() + head; }

        void drop (int n) noexcept
        {
            head += n;

            if (head >= tail)
                clear();
        }

        // srcがnullなら無音を足す。
        void push (const float* src, int n) noexcept
        {
            const int capacity = (int) data.size();

            if (tail + n > capacity)
            {
                std::memmove (data.data(), data.data() + head, sizeof (float) * (size_t) (tail - head));
                tail -= head;
                head = 0;
            }

            if (tail + n > capacity)
            {
                jassertfalse; // 容量はprepare()で上限から決めているため起きない。起きたら新しい方を捨てる。
                n = capacity - tail;
            }

            if (n <= 0)
                return;

            if (src != nullptr)
                std::memcpy (data.data() + tail, src, sizeof (float) * (size_t) n);
            else
                std::memset (data.data() + tail, 0, sizeof (float) * (size_t) n);

            tail += n;
        }

    private:
        std::vector<float> data;
        int head = 0;
        int tail = 0;
    };

    struct RnnoiseDeleter
    {
        void operator() (DenoiseState* st) const noexcept;
    };

    void clearPipeline() noexcept;
    void feed (const float* in, int n) noexcept;
    void processFrame() noexcept;
    void pullMixed (int n) noexcept;
    void updateLatencyForUi() noexcept;

    std::unique_ptr<DenoiseState, RnnoiseDeleter> rnnoise;

    State state = State::Resting;
    double gain = 0.0; // 0=原音, 1=処理後。反転してもここから続けて動く。

    bool runTarget = false;
    float impactTarget = 0.0f; // ponytail: T-011のインパクト抑制で使う。それまでは保持するだけ。

    double sampleRate = 48000.0;
    int maxBlockSamples = 0;
    bool native = true;      // 出力レートが48kHz（変換なし）
    double upRatio = 1.0;    // 上げ側の速度比 = fs/48000
    double downRatio = 1.0;  // 下げ側の速度比 = 48000/fs
    int upNeed = 480;        // 1フレーム（480サンプル@48kHz）を作るのに必要な入力の上限
    int downNeed = 1;        // 下げ側で1サンプル出すのに必要な入力の上限
    int prefill = 480;       // 出力FIFOの初期充填P
    int delaySamples = 1440; // D
    int fadeLenSamples = 1;
    int primedSamples = 0;

    SampleQueue inQueue;   // 出力レートの入力
    SampleQueue outQueue;  // 出力レートの処理後の出力（初期充填P）
    std::vector<float> dryLine; // 原音の遅延線（Dサンプル）
    int dryPos = 0;
    std::vector<float> wetScratch;  // 1ブロック分の混合結果
    std::vector<float> frameIn;     // 48kHzの1フレーム（RNNoiseの入力。int16の値域）
    std::vector<float> frameOut;    // 48kHzの1フレーム（RNNoiseの出力）
    std::vector<float> downStage;   // 下げ側の入力（48kHz）。端数の持ち越しを含む
    std::vector<float> downOut;     // 下げ側の出力（出力レート）
    int downStaged = 0;

    juce::LagrangeInterpolator upInterp;
    juce::LagrangeInterpolator downInterp;
    juce::SmoothedValue<float> dSmoothed; // 原音の混合比d（50ms）

    int underflowCount = 0;

    // 音声スレッドのみが書く。UIスレッドはgetLatencySamples()でloadのみ。
    std::atomic<int> latencySamplesForUi { 0 };
};

} // namespace vc
