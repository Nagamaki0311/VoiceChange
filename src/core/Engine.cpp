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
constexpr double kEffectCrossfadeSeconds = 0.020;
constexpr float kDefaultCarrierHz = 110.0f; // 一度も有声にならないうちのトークボックスのキャリア
constexpr float kVoicedThreshold = 0.5f;

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
    fxInScratch.assign ((size_t) maxBlockSamples, 0.0f);
    fxOldScratch.assign ((size_t) maxBlockSamples, 0.0f);

    noiseReducer.prepare (sampleRate, maxBlockSamples);
    equalizer.prepare (sampleRate);
    shifter.prepare (sampleRate, maxBlockSamples);
    detector.prepare (sampleRate, maxBlockSamples);
    echo.prepare (sampleRate, maxBlockSamples);
    ringMod.prepare (sampleRate, maxBlockSamples);
    talkbox.prepare (sampleRate, maxBlockSamples);

    const int presetIdx = juce::jlimit (0, (int) kPresets.size() - 1, atomicParams.preset.load (std::memory_order_relaxed));
    activeEffect = kPresets[(size_t) presetIdx].effect;
    fadeFrom = Effect::None;
    fading = false;
    fadePos = 0;
    fadeLen = std::max (1, (int) std::lround (kEffectCrossfadeSeconds * sampleRate));
    detectorRunning = false;
    kerokeroCorrection = 0.0f;

    reverb.setSampleRate (sampleRate);
    reverb.reset();
    reverbActive = false;
    reverbParamsStale = true;
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

    presetGainSmoothed.reset (sampleRate, kEffectCrossfadeSeconds);
    presetGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (kPresets[(size_t) presetIdx].gainDb));

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
    detector.reset();
    detectorRunning = false;
    kerokeroCorrection = 0.0f;
    echo.reset();
    ringMod.reset();
    talkbox.reset();
    fading = false;
    reverb.reset();
    reverbActive = false;
    reverbParamsStale = true;
    compressor.reset();
}

void Engine::resetMic() noexcept
{
    noiseReducer.reset();
    equalizer.reset();
}

void Engine::handleNonFinite (float* buf, int n) noexcept
{
    juce::FloatVectorOperations::clear (buf, n);
    resetChain();
    resetMic(); // 非有限値の検出時はマイク処理もリセットする（D-020）。バイパス解除時とは違う
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
        // setParametersは補間中はサンプルごと、収束後は最初の1回だけ（rが一定なら値は変わらない）。
        const bool smoothing = reverbGainSmoothed.isSmoothing();
        const float r = reverbGainSmoothed.getNextValue();

        if (smoothing || reverbParamsStale)
        {
            juce::Reverb::Parameters p;
            p.roomSize = 0.5f;
            p.damping = 0.5f;
            p.wetLevel = r / 3.0f;        // D-008: JUCEの内部倍率(×3)を打ち消す
            p.dryLevel = (1.0f - r) / 2.0f; // D-008: JUCEの内部倍率(×2)を打ち消す
            p.width = 1.0f;
            p.freezeMode = 0.0f;
            reverb.setParameters (p);
            reverbParamsStale = false;
        }

        reverb.processMono (buf + i, 1);
    }

    if (! reverbGainSmoothed.isSmoothing() && reverbGainSmoothed.getCurrentValue() <= 0.0f)
    {
        reverb.reset();
        reverbActive = false;
        reverbParamsStale = true;
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

void Engine::runEffect (Effect effect, const float* in, float* out, int n, float carrierHz, float voicing) noexcept
{
    switch (effect)
    {
        case Effect::None:
            if (in != out)
                std::memcpy (out, in, sizeof (float) * (size_t) n);
            break;
        case Effect::Echo:     echo.process (in, out, n); break;
        case Effect::Robot:    ringMod.process (in, out, n); break;
        case Effect::Talkbox:  talkbox.process (in, out, n, carrierHz, voicing); break;
    }
}

// 層2のピッチ以外の効果。効果が変わるときは新効果をreset()し、旧新を20ms並行処理してクロスフェードする。
// 切替中に来た次の変更は、切替が終わった次のブロックで反映する（保留）。
void Engine::processLayer2 (float* buf, int n, Effect desired, float carrierHz, float voicing) noexcept
{
    if (! fading && desired != activeEffect)
    {
        fadeFrom = activeEffect;
        activeEffect = desired;
        fading = true;
        fadePos = 0;

        switch (activeEffect)
        {
            case Effect::Echo:    echo.reset(); break;
            case Effect::Robot:   ringMod.reset(); break;
            case Effect::Talkbox: talkbox.reset(); break;
            case Effect::None:    break;
        }
    }

    if (! fading)
    {
        runEffect (activeEffect, buf, buf, n, carrierHz, voicing);
        return;
    }

    std::memcpy (fxInScratch.data(), buf, sizeof (float) * (size_t) n);
    runEffect (fadeFrom, fxInScratch.data(), fxOldScratch.data(), n, carrierHz, voicing);
    runEffect (activeEffect, fxInScratch.data(), buf, n, carrierHz, voicing);

    for (int i = 0; i < n && fadePos < fadeLen; ++i, ++fadePos)
    {
        const float g = (float) (fadePos + 1) / (float) fadeLen;
        buf[i] = fxOldScratch[(size_t) i] * (1.0f - g) + buf[i] * g;
    }

    if (fadePos >= fadeLen)
        fading = false;
}

void Engine::processChain (float* buf, int n, int presetIdx, int pitchSemis, float gainDb, float reverbAmt) noexcept
{
    const auto presetEnum = static_cast<Preset> (presetIdx);
    const auto& spec = kPresets[(size_t) presetIdx];

    // ピッチ検出はケロケロ・トークボックス（切替中の旧効果を含む）のときだけ動かす。加工前の声を検出する。
    const bool needDetector = spec.needsDetector || activeEffect == Effect::Talkbox || (fading && fadeFrom == Effect::Talkbox);

    if (needDetector && ! detectorRunning)
    {
        detector.reset();
        kerokeroCorrection = 0.0f;
    }

    detectorRunning = needDetector;

    float detectedHz = 0.0f;
    float voicing = 0.0f;

    if (needDetector)
    {
        detector.process (buf, n);
        detectedHz = detector.getFrequencyHz();
        voicing = detector.getVoicing();

        // ケロケロの補正量 = round(m) - m（m = 検出音高のMIDIノート番号、A4 = 440Hz）。
        // 無声・無音区間は直前の補正量を保つ。
        if (voicing >= kVoicedThreshold && detectedHz > 0.0f)
        {
            const float m = 69.0f + 12.0f * std::log2 (detectedHz / 440.0f);
            kerokeroCorrection = std::round (m) - m;
        }
    }

    const bool shifterRun = shifterShouldRun (presetEnum, pitchSemis);
    const float correction = presetEnum == Preset::Kerokero ? kerokeroCorrection : 0.0f;
    const float semitonesTotal = (float) pitchSemis + spec.semitones + correction;

    shifter.setTarget (shifterRun, semitonesTotal, spec.formant);
    shifter.process (buf, buf, n);

    // トークボックスのキャリア = 検出f0 × 2^(層1ピッチ/12)。一度も有声にならないうちは既定値を使う。
    const float carrierHz = (detectedHz > 0.0f ? detectedHz : kDefaultCarrierHz) * std::exp2 ((float) pitchSemis / 12.0f);
    processLayer2 (buf, n, spec.effect, carrierHz, voicing);

    processReverb (buf, n, reverbAmt);

    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
    gainSmoothed.applyGain (buf, n);

    // プリセットの声量補正（spec.gainDb、D-027）。層2・リバーブ・層1のゲインの後、リミッターの前。層1のゲイン（UIのゲインスライダー）とは別の補間器で、
    // 値は変えない。切替時の変化は、声質の切替（層2・シフターのクロスフェード）と同じ20msで補間する（クリックが出ず、切替の最中に補正が
    // 古い値のまま残って過渡のピークを持ち上げることもない。層1と共通の50msでは、ミニオン→トークボックスがN9bの限界1.5を超えた）。
    presetGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (spec.gainDb));
    presetGainSmoothed.applyGain (buf, n);

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
    // 防御: 主対策はParams::sanitize()（設定読み込み時）。ここは読み出し側での最終防御
    // （レビュー指摘2）で、非有限値が紛れ込んでも初期値として扱い恒久的な無音化を防ぐ。
    const float rawGainDb = atomicParams.gainDb.load (std::memory_order_relaxed);
    const float rawReverbAmt = atomicParams.reverb.load (std::memory_order_relaxed);
    const float gainDb = std::isfinite (rawGainDb) ? rawGainDb : 0.0f;
    const float reverbAmt = std::isfinite (rawReverbAmt) ? rawReverbAmt : 0.0f;
    const int pitchSemis = atomicParams.pitch.load (std::memory_order_relaxed);
    const int presetIdx = juce::jlimit (0, (int) kPresets.size() - 1, atomicParams.preset.load (std::memory_order_relaxed));
    const bool targetOn = atomicParams.enabled.load (std::memory_order_relaxed);

    updateInputPeak (buf, n);

    // マイク処理（D-020: ノイズ除去 → EQ）: バイパス判定の前。全体OFFでも動き、バイパス解除でリセットしない。
    // 以降のdryScratch・バイパスの素通しは、マイク処理後の信号になる。
    const float rawNrBackground = atomicParams.nrBackground.load (std::memory_order_relaxed);
    const float rawNrImpact = atomicParams.nrImpact.load (std::memory_order_relaxed);
    noiseReducer.setTarget (atomicParams.nrEnabled.load (std::memory_order_relaxed),
                            std::isfinite (rawNrBackground) ? rawNrBackground : kNrBackgroundDefault,
                            std::isfinite (rawNrImpact) ? rawNrImpact : 0.0f);

    if (! noiseReducer.process (buf, n))
    {
        // 入力に非有限値がある。RNNoiseは遅延の後で出力へ出すため、出力の検査では遅れて検出することになる。
        // 入力の時点で、このブロックを無音にして全体をリセットする（チェーンの先頭がノイズ除去のとき、
        // E5の「そのブロックを無音・フラグ」と同じ挙動にそろえる）。
        handleNonFinite (buf, n);
        return;
    }

    // EQ（NoiseReducerの直後）。各フィールドは独立に読む（1ブロックだけ新旧混在しうる。plan.md 8.2）。
    // 範囲外・非有限値・不正なタイプのint値は、Equalizer::setTarget（sanitizeEqBand）が丸める・初期値にする。
    std::array<EqBandSettings, kEqBands> eqSettings;

    for (size_t i = 0; i < eqSettings.size(); ++i)
    {
        const auto& a = atomicParams.eqBands[i];
        eqSettings[i] = { a.on.load (std::memory_order_relaxed),
                          eqTypeFromInt (a.type.load (std::memory_order_relaxed), kEqDefaults[i].type),
                          a.hz.load (std::memory_order_relaxed),
                          a.gainDb.load (std::memory_order_relaxed),
                          a.q.load (std::memory_order_relaxed) };
    }

    equalizer.setTarget (atomicParams.eqEnabled.load (std::memory_order_relaxed), eqSettings,
                         atomicParams.eqOutputGainDb.load (std::memory_order_relaxed));
    equalizer.process (buf, n);

    if (! targetOn && chainGain <= 0.0)
    {
        // 完全バイパス: チェーン未処理。最終出力の非有限値検査（D-010）と±1.0へのクリップだけは常に行う。
        // クリップはリミッターを通らない入力（補間のオーバーシュート等）が0dBFSを超えないため（D-018）。
        // |x|<=1ではビット一致。
        if (checkAndHandleFinalNonFinite (buf, n))
            juce::FloatVectorOperations::clip (buf, buf, -1.0f, 1.0f, n);
        return;
    }

    if (targetOn && chainGain <= 0.0)
        resetChain(); // 解除時は層1・層2だけreset()する（シフターはPriming経由で復帰）。マイク処理はリセットしない（D-020）

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

    // クロスフェード中は入力（dry）が混ざるので、完全バイパスと同じくクリップする（D-018）。
    if (checkAndHandleFinalNonFinite (buf, n))
        juce::FloatVectorOperations::clip (buf, buf, -1.0f, 1.0f, n);
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
