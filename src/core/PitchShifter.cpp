#include "PitchShifter.h"

#include <cstring>

namespace vc
{

namespace
{
constexpr double kFadeSeconds = 0.020;
} // namespace

PitchShifter::BlockDecision PitchShifter::decideStretchBlock (double deviceLatencyMs, double ringTargetMs) noexcept
{
    const double budget = 50.0 - deviceLatencyMs - ringTargetMs - 2.0;
    const bool overBudget = budget < 20.0;
    const double clamped = juce::jlimit (20.0, 40.0, budget);
    return { (int) std::lround (clamped), overBudget };
}

void PitchShifter::prepare (double newSampleRate, int newMaxBlockSamples, int stretchBlockSamples)
{
    jassert (newSampleRate > 0.0 && newMaxBlockSamples > 0 && stretchBlockSamples > 0);

    sampleRate = newSampleRate;
    maxBlockSamples = newMaxBlockSamples;

    stretch.configure (1, stretchBlockSamples, juce::jmax (1, stretchBlockSamples / 4));

    wetScratch.assign ((size_t) maxBlockSamples, 0.0f);

    fadeLenSamples = juce::jmax (1, (int) std::lround (kFadeSeconds * sampleRate));
    requiredPrimeSamples = stretch.inputLatency() + stretch.outputLatency();

    // ponytail: Stretchのpeaks/Temporaryはprocess()内で稀に確保することがある
    // (docs/plan.md 1章5・6章)。実運用で使う移調・フォルマントの組み合わせを一通り
    // 空回しし、必要な容量を先に確保させる。音声スレッドでは呼ばない(prepare()のみ)。
    warmUp();

    reset();
}

void PitchShifter::warmUp()
{
    juce::Random rng (12345);
    std::vector<float> noise ((size_t) maxBlockSamples, 0.0f);
    std::vector<float> scratchOut ((size_t) maxBlockSamples, 0.0f);

    // Engineで実際に使う範囲(層1ピッチ-12〜+12 + プリセットの移調-6〜+8)を広めにカバーする。
    constexpr float semitoneSet[] = { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 20.0f };
    constexpr float formantSet[] = { 0.75f, 1.0f, 1.4f, 1.6f };

    for (const float semis : semitoneSet)
    {
        for (const float formant : formantSet)
        {
            stretch.setTransposeSemitones (semis);
            stretch.setFormantFactor (formant, true);

            for (int block = 0; block < 3; ++block)
            {
                for (int i = 0; i < maxBlockSamples; ++i)
                    noise[(size_t) i] = rng.nextFloat() * 0.2f - 0.1f;

                const float* inCh[1] = { noise.data() };
                float* outCh[1] = { scratchOut.data() };
                stretch.process (inCh, maxBlockSamples, outCh, maxBlockSamples);
            }
        }
    }
}

void PitchShifter::setTarget (bool run, float semitones, float formantFactor) noexcept
{
    runTarget = run;
    targetSemitones = semitones;
    targetFormant = formantFactor;
}

void PitchShifter::reset() noexcept
{
    stretch.reset();
    state = State::Resting;
    gain = 0.0;
    primedSamples = 0;
}

int PitchShifter::getLatencySamples() const noexcept
{
    if (state == State::FadingIn || state == State::Active || state == State::FadingOut)
        return stretch.inputLatency() + stretch.outputLatency();

    return 0;
}

void PitchShifter::process (const float* in, float* out, int n) noexcept
{
    jassert (n <= maxBlockSamples);

    // ----- 状態遷移（本ブロックの振る舞いを決める前に確定させる） -----
    switch (state)
    {
        case State::Resting:
            if (runTarget)
            {
                stretch.reset();
                primedSamples = 0;
                state = State::Priming;
            }
            break;

        case State::Priming:
            if (! runTarget)
                state = State::Resting;
            break;

        case State::FadingIn:
            if (! runTarget)
                state = State::FadingOut;
            break;

        case State::Active:
            if (! runTarget)
                state = State::FadingOut;
            break;

        case State::FadingOut:
            if (runTarget)
                state = State::FadingIn;
            break;
    }

    if (state != State::Resting)
    {
        stretch.setTransposeSemitones (targetSemitones);
        stretch.setFormantFactor (targetFormant, true);
    }

    // ----- 本ブロックの処理 -----
    switch (state)
    {
        case State::Resting:
        {
            if (out != in)
                std::memcpy (out, in, sizeof (float) * (size_t) n);
            break;
        }

        case State::Priming:
        {
            const float* inCh[1] = { in };
            float* outCh[1] = { wetScratch.data() };
            stretch.process (inCh, n, outCh, n); // 出力は使わず捨てる(送り込みだけ進める)

            if (out != in)
                std::memcpy (out, in, sizeof (float) * (size_t) n);

            primedSamples += n;

            if (primedSamples >= requiredPrimeSamples)
                state = State::FadingIn;

            break;
        }

        case State::FadingIn:
        case State::Active:
        case State::FadingOut:
        {
            const float* inCh[1] = { in };
            float* outCh[1] = { wetScratch.data() };
            stretch.process (inCh, n, outCh, n);

            if (state == State::Active)
            {
                std::memcpy (out, wetScratch.data(), sizeof (float) * (size_t) n);
            }
            else
            {
                const bool ascending = (state == State::FadingIn);
                const double step = 1.0 / (double) fadeLenSamples;

                for (int i = 0; i < n; ++i)
                {
                    out[i] = (float) (in[i] * (1.0 - gain) + (double) wetScratch[(size_t) i] * gain);
                    gain += ascending ? step : -step;
                    gain = juce::jlimit (0.0, 1.0, gain);
                }

                if (ascending && gain >= 1.0)
                    state = State::Active;
                else if ((! ascending) && gain <= 0.0)
                    state = State::Resting;
            }

            break;
        }
    }
}

} // namespace vc
