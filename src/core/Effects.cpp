#include "Effects.h"

#include <algorithm>
#include <cmath>

namespace vc
{

namespace
{
constexpr double kEchoDelaySeconds = 0.300;
constexpr double kEchoMaxDelaySeconds = 0.35;
constexpr float kEchoFeedback = 0.45f;
constexpr float kEchoWet = 0.6f;
constexpr float kEchoFeedbackCutoffHz = 3500.0f;
constexpr double kEchoFadeInSeconds = 0.020;

constexpr double kRingFrequencyHz = 40.0;

constexpr double kTalkboxLowHz = 120.0;
constexpr double kTalkboxHighHz = 7000.0;
// 変調側は2段重ねて実効帯域幅を中心周波数の約0.2倍（隣接バンド間隔0.24倍とほぼ同じ）にする。
// キャリア側は帯域を広げる（Q=1.5）。同じQだと、倍音が隣接バンドの境目に来たときに2バンドの位相差で
// 打ち消し合い（f0=250Hzの/u/で入力比-12dB）、母音・f0による音量差が15dBに広がったため。
constexpr float kTalkboxBandQ = 3.0f;
constexpr float kTalkboxCarrierBandQ = 1.5f;
constexpr double kTalkboxAttackSeconds = 0.005;
constexpr double kTalkboxReleaseSeconds = 0.020;
// キャリア側の各バンド出力は自身の包絡（変調側と同じ全波整流+アタック5ms/リリース20ms）で割って
// 正規化してから変調側の包絡を掛ける。鋸波の-6dB/oct傾斜とf0による倍音数の違いを打ち消し、
// 出力音量が母音・f0にほぼ依存しないようにする（正規化なしではX3aの音量差が約10dBあった）。
constexpr float kTalkboxCarrierLevelInit = 0.1f;
constexpr float kTalkboxCarrierLevelFloor = 1.0e-5f;
constexpr double kSmoothSeconds = 0.005;
constexpr float kTalkboxMinCarrierHz = 40.0f;

// キャリアは鋸波2本（±10セントのデチューン。厚み）の和。位相はそれぞれ別に持つ。
constexpr std::array<float, 2> kTalkboxDetuneCents { -10.0f, 10.0f };
// 有声度の下限。検出器が無声寄りに判定するフレーム（低い声・息混じり）でも、低域・中域のキャリアは鋸波を主にして音程のあるフレーズを保つ。
constexpr float kTalkboxVoicingFloor = 0.8f;
// 2.5kHz以上のバンドのキャリアを、下限適用後の有声度から検出器の値そのままへ近づける割合。無声の摩擦音（s・sh）を雑音で鳴らし、子音を保つ。
constexpr float kTalkboxConsonant = 0.6f;
constexpr double kTalkboxConsonantSplitHz = 2500.0;
// 鋸波2本の和を1本ぶんの振幅へそろえる（電力の和を1にする）。
constexpr float kTalkboxSawNorm = 0.70710678f;

double onePoleCoeff (double seconds, double sampleRate) noexcept
{
    return std::exp (-1.0 / (seconds * sampleRate));
}

// PolyBLEP残差。phaseは0..1、dtは1サンプルあたりの位相増分。
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }

    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }

    return 0.0f;
}
} // namespace

// ===== SECTION: Echo =====

void Echo::prepare (double sampleRate, int maxBlockSamples)
{
    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSamples, 1u };
    delay.prepare (spec);
    delay.setMaximumDelayInSamples ((int) std::ceil (kEchoMaxDelaySeconds * sampleRate) + 1);
    delay.setDelay ((float) std::lround (kEchoDelaySeconds * sampleRate));

    feedbackLowPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, kEchoFeedbackCutoffHz);

    fadeLenSamples = std::max (1, (int) std::lround (kEchoFadeInSeconds * sampleRate));
    reset();
}

void Echo::reset() noexcept
{
    delay.reset();
    feedbackLowPass.reset();
    fadePos = 0;
}

void Echo::process (const float* in, float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = in[i];
        const float delayed = delay.popSample (0);
        const float feedback = feedbackLowPass.processSample (delayed) * kEchoFeedback;

        float write = x + feedback;
        if (fadePos < fadeLenSamples)
        {
            write *= (float) fadePos / (float) fadeLenSamples;
            ++fadePos;
        }

        delay.pushSample (0, write);
        out[i] = x + kEchoWet * delayed;
    }
}

// ===== SECTION: RingModulator =====

void RingModulator::prepare (double sampleRate, int maxBlockSamples)
{
    juce::ignoreUnused (maxBlockSamples);
    phaseInc = juce::MathConstants<double>::twoPi * kRingFrequencyHz / sampleRate;
    reset();
}

void RingModulator::process (const float* in, float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        out[i] = in[i] * (float) std::sin (phase);
        phase += phaseInc;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
    }
}

// ===== SECTION: PhraseSequencer =====

void PhraseSequencer::prepare (double fs)
{
    sampleRateInt = std::llround (fs);
    jassert (boundaryOf (1) >= 1 && boundaryOf (2) - boundaryOf (1) >= 1); // ステップ長が1サンプル以上（next()は1回に1ステップだけ進める）
    reset();
}

long long PhraseSequencer::boundaryOf (long long step) const noexcept
{
    constexpr long long denominator = (long long) kBpm * kStepsPerBeat;
    return (step * 60 * sampleRateInt + denominator - 1) / denominator;
}

void PhraseSequencer::reset() noexcept
{
    clock = 0;
    stepIndex = 0;
    nextBoundary = boundaryOf (1);
    hz = midiToHz (kMidiNotes[0]);
}

float PhraseSequencer::midiToHz (int midiNote) noexcept
{
    return 440.0f * std::exp2 (((float) midiNote - 69.0f) / 12.0f);
}

float PhraseSequencer::next() noexcept
{
    if (clock >= nextBoundary)
    {
        ++stepIndex;
        nextBoundary = boundaryOf (stepIndex + 1);
        hz = midiToHz (kMidiNotes[(size_t) (stepIndex % kNumSteps)]);
    }

    ++clock;
    return hz;
}

// ===== SECTION: Talkbox =====

void Talkbox::prepare (double fs, int maxBlockSamples)
{
    sampleRate = fs;
    carrierLowScratch.assign ((size_t) maxBlockSamples, 0.0f);
    carrierHighScratch.assign ((size_t) maxBlockSamples, 0.0f);
    modulatorScratch.assign ((size_t) maxBlockSamples, 0.0f);
    sequencer.prepare (fs);

    for (size_t k = 0; k < detuneRatio.size(); ++k)
        detuneRatio[k] = std::exp2 (kTalkboxDetuneCents[k] / 1200.0f);

    const double high = std::min (kTalkboxHighHz, fs * 0.4);
    const double ratio = std::pow (high / kTalkboxLowHz, 1.0 / (double) (kNumBands - 1));

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto centre = (float) (kTalkboxLowHz * std::pow (ratio, (double) b));

        for (int stage = 0; stage < 2; ++stage)
        {
            bands[(size_t) b].mod[(size_t) stage].coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, centre, kTalkboxBandQ);
            bands[(size_t) b].carrier[(size_t) stage].coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, centre, kTalkboxCarrierBandQ);
        }

        bands[(size_t) b].useHighCarrier = (double) centre >= kTalkboxConsonantSplitHz;
    }

    attackCoeff = (float) onePoleCoeff (kTalkboxAttackSeconds, fs);
    releaseCoeff = (float) onePoleCoeff (kTalkboxReleaseSeconds, fs);

    voicingLow.reset (fs, kSmoothSeconds);
    voicingLow.setCurrentAndTargetValue (0.0f);
    voicingHigh.reset (fs, kSmoothSeconds);
    voicingHigh.setCurrentAndTargetValue (0.0f);

    reset();
}

void Talkbox::reset() noexcept
{
    for (auto& band : bands)
    {
        for (auto& f : band.mod)
            f.reset();
        for (auto& f : band.carrier)
            f.reset();
        band.envelope = 0.0f;
        band.carrierEnvelope = kTalkboxCarrierLevelInit;
    }

    phase = { 0.0, 0.0 };
    sequencer.reset();
}

void Talkbox::process (const float* modulator, float* out, int n, float pitchRatio, TalkboxRange range, float voicing) noexcept
{
    const float fMax = (float) (sampleRate * 0.25);
    const float scale = pitchRatio * (range == TalkboxRange::High ? 2.0f : 1.0f);

    // 有声度: 低域・中域のキャリアは下限適用後、2.5kHz以上は下限適用後から検出器の値へ子音ぶん近づけた値。
    const float raw = juce::jlimit (0.0f, 1.0f, voicing);
    const float floored = std::max (raw, kTalkboxVoicingFloor);
    voicingLow.setTargetValue (floored);
    voicingHigh.setTargetValue (juce::jlimit (0.0f, 1.0f, floored + kTalkboxConsonant * (raw - floored)));

    // キャリア（PolyBLEP鋸波2本の和と白色雑音を有声度で混合）。低域・中域用と、2.5kHz以上（子音）用の2本を作る。
    float* carrierLow = carrierLowScratch.data();
    float* carrierHigh = carrierHighScratch.data();

    for (int i = 0; i < n; ++i)
    {
        const float base = sequencer.next() * scale;
        const float vl = voicingLow.getNextValue();
        const float vh = voicingHigh.getNextValue();
        float sum = 0.0f;

        for (size_t k = 0; k < phase.size(); ++k)
        {
            const float dt = juce::jlimit (kTalkboxMinCarrierHz, fMax, base * detuneRatio[k]) / (float) sampleRate;
            const float t = (float) phase[k];
            sum += 2.0f * t - 1.0f - polyBlep (t, dt);

            phase[k] += (double) dt;
            if (phase[k] >= 1.0)
                phase[k] -= 1.0;
        }

        sum *= kTalkboxSawNorm;
        const float noise = rng.nextFloat() * 2.0f - 1.0f;
        carrierLow[i] = vl * sum + (1.0f - vl) * noise;
        carrierHigh[i] = vh * sum + (1.0f - vh) * noise;
    }

    // modulator == outでも壊れないよう、入力をコピーしてから出力を積算する。
    float* mod = modulatorScratch.data();
    std::copy (modulator, modulator + n, mod);
    std::fill (out, out + n, 0.0f);

    for (auto& band : bands)
    {
        const float* carrier = band.useHighCarrier ? carrierHigh : carrierLow;
        float env = band.envelope;
        float carrierEnv = band.carrierEnvelope;

        for (int i = 0; i < n; ++i)
        {
            const float m = band.mod[1].processSample (band.mod[0].processSample (mod[i]));
            const float rect = std::abs (m);
            const float coeff = rect > env ? attackCoeff : releaseCoeff;
            env = coeff * env + (1.0f - coeff) * rect;

            const float c = band.carrier[1].processSample (band.carrier[0].processSample (carrier[i]));
            const float cRect = std::abs (c);
            const float cCoeff = cRect > carrierEnv ? attackCoeff : releaseCoeff;
            carrierEnv = cCoeff * carrierEnv + (1.0f - cCoeff) * cRect;
            out[i] += env * c / std::max (carrierEnv, kTalkboxCarrierLevelFloor);
        }

        band.envelope = env;
        band.carrierEnvelope = carrierEnv;
    }

    juce::FloatVectorOperations::multiply (out, kOutputGain[(size_t) range], n);
}

} // namespace vc
