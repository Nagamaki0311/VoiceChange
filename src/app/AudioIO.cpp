#include "AudioIO.h"

#include <cstring>

namespace vc
{

// ===== SECTION: コールバッククラス =====

class AudioIO::InputCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit InputCallback (AudioIO& ownerIn) : owner (ownerIn) {}

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
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
        }

        owner.inputCallbackCount.fetch_add (1, std::memory_order_relaxed);
    }

private:
    AudioIO& owner;
};

class AudioIO::OutputCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit OutputCallback (AudioIO& ownerIn) : owner (ownerIn) {}

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
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

                for (int ch = 0; ch < numOutputChannels; ++ch)
                    if (float* out = outputChannelData[ch])
                        std::memcpy (out + offset, mono, sizeof (float) * (size_t) chunk);

                offset += chunk;
                remaining -= chunk;
            }
        }
        catch (...)
        {
            owner.errorFlag.store (true, std::memory_order_relaxed);

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
};

// ===== SECTION: AudioIO =====

AudioIO::AudioIO()
{
    createDeviceTypes();
}

AudioIO::~AudioIO()
{
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

    errorFlag.store (false, std::memory_order_relaxed);
    inputCallbackCount.store (0, std::memory_order_relaxed);
    outputCallbackCount.store (0, std::memory_order_relaxed);
    cpuLoad.store (0.0f, std::memory_order_relaxed);

    inputCallback = std::make_unique<InputCallback> (*this);
    outputCallback = std::make_unique<OutputCallback> (*this);

    inputDevice->start (inputCallback.get());
    outputDevice->start (outputCallback.get());

    return true;
}

void AudioIO::close()
{
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
}

LatencyBreakdown AudioIO::getLatency() const noexcept
{
    LatencyBreakdown b;

    b.deviceInMs = inputInfo.rate > 0.0
        ? ((double) inputInfo.latencySamples + (double) inputInfo.bufferSize) / inputInfo.rate * 1000.0
        : 0.0;

    b.deviceOutMs = outputInfo.rate > 0.0
        ? ((double) outputInfo.latencySamples + (double) outputInfo.bufferSize) / outputInfo.rate * 1000.0
        : 0.0;

    b.ringBufferMs = fifo.getLatencyMs();
    b.totalMs = b.deviceInMs + b.deviceOutMs + b.ringBufferMs;

    return b;
}

juce::String AudioIO::getErrorText() const
{
    return errorText;
}

} // namespace vc
