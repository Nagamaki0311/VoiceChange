#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <vector>

// ===== SECTION: ResamplingFifo =====
// 入力スレッド(push)と出力スレッド(pull)のSPSCリングバッファ + クロックずれ補正リサンプラ。
// docs/spec.md「クロックずれ補正（リングバッファ）」、docs/decisions.md D-006・D-012、docs/plan.md 2.5節「ResamplingFifo」参照。
//
// D-012: 制御・判定に使う充填量は「入力を連続的に到着したとみなす連続換算値」fill_cを使う。
// fill_c = 生の充填量 − 直前の入力ブロック長 + fs_in×(現在時刻 − 直前のpush時刻)（0〜直前ブロック長でクランプ）。
// 生の充填量は常にfill_c以上なので、fill_cを目標に保てばアンダーランへの余裕は従来と同じかそれ以上になる。
// アンダーラン判定（needed不足）だけは生の充填量のまま行う。

namespace vc
{

// ResamplingFifoの統計値。UIスレッドから読むのでatomicのみ。
struct ResamplingFifoStats
{
    std::atomic<float> fillSmoothedSamples { 0.0f }; // 平滑化した充填量（入力レートのサンプル数）
    std::atomic<float> targetSamples { 0.0f };        // 目標充填量（入力レートのサンプル数）
    std::atomic<float> speedCorrectionPpm { 0.0f };   // 公称比からの速度補正量（ppm）
    std::atomic<float> jitterMarginMs { 2.0f };       // ジッタ余裕（ms）
    std::atomic<std::uint32_t> underruns { 0 };
    std::atomic<std::uint32_t> overruns { 0 };
    std::atomic<std::uint32_t> discards { 0 };
};

class ResamplingFifo
{
public:
    ResamplingFifo() = default;

    // メッセージスレッドのみ。デバイス停止中に呼ぶ。
    // inBlock/outBlockは入出力デバイスのバッファ長（サンプル数）。
    void prepare (double inRate, double outRate, int maxInBlock, int maxOutBlock, int inBlock, int outBlock);

    // 入力スレッドのみ。空きが足りない分は捨ててoverruns++。nowSecondsは単調時計の秒数
    // （AudioIOは`juce::Time::getMillisecondCounterHiRes()*1e-3`、テストは模擬イベント時刻）。
    void push (const float* mono, int n, double nowSeconds) noexcept;

    // 出力スレッドのみ。n <= prepare()時のmaxOutBlock。nowSecondsはpush()と同じ時計。
    void pull (float* out, int n, double nowSeconds) noexcept;

    const ResamplingFifoStats& stats() const noexcept { return fifoStats; }

    // 平滑化したfill_c + (Bi + Bo)/2（それぞれms換算して合算）。D-006・D-012。
    double getLatencyMs() const noexcept;

private:
    enum class Phase
    {
        Refilling,      // 目標充填量に達するまで無音を出す
        FadeIn,         // Refilling脱出直後の2msフェードイン
        Idle,           // 通常のリサンプリング
        UnderrunFadeOut,// 使われない(handleUnderrun内で完結する即時フェード) : 予約
        OverrunFadeOut, // 破棄前の2msフェードアウト
        OverrunFadeIn   // 破棄後の2msフェードイン
    };

    int neededInput (int outSamples) const noexcept;
    void recomputeTarget() noexcept;
    void updateFillStats (double fillCSamples, int outputSamplesElapsed) noexcept;
    double continuousFill (int rawFilled, double nowSeconds) const noexcept;
    void updateSpeedRatio() noexcept;
    void processNormal (float* out, int chunk) noexcept;
    void handleUnderrun (float* out, int offset, int remaining, int filled, double nowSeconds) noexcept;
    void applyRamp (float* out, int chunk, bool ascending) noexcept;

    juce::AbstractFifo fifo { 2 };
    std::vector<float> buffer;
    std::vector<float> scratch;
    juce::LagrangeInterpolator interp;

    double inRate = 48000.0, outRate = 48000.0;
    double nominalRatio = 1.0;   // inRate / outRate
    double speedRatio = 1.0;     // 現在の速度比（±0.1%補正込み）
    int inBlockSamples = 0, outBlockSamples = 0;

    double fillSmoothed = 0.0;   // 内部保持用（入力レートのサンプル数、double精度）
    double targetSamples = 0.0;  // 内部保持用
    double jitterMarginMs = 2.0;

    Phase phase = Phase::Refilling;
    int phaseCount = 0;   // フェード残りサンプル数（出力レート）
    double gain = 0.0;    // フェード中の現在ゲイン
    int fadeLenSamplesOut = 0; // 2ms相当（出力レート）

    // D-012: 直前のpush時刻とブロック長。push()が書き、pull()が読む(単一方向のクロス
    // スレッド読み取り)。release/acquireで対にする（blockLenを先にrelaxed書き、timeをrelease書き。
    // 読み手はtimeをacquire読みしてからblockLenを読めば、timeの新しい値が見えた時点でblockLenも
    // 新しい値が見える）。
    std::atomic<int> lastPushBlockLen { 0 };
    std::atomic<double> lastPushTime { 0.0 };

    ResamplingFifoStats fifoStats;
};

} // namespace vc
