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
// 監視対象は3つ: (1)コールバック回数の停滞（入力・出力それぞれ。主軸）、(2)audioDeviceError・例外、
// (3)audioDeviceAboutToStartの2回目以降でレート/バッファ長が変化した場合。
// JUCE 9のWASAPIは、デバイス抜去・セッション失効(flagShutdown)やサンプルレート変更を検出すると
// handleAsyncUpdate() → close() → stop() → audioDeviceStopped() をメッセージスレッドで呼ぶ。こちらの
// stop()以外でaudioDeviceStopped()が呼ばれたらdeviceStoppedFlagを立て、抜去・レート変更の即時検出経路にする。
// 呼ばれないのは入力専用スレッドの1秒タイムアウトによる無言終了だけで、それはコールバック回数の停滞
// （入力・出力それぞれ2秒）で検出する（D-016）。

namespace vc
{

// 出力デバイス名にVB-CABLE（"CABLE Input"）を含むものがあるか（docs/spec.md「VB-CABLE検出」）。
// Main.cpp（起動時ダイアログ）とMainComponent（W1表示）の両方が使うため、ここに置く。
bool containsCableInput (const juce::StringArray& deviceNames) noexcept;

// docs/spec.md「初回起動時の既定値」。入力はシステム既定（一覧の先頭。docs/plan.md 1章の事実5）、
// 出力は「CABLE Input」を含むデバイス（なければシステム既定）。一覧が空なら空文字列。
// Main.cpp（起動時）とAudioIO（起動時にデバイスが0件だった場合の後追い）が使う。
juce::String pickDefaultInputName (const juce::StringArray& names);
juce::String pickDefaultOutputName (const juce::StringArray& names);

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
    double noiseMs = 0.0;   // ノイズ除去が稼働中のみ非0（D-025。50ms目標＝W2の判定には含めない）
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
    // 起動時・手動選択時に呼ぶ（新しい「世代」の開始。startに一度も成功していない世代の失敗はE4扱い。D-016）。
    // 失敗しても再接続の再試行は続く。名前が空の側は開けない（失敗側に数える）。
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

    // 直近のopen()で開けなかった側（成功していればNone）。E4の文言と赤枠の行の判定に使う（T-007）。
    enum class FailedSide { None, Input, Output, Both };
    FailedSide getFailedSide() const noexcept { return failedSide; }

    // 層1パラメータ・プリセット・ON/OFF（音声スレッドとはatomicのみでやり取りする、D-001）。
    AtomicParams& engineParams() noexcept { return engine.params(); }

    float takeInputPeak() noexcept { return engine.takeInputPeak(); }

    // リングバッファの統計（充填量・アンダーラン/オーバーラン・速度補正）。StatsLog用（T-007）。
    const ResamplingFifoStats& getFifoStats() const noexcept { return fifo.stats(); }

    // ===== T-007: 再接続・エラー表示 =====
    // 一度は動いていた(現在のopen()世代でstartまで成功した)のに異常状態になり、再open()に成功するまでの間。
    // design.md 6.1節「デバイス切断・再接続中」(E1〜E3)の判定に使う（MainComponent・トレイの両方）。
    // 一度もstartに成功していない世代の失敗は含まない（そちらはE4。getFailedSide()で判定する）。
    bool isReconnecting() const noexcept { return connectionMonitor.isFailed() && startedThisGeneration; }

    // 異常状態になってからの経過秒数（design.md 6.2節 E1〜E3の"{n}秒経過"）。異常でなければ0。
    double getReconnectElapsedSeconds() const noexcept;

    // open()に渡された最後のデバイス名（成功・失敗を問わない）。再接続・E1/E2の判定・コンボボックスの選択表示に使う。
    // 空のまま(起動時にデバイスが0件)なら、既定デバイスが現れた時点でAudioIOが採用して開く。
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

    // open()の本体。再接続の再試行(TryReopen)は世代を進めないためこちらを直接呼ぶ。
    bool openDevices (const juce::String& inName, const juce::String& outName);

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
    FailedSide failedSide = FailedSide::None;
    bool startedThisGeneration = false; // 現在のopen()世代でstartまで一度でも成功したか（メッセージスレッドのみ）

    // open()に渡された名前(成功・失敗を問わず記録する)。ConnectionMonitorがTryReopenを返したとき、
    // 同じ名前で開き直すために使う（D-009: 保存済みデバイスが無い場合も既定へ切り替えず再試行する）。
    juce::String desiredInputName, desiredOutputName;

    std::atomic<std::uint64_t> inputCallbackCount { 0 };
    std::atomic<std::uint64_t> outputCallbackCount { 0 };
    std::atomic<bool> errorFlag { false };          // audioDeviceErrorまたは例外
    std::atomic<bool> deviceStoppedFlag { false };  // こちらのstop()以外でaudioDeviceStoppedが呼ばれた（WASAPIの抜去・レート変更の即時検出。無言終了では立たない）
    std::atomic<bool> muteOutputFlag { false };     // レート/バッファ長の変化後、再オープンまで出力を無音にする
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
