#include "AudioIO.h"

#include <cstring>

namespace vc
{

bool containsCableInput (const juce::StringArray& deviceNames) noexcept
{
    for (const auto& n : deviceNames)
        if (n.containsIgnoreCase ("CABLE Input"))
            return true;

    return false;
}

// ===== SECTION: コールバッククラス =====

// audioDeviceAboutToStart/audioDeviceStoppedの検出はInputCallback・OutputCallbackで共通のため、
// 小さなヘルパーにまとめる(継承ではなくメンバとして持つ。両クラスの主目的はコールバック処理そのもの)。
class StartStopWatcher
{
public:
    // 2回目以降のaboutToStartでレート・バッファ長が変化していればreopenRequestedFlagを立てる
    // (docs/plan.md 2.5節「AudioIOの2回目以降のaudioDeviceAboutToStartでレート変化」)。
    void aboutToStart (juce::AudioIODevice* device, std::atomic<bool>& reopenRequestedFlag) noexcept
    {
        if (device == nullptr)
            return;

        const double rate = device->getCurrentSampleRate();
        const int bufferSize = device->getCurrentBufferSizeSamples();

        if (hasStartedOnce && (rate != lastRate || bufferSize != lastBufferSize))
            reopenRequestedFlag.store (true, std::memory_order_relaxed);

        hasStartedOnce = true;
        lastRate = rate;
        lastBufferSize = bufferSize;
    }

    // こちらからのclose()中のstop()以外でstopped()が呼ばれた場合は無言終了とみなす
    // (docs/plan.md 2.5節「WASAPI入力スレッドの無言終了」)。
    static void stopped (const std::atomic<bool>& expectingIntentionalStop, std::atomic<bool>& deviceStoppedFlag) noexcept
    {
        if (! expectingIntentionalStop.load (std::memory_order_relaxed))
            deviceStoppedFlag.store (true, std::memory_order_relaxed);
    }

private:
    bool hasStartedOnce = false;
    double lastRate = 0.0;
    int lastBufferSize = 0;
};

class AudioIO::InputCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit InputCallback (AudioIO& ownerIn) : owner (ownerIn) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        watcher.aboutToStart (device, owner.reopenRequestedFlag);
    }

    void audioDeviceStopped() override
    {
        StartStopWatcher::stopped (owner.expectingIntentionalStop, owner.deviceStoppedFlag);
    }

    void audioDeviceError (const juce::String&) override { owner.errorFlag.store (true, std::memory_order_relaxed); }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                            float* const* /*outputChannelData*/, int /*numOutputChannels*/,
                                            int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const juce::ScopedNoDenormals noDenormals;

        // D-012: 連続換算充填量の基準時刻。単調時計（ミリ秒カウンタ）を秒に変換する。
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

        try
        {
            float* const mono = owner.inputMonoScratch.data();
            const int scratchSize = (int) owner.inputMonoScratch.size();

            int offset = 0;
            int remaining = numSamples;

            while (remaining > 0)
            {
                const int chunk = juce::jmin (remaining, scratchSize);

                for (int i = 0; i < chunk; ++i)
                {
                    float sum = 0.0f;
                    int count = 0;

                    for (int ch = 0; ch < numInputChannels; ++ch)
                    {
                        if (const float* data = inputChannelData[ch])
                        {
                            sum += data[offset + i];
                            ++count;
                        }
                    }

                    mono[i] = count > 0 ? sum / (float) count : 0.0f;
                }

                owner.fifo.push (mono, chunk, now);

                offset += chunk;
                remaining -= chunk;
            }
        }
        catch (...)
        {
            owner.errorFlag.store (true, std::memory_order_relaxed);
            owner.engine.raiseExceptionErrorFlag();
        }

        owner.inputCallbackCount.fetch_add (1, std::memory_order_relaxed);
    }

private:
    AudioIO& owner;
    StartStopWatcher watcher;
};

class AudioIO::OutputCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit OutputCallback (AudioIO& ownerIn) : owner (ownerIn) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        watcher.aboutToStart (device, owner.reopenRequestedFlag);
    }

    void audioDeviceStopped() override
    {
        StartStopWatcher::stopped (owner.expectingIntentionalStop, owner.deviceStoppedFlag);
    }

    void audioDeviceError (const juce::String&) override { owner.errorFlag.store (true, std::memory_order_relaxed); }

    void audioDeviceIOCallbackWithContext (const float* const* /*inputChannelData*/, int /*numInputChannels*/,
                                            float* const* outputChannelData, int numOutputChannels,
                                            int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const juce::ScopedNoDenormals noDenormals;

        const juce::int64 startTicks = juce::Time::getHighResolutionTicks();

        // D-012: 連続換算充填量の基準時刻。push側と同じ単調時計。
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

        try
        {
            float* const mono = owner.monoScratch.data();
            const int scratchSize = (int) owner.monoScratch.size();

            int offset = 0;
            int remaining = numSamples;

            while (remaining > 0)
            {
                const int chunk = juce::jmin (remaining, scratchSize);

                owner.fifo.pull (mono, chunk, now);
                owner.engine.process (mono, chunk);

                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (float* out = outputChannelData[ch])
                        std::memcpy (out + offset, mono, sizeof (float) * (size_t) chunk);

                offset += chunk;
                remaining -= chunk;
            }
        }
        catch (...)
        {
            // 例外時は無音＋エラーフラグ（bit1）。
            owner.errorFlag.store (true, std::memory_order_relaxed);
            owner.engine.raiseExceptionErrorFlag();

            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (float* out = outputChannelData[ch])
                    juce::FloatVectorOperations::clear (out, numSamples);
        }

        const juce::int64 endTicks = juce::Time::getHighResolutionTicks();
        const double elapsedSeconds = (double) (endTicks - startTicks) / (double) juce::Time::getHighResolutionTicksPerSecond();
        const double audioSeconds = owner.outputInfo.rate > 0.0 ? (double) numSamples / owner.outputInfo.rate : 0.0;
        const float instantLoad = audioSeconds > 0.0 ? (float) (elapsedSeconds / audioSeconds) : 0.0f;

        // CPU使用率の平滑化(簡易指数移動平均)。
        const float prev = owner.cpuLoad.load (std::memory_order_relaxed);
        owner.cpuLoad.store (prev * 0.9f + instantLoad * 0.1f, std::memory_order_relaxed);

        owner.outputCallbackCount.fetch_add (1, std::memory_order_relaxed);
    }

private:
    AudioIO& owner;
    StartStopWatcher watcher;
};

// ===== SECTION: AudioIO =====

AudioIO::AudioIO()
{
    createDeviceTypes();
    startTimer (500); // ConnectionMonitorの評価(docs/plan.md 2.5節「AudioIO」)。デバイス未オープン中も動かし続ける。
}

AudioIO::~AudioIO()
{
    stopTimer();

    for (auto* t : deviceTypesLowLatency)
        t->removeListener (this);
    for (auto* t : deviceTypesShared)
        t->removeListener (this);

    close();
}

void AudioIO::createDeviceTypes()
{
#if JUCE_WINDOWS
    deviceTypesLowLatency.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    deviceTypesShared.add (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));

    if (auto* t = deviceTypesLowLatency.getFirst())
        t->scanForDevices();
    if (auto* t = deviceTypesShared.getFirst())
        t->scanForDevices();
#else
    // Linux（開発用）: 既定のデバイス型を1つだけ使う。
    juce::AudioDeviceManager dummyManagerForTypes;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    dummyManagerForTypes.createAudioDeviceTypes (types);

    if (auto* first = types.getFirst())
    {
        first->scanForDevices();
        deviceTypesLowLatency.add (types.removeAndReturn (0));
    }
#endif

    // 一覧変更通知(audioDeviceListChanged)をConnectionMonitorの評価に流す(docs/plan.md 2.5節)。
    for (auto* t : deviceTypesLowLatency)
        t->addListener (this);
    for (auto* t : deviceTypesShared)
        t->addListener (this);
}

juce::AudioIODeviceType* AudioIO::nameListType() const
{
    return deviceTypesLowLatency.getFirst();
}

juce::StringArray AudioIO::getInputNames() const
{
    if (auto* t = nameListType())
        return t->getDeviceNames (true);

    return {};
}

juce::StringArray AudioIO::getOutputNames() const
{
    if (auto* t = nameListType())
        return t->getDeviceNames (false);

    return {};
}

std::unique_ptr<juce::AudioIODevice> AudioIO::openOneDevice (const juce::String& outName, const juce::String& inName,
                                                              bool wantInput, DeviceInfo& infoOut)
{
    const juce::String targetName = wantInput ? inName : outName;

    if (targetName.isEmpty())
        return nullptr;

    auto tryOpen = [&] (juce::AudioIODeviceType* type, bool lowLatency) -> std::unique_ptr<juce::AudioIODevice>
    {
        if (type == nullptr)
            return nullptr;

        std::unique_ptr<juce::AudioIODevice> device (wantInput ? type->createDevice ({}, targetName)
                                                                : type->createDevice (targetName, {}));

        if (device == nullptr)
            return nullptr;

        int bufferSize = device->getDefaultBufferSize();

        if (lowLatency)
        {
            const juce::Array<int> sizes = device->getAvailableBufferSizes();

            if (! sizes.isEmpty())
            {
                bufferSize = sizes.getFirst();

                for (const int s : sizes)
                    bufferSize = juce::jmin (bufferSize, s);
            }
        }

        const int numChannels = wantInput ? device->getInputChannelNames().size()
                                           : device->getOutputChannelNames().size();

        juce::BigInteger channels;
        channels.setRange (0, numChannels, true);
        const juce::BigInteger empty;

        const juce::String err = wantInput ? device->open (channels, empty, 0.0, bufferSize)
                                            : device->open (empty, channels, 0.0, bufferSize);

        if (err.isNotEmpty())
            return nullptr;

        infoOut.name = device->getName();
        infoOut.lowLatency = lowLatency;
        infoOut.rate = device->getCurrentSampleRate();
        infoOut.bufferSize = device->getCurrentBufferSizeSamples();
        infoOut.latencySamples = wantInput ? device->getInputLatencyInSamples() : device->getOutputLatencyInSamples();

        return device;
    };

#if JUCE_WINDOWS
    if (auto dev = tryOpen (deviceTypesLowLatency.getFirst(), true))
        return dev;

    return tryOpen (deviceTypesShared.getFirst(), false);
#else
    return tryOpen (deviceTypesLowLatency.getFirst(), false);
#endif
}

bool AudioIO::open (const juce::String& inName, const juce::String& outName)
{
    close();

    // 成功・失敗を問わず記録する(ConnectionMonitorのTryReopenが同じ名前で開き直すため。D-009)。
    desiredInputName = inName;
    desiredOutputName = outName;

    errorText.clear();

    DeviceInfo newInputInfo, newOutputInfo;
    auto newInputDevice = openOneDevice (outName, inName, true, newInputInfo);

    if (newInputDevice == nullptr)
    {
        errorText = "入力デバイスを開けませんでした: " + inName;
        return false;
    }

    auto newOutputDevice = openOneDevice (outName, inName, false, newOutputInfo);

    if (newOutputDevice == nullptr)
    {
        newInputDevice->close();
        errorText = "出力デバイスを開けませんでした: " + outName;
        return false;
    }

    inputDevice = std::move (newInputDevice);
    outputDevice = std::move (newOutputDevice);
    inputInfo = newInputInfo;
    outputInfo = newOutputInfo;

    // maxIn/maxOutBlockは実際のバッファ長に余裕を持たせ、想定外に大きいコールバックが来ても
    // ResamplingFifo::pull()のスクラッチ容量を超えないようにする（AudioIO側でチャンク分割もする）。
    const int maxInBlock = juce::jmax (inputInfo.bufferSize * 2, 4096);
    const int maxOutBlock = juce::jmax (outputInfo.bufferSize * 2, 4096);

    inputMonoScratch.assign ((size_t) maxInBlock, 0.0f);
    monoScratch.assign ((size_t) maxOutBlock, 0.0f);

    fifo.prepare (inputInfo.rate, outputInfo.rate, maxInBlock, maxOutBlock, inputInfo.bufferSize, outputInfo.bufferSize);

    // D-014: ピッチシフターのブロック長は120ms固定（PitchShifter::prepare内部でpresetDefault
    // 相当を使う）。デバイス遅延から毎回計算する方式(D-003・decideStretchBlock)は廃止した。
    engine.prepare ({ outputInfo.rate, maxOutBlock });

    errorFlag.store (false, std::memory_order_relaxed);
    deviceStoppedFlag.store (false, std::memory_order_relaxed);
    reopenRequestedFlag.store (false, std::memory_order_relaxed);
    inputCallbackCount.store (0, std::memory_order_relaxed);
    outputCallbackCount.store (0, std::memory_order_relaxed);
    cpuLoad.store (0.0f, std::memory_order_relaxed);

    inputCallback = std::make_unique<InputCallback> (*this);
    outputCallback = std::make_unique<OutputCallback> (*this);

    inputDevice->start (inputCallback.get());
    outputDevice->start (outputCallback.get());

    // 新しいコールバック回数(0)を基準にウォッチドッグを再開する(D-009: 両方揃ってから開き直す)。
    connectionMonitor = ConnectionMonitor {};
    reconnectStartMs = 0;

    return true;
}

void AudioIO::close()
{
    // stop()自体がaudioDeviceStopped()を呼びうるため、こちらからの意図した停止であることを
    // 先に伝えておく(無言終了の誤検出を防ぐ。docs/plan.md 2.5節)。
    expectingIntentionalStop.store (true, std::memory_order_relaxed);

    if (inputDevice != nullptr)
    {
        inputDevice->stop();
        inputDevice->close();
        inputDevice.reset();
    }

    if (outputDevice != nullptr)
    {
        outputDevice->stop();
        outputDevice->close();
        outputDevice.reset();
    }

    inputCallback.reset();
    outputCallback.reset();

    expectingIntentionalStop.store (false, std::memory_order_relaxed);
}

LatencyBreakdown AudioIO::getLatency() const noexcept
{
    LatencyBreakdown b;

    // レビュー指摘3: getInput/OutputLatencyInSamples()は既に「ストリーム遅延+バッファ長」を
    // 返す（JUCEのWASAPI実装ではlatencyIn = latencySamples + currentBufferSizeSamples）ため、
    // ここでbufferSizeを再加算すると二重計上になる。
    // 注: Linux（ALSA）ではこの値にバッファ長が含まれない実装差異があるが、Linuxは開発用のため対応しない。
    b.deviceInMs = inputInfo.rate > 0.0
        ? (double) inputInfo.latencySamples / inputInfo.rate * 1000.0
        : 0.0;

    b.deviceOutMs = outputInfo.rate > 0.0
        ? (double) outputInfo.latencySamples / outputInfo.rate * 1000.0
        : 0.0;

    b.ringBufferMs = fifo.getLatencyMs();

    b.shifterMs = outputInfo.rate > 0.0
        ? (double) engine.getShifterLatencySamples() / outputInfo.rate * 1000.0
        : 0.0;

    b.totalMs = b.deviceInMs + b.deviceOutMs + b.ringBufferMs + b.shifterMs;

    return b;
}

juce::String AudioIO::getErrorText() const
{
    return errorText;
}

// ===== SECTION: T-007 再接続・エラー表示 =====

double AudioIO::getReconnectElapsedSeconds() const noexcept
{
    if (! connectionMonitor.isFailed())
        return 0.0;

    const auto nowMs = (juce::int64) juce::Time::getMillisecondCounter();
    return juce::jmax (0.0, (double) (nowMs - reconnectStartMs) * 0.001);
}

bool AudioIO::hasRecentEngineError() const noexcept
{
    if (lastEngineErrorMs == 0)
        return false;

    const auto nowMs = (juce::int64) juce::Time::getMillisecondCounter();
    return (nowMs - lastEngineErrorMs) < 10000;
}

juce::String AudioIO::getLastEngineErrorTimeText() const noexcept
{
    return lastEngineErrorWallClock.formatted ("%H:%M:%S");
}

void AudioIO::timerCallback()
{
    // Engineのエラーフラグ(bit0: 非有限値, bit1: 例外)はここだけが読んで消費する(design.md 6.1節 E5)。
    if (engine.getErrorFlags() != 0)
    {
        engine.clearErrorFlags();
        lastEngineErrorMs = (juce::int64) juce::Time::getMillisecondCounter();
        lastEngineErrorWallClock = juce::Time::getCurrentTime();
    }

    evaluateConnection (listChangedFlag.exchange (false, std::memory_order_relaxed));
}

void AudioIO::evaluateConnection (bool listChangedThisTick)
{
    // まだ一度もopen()を試みていない(起動直後、入力または出力が0件で自動オープンされていない等)場合は
    // 監視対象がない。ここで評価してしまうと、コールバック回数が進まないことを「切断」と誤検出し、
    // design.md 6.1節のE6(入力デバイス0件)等より優先度の高いE1〜E3を誤って出してしまう。
    if (desiredInputName.isEmpty() && desiredOutputName.isEmpty())
        return;

    // (1)audioDeviceError・例外、(2)無言終了を合成してConnectionMonitorのerrorFlagにする。
    const bool combinedError = errorFlag.exchange (false, std::memory_order_relaxed)
                              || deviceStoppedFlag.exchange (false, std::memory_order_relaxed);
    const bool reopenReq = reopenRequestedFlag.exchange (false, std::memory_order_relaxed);

    const bool devicesPresent = desiredInputName.isNotEmpty() && desiredOutputName.isNotEmpty()
                               && getInputNames().contains (desiredInputName)
                               && getOutputNames().contains (desiredOutputName);

    const auto nowMs = (juce::int64) juce::Time::getMillisecondCounter();

    const auto action = connectionMonitor.update (nowMs,
        inputCallbackCount.load (std::memory_order_relaxed),
        outputCallbackCount.load (std::memory_order_relaxed),
        combinedError, reopenReq, devicesPresent, listChangedThisTick);

    if (action == ConnectionMonitor::Action::CloseAndFail)
    {
        reconnectStartMs = nowMs;
        close(); // D-009: 片方だけの異常でも両方閉じる(inputDevice/outputDeviceを個別に見ない)
    }
    else if (action == ConnectionMonitor::Action::TryReopen)
    {
        // D-009: 保存済みデバイスが見つからない間も同じ名前で再試行を続ける(既定へは切り替えない)。
        open (desiredInputName, desiredOutputName); // 成功時はopen()内でconnectionMonitorがリセットされる
    }
}

} // namespace vc
