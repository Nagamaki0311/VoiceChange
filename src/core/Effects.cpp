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

// 出力音量を入力と揃える固定の補正ゲイン（X3で母音3種×f0 3種の中央値から決めた。docs/spec.mdに記録）。
constexpr float kTalkboxGain = 0.904f; // 中央値+0.88dBを打ち消す

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

// ===== SECTION: Talkbox =====

void Talkbox::prepare (double fs, int maxBlockSamples)
{
    sampleRate = fs;
    carrierScratch.assign ((size_t) maxBlockSamples, 0.0f);
    modulatorScratch.assign ((size_t) maxBlockSamples, 0.0f);

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
    }

    attackCoeff = (float) onePoleCoeff (kTalkboxAttackSeconds, fs);
    releaseCoeff = (float) onePoleCoeff (kTalkboxReleaseSeconds, fs);

    carrierFreq.reset (fs, kSmoothSeconds);
    carrierFreq.setCurrentAndTargetValue (110.0f);
    voicingSmoothed.reset (fs, kSmoothSeconds);
    voicingSmoothed.setCurrentAndTargetValue (0.0f);

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

    phase = 0.0;
}

void Talkbox::process (const float* modulator, float* out, int n, float carrierHz, float voicing) noexcept
{
    const float fMax = (float) (sampleRate * 0.25);
    carrierFreq.setTargetValue (juce::jlimit (kTalkboxMinCarrierHz, fMax, carrierHz));
    voicingSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, voicing));

    // キャリア（PolyBLEP鋸波と白色雑音を有声度で混合）。
    float* carrier = carrierScratch.data();
    for (int i = 0; i < n; ++i)
    {
        const float dt = carrierFreq.getNextValue() / (float) sampleRate;
        const float v = voicingSmoothed.getNextValue();
        const float t = (float) phase;

        const float saw = 2.0f * t - 1.0f - polyBlep (t, dt);
        const float noise = rng.nextFloat() * 2.0f - 1.0f;
        carrier[i] = v * saw + (1.0f - v) * noise;

        phase += (double) dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }

    // modulator == outでも壊れないよう、入力をコピーしてから出力を積算する。
    float* mod = modulatorScratch.data();
    std::copy (modulator, modulator + n, mod);
    std::fill (out, out + n, 0.0f);

    for (auto& band : bands)
    {
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

    juce::FloatVectorOperations::multiply (out, kTalkboxGain, n);
}

} // namespace vc
