#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include "core/ConnectionMonitor.h"
#include "core/Engine.h"
#include "core/ResamplingFifo.h"

#include <atomic>
#include <cstdint>
#include <memory>

// ===== SECTION: AudioIO =====
// 入力・出力デバイスをそれぞれ別のAudioIODeviceとして開き、独立したコールバックスレッドで動かす（D-001）。
// docs/spec.md「アーキテクチャ」「デバイスタイプ」「デバイス選択」、docs/plan.md 2.5節「AudioIO」
// 「ConnectionMonitor」、docs/decisions.md D-009参照。
// 500msの`juce::Timer`でConnectionMonitorを評価し、片方だけの異常でも両方をclose()、両方揃ってから
// 両方をopen()し直す（D-009。この対称性はConnectionMonitorではなくここで保証する）。
// 監視対象は3つ: (1)audioDeviceError、(2)無言終了（audioDeviceStoppedがこちらからのstop()以外で
// 呼ばれた場合）、(3)audioDeviceAboutToStartの2回目以降でレート/バッファ長が変化した場合。
// (1)(2)はerrorFlag/deviceStoppedFlagとして合成し、ConnectionMonitorのerrorFlag引数に渡す。

namespace vc
{

// 出力デバイス名にVB-CABLE（"CABLE Input"）を含むものがあるか（docs/spec.md「VB-CABLE検出」）。
// Main.cpp（起動時ダイアログ）とMainComponent（W1表示）の両方が使うため、ここに置く。
bool containsCableInput (const juce::StringArray& deviceNames) noexcept;

struct DeviceInfo
{
    juce::String name;
    bool lowLatency = false;
    double rate = 0.0;
    int bufferSize = 0;
    int latencySamples = 0;
};

struct LatencyBreakdown
{
    double deviceInMs = 0.0;
    double deviceOutMs = 0.0;
    double ringBufferMs = 0.0;
    double shifterMs = 0.0; // ピッチシフター稼働中のみ非0（休止中は0。D-002）
    double totalMs = 0.0;
};

class AudioIO final : private juce::Timer,
                       private juce::AudioIODeviceType::Listener
{
public:
    AudioIO();
    ~AudioIO() override;

    // メッセージスレッドのみ。scanForDevices()済みの型から得た名前を返す。
    juce::StringArray getInputNames() const;
    juce::StringArray getOutputNames() const;

    // メッセージスレッドのみ。両方停止 → 作成 → open → prepare → start。
    // 低遅延モードで開けなければ共有モードへフォールバックする（Windowsのみ。D-001）。
    bool open (const juce::String& inName, const juce::String& outName);
    void close();

    // 両方のデバイスが開いているか（UIの起動中表示・エラー表示の判定に使う。T-006）。
    bool isOpen() const noexcept { return inputDevice != nullptr && outputDevice != nullptr; }

    DeviceInfo getInputInfo() const noexcept { return inputInfo; }
    DeviceInfo getOutputInfo() const noexcept { return outputInfo; }

    LatencyBreakdown getLatency() const noexcept;

    // 出力コールバック内の処理時間 ÷ 担当音声長（平滑化、1コアあたりの割合）。
    float getCpuLoad() const noexcept { return cpuLoad.load (std::memory_order_relaxed); }

    juce::String getErrorText() const;

    // 層1パラメータ・プリセット・ON/OFF（音声スレッドとはatomicのみでやり取りする、D-001）。
    AtomicParams& engineParams() noexcept { return engine.params(); }

    float takeInputPeak() noexcept { return engine.takeInputPeak(); }

    // リングバッファの統計（充填量・アンダーラン/オーバーラン・速度補正）。StatsLog用（T-007）。
    const ResamplingFifoStats& getFifoStats() const noexcept { return fifo.stats(); }

    std::uint64_t getInputCallbackCount() const noexcept { return inputCallbackCount.load (std::memory_order_relaxed); }
    std::uint64_t getOutputCallbackCount() const noexcept { return outputCallbackCount.load (std::memory_order_relaxed); }
    bool hasErrorFlag() const noexcept { return errorFlag.load (std::memory_order_relaxed); }

    // ===== T-007: 再接続・エラー表示 =====
    // ConnectionMonitorが異常状態(CloseAndFail後、再open()に成功するまで)かどうか。design.md 6.1節
    // 「デバイス切断・再接続中」の判定に使う（MainComponent・トレイの両方）。
    bool isReconnecting() const noexcept { return connectionMonitor.isFailed(); }

    // 異常状態になってからの経過秒数（design.md 6.2節 E1〜E3の"{n}秒経過"）。異常でなければ0。
    double getReconnectElapsedSeconds() const noexcept;

    // open()に渡された最後のデバイス名（成功・失敗を問わない）。再接続とE1/E2の判定に使う。
    juce::String getDesiredInputName() const noexcept { return desiredInputName; }
    juce::String getDesiredOutputName() const noexcept { return desiredOutputName; }

    // Engineの非有限値・例外フラグ(bit0/bit1)を検出後10秒間trueを返す(design.md 6.1節 E5)。
    // 元のフラグは本クラスの500msタイマーが読み取ってすぐ消費するため(複数箇所からの重複クリアを
    // 避けるため唯一の消費者にする)、MainComponent・トレイはこちらを読むだけにする。
    bool hasRecentEngineError() const noexcept;
    juce::String getLastEngineErrorTimeText() const noexcept; // "HH:MM:SS"。E5表示用

private:
    class InputCallback;
    class OutputCallback;

    void createDeviceTypes();
    juce::AudioIODeviceType* nameListType() const;

    // 低遅延→共有の順で開く。開いたデバイスと実際に使ったモードを返す(nullptrなら失敗)。
    std::unique_ptr<juce::AudioIODevice> openOneDevice (const juce::String& outName,
                                                         const juce::String& inName,
                                                         bool wantInput,
                                                         DeviceInfo& infoOut);

    // juce::Timer（500ms）。ConnectionMonitorの評価とEngineエラーフラグの消費を行う。
    void timerCallback() override;
    // juce::AudioIODeviceType::Listener。デバイス一覧の変更を検出する(スレッドは不定のためatomicで受ける)。
    void audioDeviceListChanged() override { listChangedFlag.store (true, std::memory_order_relaxed); }

    void evaluateConnection (bool listChangedThisTick);

    juce::OwnedArray<juce::AudioIODeviceType> deviceTypesLowLatency; // Windows: sharedLowLatency。Linux: 既定の型1つ。
    juce::OwnedArray<juce::AudioIODeviceType> deviceTypesShared;     // Windows: shared。Linuxでは未使用。

    std::unique_ptr<juce::AudioIODevice> inputDevice;
    std::unique_ptr<juce::AudioIODevice> outputDevice;
    std::unique_ptr<InputCallback> inputCallback;
    std::unique_ptr<OutputCallback> outputCallback;

    ResamplingFifo fifo;
    Engine engine;

    DeviceInfo inputInfo, outputInfo;
    juce::String errorText;

    // open()に渡された名前(成功・失敗を問わず記録する)。ConnectionMonitorがTryReopenを返したとき、
    // 同じ名前で開き直すために使う（D-009: 保存済みデバイスが無い場合も既定へ切り替えず再試行する）。
    juce::String desiredInputName, desiredOutputName;

    std::atomic<std::uint64_t> inputCallbackCount { 0 };
    std::atomic<std::uint64_t> outputCallbackCount { 0 };
    std::atomic<bool> errorFlag { false };          // audioDeviceErrorまたは例外
    std::atomic<bool> deviceStoppedFlag { false };  // 無言終了（こちらからのstop()以外でaudioDeviceStoppedが呼ばれた）
    std::atomic<bool> reopenRequestedFlag { false }; // audioDeviceAboutToStartの2回目以降でレート/バッファ長が変化
    std::atomic<bool> expectingIntentionalStop { false }; // close()内でstop()を呼ぶ間だけtrue
    std::atomic<bool> listChangedFlag { false };
    std::atomic<float> cpuLoad { 0.0f };

    ConnectionMonitor connectionMonitor;
    juce::int64 reconnectStartMs = 0; // CloseAndFailに遷移した時刻(タイマー・isReconnecting()と同じくメッセージスレッドのみ)

    // Engineのエラーフラグ(bit0/bit1)を本クラスのタイマーだけが消費する(唯一の消費者にする理由は上記)。
    juce::int64 lastEngineErrorMs = 0;
    juce::Time lastEngineErrorWallClock;

    std::vector<float> monoScratch;      // 出力コールバックの一時バッファ(pull用)。open()内でのみ確保する。
    std::vector<float> inputMonoScratch; // 入力コールバックの一時バッファ(モノラル化用)。open()内でのみ確保する。
};

} // namespace vc
