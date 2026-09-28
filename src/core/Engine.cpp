#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vc
{

namespace
{
constexpr double kBypassCrossfadeSeconds = 0.020;
constexpr double kGainRampSeconds = 0.050;
constexpr double kReverbRampSeconds = 0.020;

bool allFinite (const float* data, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (data[i]))
            return false;

    return true;
}
} // namespace

void Engine::prepare (const EngineConfig& config)
{
    sampleRate = config.sampleRate;
    maxBlockSamples = config.maxBlock;

    dryScratch.assign ((size_t) maxBlockSamples, 0.0f);

    shifter.prepare (sampleRate, maxBlockSamples);

    reverb.setSampleRate (sampleRate);
    reverb.reset();
    reverbActive = false;
    reverbGainSmoothed.reset (sampleRate, kReverbRampSeconds);
    reverbGainSmoothed.setCurrentAndTargetValue (0.0f);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSamples, 1u };
    compressor.prepare (spec);
    compressor.setThreshold (-1.0f);
    compressor.setRatio (100.0f);
    compressor.setAttack (0.1f);
    compressor.setRelease (100.0f);
    compressor.reset();

    gainSmoothed.reset (sampleRate, kGainRampSeconds);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (atomicParams.gainDb.load (std::memory_order_relaxed)));

    chainGain = atomicParams.enabled.load (std::memory_order_relaxed) ? 1.0 : 0.0;

    inputPeak.store (0.0f, std::memory_order_relaxed);
    errorFlags.store (0, std::memory_order_relaxed);
}

void Engine::updateInputPeak (const float* buf, int n) noexcept
{
    float localPeak = 0.0f;

    for (int i = 0; i < n; ++i)
        localPeak = std::max (localPeak, std::abs (buf[i]));

    float prev = inputPeak.load (std::memory_order_relaxed);

    while (localPeak > prev && ! inputPeak.compare_exchange_weak (prev, localPeak, std::memory_order_relaxed))
    {
    }
}

float Engine::takeInputPeak() noexcept
{
    return inputPeak.exchange (0.0f, std::memory_order_relaxed);
}

void Engine::resetChain() noexcept
{
    shifter.reset();
    reverb.reset();
    reverbActive = false;
    compressor.reset();
}

void Engine::handleNonFinite (float* buf, int n) noexcept
{
    juce::FloatVectorOperations::clear (buf, n);
    resetChain();
    errorFlags.fetch_or (0x1u, std::memory_order_relaxed);
}

bool Engine::checkAndHandleFinalNonFinite (float* buf, int n) noexcept
{
    if (allFinite (buf, n))
        return true;

    handleNonFinite (buf, n);
    return false;
}

void Engine::processReverb (float* buf, int n, float reverbAmt) noexcept
{
    reverbGainSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, reverbAmt));

    if (! reverbActive && ! reverbGainSmoothed.isSmoothing() && reverbGainSmoothed.getCurrentValue() <= 0.0f)
        return; // r=0で補間完了 → 処理を止める（D-008）

    reverbActive = true;

    for (int i = 0; i < n; ++i)
    {
        const float r = reverbGainSmoothed.getNextValue();

        juce::Reverb::Parameters p;
        p.roomSize = 0.5f;
        p.damping = 0.5f;
        p.wetLevel = r / 3.0f;        // D-008: JUCEの内部倍率(×3)を打ち消す
        p.dryLevel = (1.0f - r) / 2.0f; // D-008: JUCEの内部倍率(×2)を打ち消す
        p.width = 1.0f;
        p.freezeMode = 0.0f;
        reverb.setParameters (p);

        reverb.processMono (buf + i, 1);
    }

    if (! reverbGainSmoothed.isSmoothing() && reverbGainSmoothed.getCurrentValue() <= 0.0f)
    {
        reverb.reset();
        reverbActive = false;
    }
}

void Engine::applyLimiter (float* buf, int n) noexcept
{
    // D-007: juce::dsp::Limiterは常時圧縮がかかり透過的でないため使わない。
    // Compressor（閾値-1dB・比100・アタック0.1ms・リリース100ms）+ クリップで構成する。
    float* channels[1] = { buf };
    juce::dsp::AudioBlock<float> block (channels, 1, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> context (block);
    compressor.process (context);

    juce::FloatVectorOperations::clip (buf, buf, -1.0f, 1.0f, n);
}

void Engine::processChain (float* buf, int n, int presetIdx, int pitchSemis, float gainDb, float reverbAmt) noexcept
{
    const auto presetEnum = static_cast<Preset> (presetIdx);
    const auto& spec = kPresets[(size_t) presetIdx];

    const bool shifterRun = shifterShouldRun (presetEnum, pitchSemis);
    const float semitonesTotal = (float) pitchSemis + spec.semitones; // ケロケロ補正はT-005

    shifter.setTarget (shifterRun, semitonesTotal, spec.formant);
    shifter.process (buf, buf, n);

    // 層2のピッチ以外の効果（エコー/ロボット/トークボックス）はT-005で追加する。
    // T-004時点ではプリセットを選んでも効果なしで通す。

    processReverb (buf, n, reverbAmt);

    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
    gainSmoothed.applyGain (buf, n);

    if (! allFinite (buf, n))
    {
        handleNonFinite (buf, n);
        return;
    }

    applyLimiter (buf, n);

    if (! allFinite (buf, n))
        handleNonFinite (buf, n);
}

void Engine::processChunk (float* buf, int n) noexcept
{
    const float gainDb = atomicParams.gainDb.load (std::memory_order_relaxed);
    const float reverbAmt = atomicParams.reverb.load (std::memory_order_relaxed);
    const int pitchSemis = atomicParams.pitch.load (std::memory_order_relaxed);
    const int presetIdx = juce::jlimit (0, (int) kPresets.size() - 1, atomicParams.preset.load (std::memory_order_relaxed));
    const bool targetOn = atomicParams.enabled.load (std::memory_order_relaxed);

    updateInputPeak (buf, n);

    if (! targetOn && chainGain <= 0.0)
    {
        // 完全バイパス: チェーン未処理・ビット一致。最終出力の非有限値検査だけは常に行う（D-010）。
        checkAndHandleFinalNonFinite (buf, n);
        return;
    }

    if (targetOn && chainGain <= 0.0)
        resetChain(); // 解除時はチェーンをreset()する（シフターはPriming経由で復帰）

    std::memcpy (dryScratch.data(), buf, sizeof (float) * (size_t) n);

    processChain (buf, n, presetIdx, pitchSemis, gainDb, reverbAmt);

    if (targetOn && chainGain >= 1.0)
    {
        checkAndHandleFinalNonFinite (buf, n);
        return;
    }

    // バイパス切替のクロスフェード（20ms）。gainは反転しても現在値から続けて動く。
    const double step = 1.0 / (kBypassCrossfadeSeconds * sampleRate);

    for (int i = 0; i < n; ++i)
    {
        buf[i] = (float) (dryScratch[(size_t) i] * (1.0 - chainGain) + (double) buf[i] * chainGain);
        chainGain += targetOn ? step : -step;
        chainGain = juce::jlimit (0.0, 1.0, chainGain);
    }

    checkAndHandleFinalNonFinite (buf, n);
}

void Engine::process (float* monoInOut, int n) noexcept
{
    int offset = 0;

    while (n > 0)
    {
        const int chunk = juce::jmin (n, maxBlockSamples);
        processChunk (monoInOut + offset, chunk);
        offset += chunk;
        n -= chunk;
    }
}

} // namespace vc
