#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "Params.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

// RNNoiseの不透明型（rnnoise.hのtypedef struct DenoiseStateと同じ型。ヘッダーにCのヘッダーを持ち込まない）。
struct DenoiseState;

// ===== SECTION: NoiseReducer =====
// ノイズ除去: フレーミング・48kHz変換・RNNoise・原音との混合・状態遷移・遅延報告。
// docs/spec.md「マイク処理: ノイズ除去」、docs/decisions.md D-019〜D-022・D-025、docs/plan.md 8.2節参照。
// 混合の後に、VAD連動の広帯域ゲート（D-022）とインパクト抑制（D-023）を掛ける（T-011）。EQはEqualizer（同ファイル末尾）。
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
//
// 出力レートの1サンプルごとの処理（process()の1サンプル分。ブロック長に依存しない）:
//   RNNoise出力（Dサンプル遅れ）と原音（同じDだけ遅らせる）を混合 → VadGate → ImpactSuppressor
// VADはRNNoiseの戻り値で、そのフレームの出力と同じ呼び出しで得られる。出力FIFOと同じ長さのVAD用FIFOに
// 「フレームのVADを、そのフレームが出力FIFOへ足したサンプル数だけ」並べて、出力と同じ位置で読む
// （実測: 立ち上がりでは出力より約1フレーム早く上がり、立ち下がりでは出力と同じ位置で下がる）。

namespace vc
{

class NoiseReducer
{
public:
    // メッセージスレッドのみ。デバイス停止中に呼ぶ。初回はrnnoise_create、以後（デバイスの開き直し）はrnnoise_init。
    // FIFO・遅延線・補間器をここで確保する。音声スレッドでは確保しない。
    void prepare (double sampleRate, int maxBlockSamples);

    // 音声スレッドのみ。稼働要求・背景ノイズ（0〜1）・インパクト（0〜1）。各ブロックの先頭で呼ぶ。
    void setTarget (bool run, float background, float impactAmount) noexcept;

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

    // ゲートが閉じている状態で処理したサンプルの累計（reset・パイプライン消去で0に戻る）。テスト用（N3b）。
    int getGateClosedSampleCount() const noexcept { return gate.closedSamples; }

    // インパクト抑制が減衰していた（ゲイン < 1）サンプルの累計（同上）。テスト用。
    int getImpactAttenuatedSampleCount() const noexcept { return impact.attenuatedSamples; }

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
        void drop (int n) noexcept
        {
            head += n;

            if (head >= tail)
                clear();
        }

        // srcがnullなら無音を足す。
        void push (const float* src, int n) noexcept
        {
            n = makeRoom (n);

            if (n <= 0)
                return;

            if (src != nullptr)
                std::memcpy (data.data() + tail, src, sizeof (float) * (size_t) n);
            else
                std::memset (data.data() + tail, 0, sizeof (float) * (size_t) n);

            tail += n;
        }

        // 同じ値をn個足す（VAD用）。
        void fill (float value, int n) noexcept
        {
            n = makeRoom (n);
            std::fill (data.begin() + tail, data.begin() + tail + std::max (n, 0), value);
            tail += std::max (n, 0);
        }

        const float* readPtr() const noexcept { return data.data() + head; }

    private:
        // 末尾にn個入る余地を作り、実際に入れられる個数を返す。
        int makeRoom (int n) noexcept
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

            return n;
        }

        std::vector<float> data;
        int head = 0;
        int tail = 0;
    };

    struct RnnoiseDeleter
    {
        void operator() (DenoiseState* st) const noexcept;
    };

    // VAD連動の広帯域ゲート（spec.md「ノイズ除去」5、D-022）。出力と同じタイミングの原音とVADを1サンプルずつ受け取り、ゲインを返す。
    // 開く: VAD >= 0.6、または原音の短時間パワー（10ms）が雑音床より12dB以上大きい。
    // 閉じる: VAD < 0.3 かつ 短時間パワーが雑音床+8dB未満、の状態が200ms（ホールド）続いたとき（当初の+6dBから調整。spec.md参照）。
    // ゲインは開くとき5ms・閉じるとき150msで動く（線形。閉じたときの下限は呼び出し側が毎サンプル渡すclosedGain）。
    // 雑音床は短時間パワーの2秒間の最小値で、上昇は3dB/秒まで。すべてサンプル数基準（ブロック長に依存しない）。
    struct VadGate
    {
        void prepare (double fs);       // 確保はここだけ
        void reset() noexcept;
        // valid: 遅延線が埋まって以降（false の間は開いたまま何も更新しない）。closedGain: 閉じたときの下限ゲイン（1 = ゲートなし）。
        float process (float dry, float vad, float closedGain, bool valid) noexcept;

        bool open = true;
        int closedSamples = 0; // テスト用の累計

    private:
        static constexpr int kSegments = 20; // 2秒 = 100ms × 20

        std::vector<double> squares;         // 直近10msの原音の2乗
        std::array<double, kSegments> segmentMins {};
        double sumSquares = 0.0;
        double segmentMin = 0.0;
        double minOfSegments = 0.0;
        double floorPower = 0.0;
        double floorRise = 1.0;              // 1サンプルあたりの雑音床の上昇率（3dB/秒）
        float gateGain = 1.0f;
        float openStep = 1.0f, closeStep = 1.0f;
        int window = 1, windowPos = 0, windowFill = 0;
        int segmentLength = 1, segmentCount = 0, segmentIndex = 0;
        int holdSamples = 1, holdCount = 0;
        bool floorValid = false;
    };

    // インパクト抑制（spec.md「ノイズ除去」6、D-023）。原音を出力より先読みした位置で受け取り、出力の信号に掛けるゲインを返す。
    // 検出: 速い包絡（アタック0.1ms・リリース2ms）と遅い包絡（アタック20ms・リリース200ms）の比が閾値 T = 30 - 18・s_i [dB] を超える。
    // ゲートが開いている間は閾値を6dB上げ、減衰量を半分（dB）にする。減衰量 = 24・s_i [dB]（50msで補間）。
    // 減衰: アタック1ms・ホールド3ms（再検出で延長。当初の15msから調整。spec.md参照）・リリース10ms（線形）。s_i = 0 では検出も減衰もしない。
    struct ImpactSuppressor
    {
        void prepare (double fs);
        void reset() noexcept;
        void setTarget (float impact, bool immediate) noexcept;
        // valid: 先読み位置に有効な原音が来て以降。gateOpen: 検出した時点のゲートの状態。戻り値はゲイン（1 = 減衰なし）。
        float process (float tap, bool gateOpen, bool valid) noexcept;

        int attenuatedSamples = 0; // テスト用の累計

    private:
        juce::SmoothedValue<float> depthGain; // 減衰量24・s_i[dB]のゲイン（1〜約0.063）。50ms
        float thresholdClosed = 1.0f, thresholdOpen = 1.0f; // 速い包絡/遅い包絡 の比の閾値（リニア）
        float fastAttack = 0.0f, fastRelease = 0.0f, slowAttack = 0.0f, slowRelease = 0.0f;
        float attackStep = 1.0f, releaseStep = 1.0f;
        float fast = 0.0f, slow = 0.0f;
        float latched = 1.0f;   // 検出した時点で決めた減衰ゲイン（ホールド中はこれを目標にする）
        float envelope = 1.0f;  // 現在のゲイン
        int holdSamples = 1, holdCount = 0;
        bool active = false;
        bool primed = false;
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
    int lookaheadSamples = 96; // インパクト抑制の先読み（2ms）。原音の遅延線のD - 先読みの位置を検出に使う
    int pipelineSamples = 0;   // パイプラインの消去からのサンプル数（Dで頭打ち）。遅延線に有効な原音が届いたかの判定用

    SampleQueue inQueue;   // 出力レートの入力
    SampleQueue outQueue;  // 出力レートの処理後の出力（初期充填P）
    SampleQueue vadQueue;  // outQueueと同じ位置のVAD（フレームのVADを、そのフレームが足したサンプル数だけ並べる）
    float frameVad = 0.0f; // 直近のフレームのVAD
    std::vector<float> dryLine; // 原音の遅延線（Dサンプル）
    int dryPos = 0;
    std::vector<float> wetScratch;  // 1ブロック分の混合結果（ゲート・インパクト抑制の後）
    std::vector<float> vadScratch;  // 1ブロック分のVAD
    std::vector<float> frameIn;     // 48kHzの1フレーム（RNNoiseの入力。int16の値域）
    std::vector<float> frameOut;    // 48kHzの1フレーム（RNNoiseの出力）
    std::vector<float> downStage;   // 下げ側の入力（48kHz）。端数の持ち越しを含む
    std::vector<float> downOut;     // 下げ側の出力（出力レート）
    int downStaged = 0;

    juce::LagrangeInterpolator upInterp;
    juce::LagrangeInterpolator downInterp;
    juce::SmoothedValue<float> dSmoothed; // 原音の混合比d（50ms）
    juce::SmoothedValue<float> closedGainSmoothed; // ゲートが閉じたときのゲイン（1 = ゲートなし。50ms）
    VadGate gate;
    ImpactSuppressor impact;

    int underflowCount = 0;

    // 音声スレッドのみが書く。UIスレッドはgetLatencySamples()でloadのみ。
    std::atomic<int> latencySamplesForUi { 0 };
};

// ===== SECTION: Equalizer =====
// 5バンドのパラメトリックEQ（T-012）。docs/spec.md「マイク処理: EQ」、D-024、docs/plan.md 8.2節参照。
// juce::dsp::IIR::Filter（2次）を5段直列に並べる。係数はIIR::ArrayCoefficients（std::arrayを返す）で計算し、prepare()で
// 作成済みのCoefficientsへ代入する（Array::clearQuick + ensureStorageAllocatedで、容量8があれば再確保しない。JUCE 9.0.2で確認）。
//
// 時間の基準はすべてサンプル数（ブロックの区切りに依存しない）。処理は32サンプルのグループ単位で進め、グループの先頭で
//   (1) クロスフェードが終わって素通しになったバンドのタイプ切替、(2) 補間中の係数の再計算（補間値を32サンプル進める）
// を行う。ブロックの境界がグループの途中に来ても、グループの先頭の位置は変わらない。
// ただし目標値（setTarget）の反映だけはブロックの先頭（呼び出し側の粒度）で、NoiseReducerと同じ。
//
// 切り替え（すべて20msのクロスフェード。mix = 0で素通し、1で処理後）:
//   EQ全体のON/OFF: 全体のgain（Resting → FadingIn → Active → FadingOut → Resting）。Restingでは何もしない（ビット一致）。
//   バンドの有効/無効: そのバンドのmix。
//   タイプの変更: mixを0へ下げる → （素通しになったグループの先頭で）タイプ・係数を切り替え、フィルタ状態をリセット → mixを1へ戻す。
// 周波数・ゲイン・Qは50msの補間（周波数・Qは乗算的、ゲインはdB）。周波数の実効上限は0.45×出力レート。
class Equalizer
{
public:
    // メッセージスレッドのみ。デバイス停止中に呼ぶ。Coefficientsの作成（確保）はここだけ。
    void prepare (double sampleRate);

    // 音声スレッドのみ。各ブロックの先頭で呼ぶ。範囲外の値は端へ丸め、非有限値は初期値（kEqDefaults）にする。
    void setTarget (bool run, const std::array<EqBandSettings, kEqBands>& settings) noexcept;

    // 音声スレッドのみ。その場で処理する。OFFのフェード完了後（Resting）は何もしない（bufに触れない）。
    void process (float* buf, int n) noexcept;

    // 音声スレッドのみ。Restingへ戻し、フィルタ状態を消去する（確保なし）。
    void reset() noexcept;

    // EQがRestingでない（処理中・フェード中）か。テスト用。
    bool isRunning() const noexcept { return state != State::Resting; }

private:
    enum class State
    {
        Resting,
        FadingIn,
        Active,
        FadingOut
    };

    static constexpr int kGroup = 32; // 係数の再計算周期（サンプル）

    struct Band
    {
        juce::dsp::IIR::Filter<float> filter;
        EqBandSettings target {};  // 丸め済みの目標値（周波数はfsで頭打ち済み）
        EqType type = EqType::Peak; // 係数に使っているタイプ（targetのタイプへは、素通しの間に切り替える）
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> hz { 1000.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> q { 0.71f };
        juce::SmoothedValue<float> gainDb { 0.0f };
        double mix = 0.0;          // 0=素通し, 1=処理後
        bool idle = true;          // フィルタを動かしていない（mix = 0で、目標も素通し）
    };

    void start() noexcept;                    // Restingから稼働へ。全バンドを目標へ即時に合わせ、フィルタを消去する
    void beginGroup() noexcept;               // 32サンプルのグループの先頭の処理
    void updateCoefficients (Band& b) noexcept;
    void processBand (Band& b, float* seg, int len) noexcept;
    void mixGlobal (float* seg, const float* dry, int len) noexcept;

    State state = State::Resting;
    double gain = 0.0; // 0=素通し, 1=EQ。反転しても現在値から続けて動く。
    bool runTarget = false;

    double sampleRate = 48000.0;
    float maxHz = 21600.0f; // 0.45 × fs
    double fadeStep = 1.0;  // 20msのクロスフェードの1サンプルあたりの増分
    int phase = 0;          // グループ内の位置（0〜kGroup-1）
    bool coefficientsStale = true;

    std::array<Band, kEqBands> bands;
};

} // namespace vc
