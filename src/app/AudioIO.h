#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/Engine.h"
#include "core/ResamplingFifo.h"

#include <atomic>
#include <cstdint>
#include <memory>

// ===== SECTION: AudioIO =====
// 入力・出力デバイスをそれぞれ別のAudioIODeviceとして開き、独立したコールバックスレッドで動かす（D-001）。
// docs/spec.md「アーキテクチャ」「デバイスタイプ」、docs/plan.md 2.5節「AudioIO」参照。
// ウォッチドッグ・再接続（ConnectionMonitor）はT-007。ここではコールバック回数のatomicカウンタと
// エラーフラグだけを置く。出力コールバックはpullした音をEngineに通してから全チャンネルへ複製する。

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

class AudioIO
{
public:
    AudioIO();
    ~AudioIO();

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

    std::uint32_t getEngineErrorFlags() const noexcept { return engine.getErrorFlags(); }
    void clearEngineErrorFlags() noexcept { engine.clearErrorFlags(); }
    float takeInputPeak() noexcept { return engine.takeInputPeak(); }

    std::uint64_t getInputCallbackCount() const noexcept { return inputCallbackCount.load (std::memory_order_relaxed); }
    std::uint64_t getOutputCallbackCount() const noexcept { return outputCallbackCount.load (std::memory_order_relaxed); }
    bool hasErrorFlag() const noexcept { return errorFlag.load (std::memory_order_relaxed); }

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

    std::atomic<std::uint64_t> inputCallbackCount { 0 };
    std::atomic<std::uint64_t> outputCallbackCount { 0 };
    std::atomic<bool> errorFlag { false };
    std::atomic<float> cpuLoad { 0.0f };

    std::vector<float> monoScratch;      // 出力コールバックの一時バッファ(pull用)。open()内でのみ確保する。
    std::vector<float> inputMonoScratch; // 入力コールバックの一時バッファ(モノラル化用)。open()内でのみ確保する。
};

} // namespace vc
