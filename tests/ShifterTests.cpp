#include <juce_core/juce_core.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include "TestSignals.h"

// ===== SECTION: ShifterTests =====
// T-002時点ではSmokeカテゴリのみ。src/core/PitchShifterを介したP1〜P5はT-004で追加する
// （docs/plan.md 3章 T-004参照）。

namespace
{

class ShifterSmokeTests final : public juce::UnitTest
{
public:
    ShifterSmokeTests() : juce::UnitTest ("ShifterSmoke", "Smoke") {}

    void runTest() override
    {
        beginTest ("configure(1, 960, 240) at 48kHz reports ~960 samples of latency");
        {
            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.configure (1, 960, 240);

            const int latency = stretch.inputLatency() + stretch.outputLatency();
            expect (latency >= static_cast<int> (960 * 0.95) && latency <= static_cast<int> (960 * 1.05),
                    "latency " + juce::String (latency) + " out of expected range");
        }

        beginTest ("processing 1 second of a sine wave produces finite output");
        {
            constexpr double sampleRate = 48000.0;
            constexpr int blockSamples = 960;

            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.configure (1, blockSamples, blockSamples / 4);
            stretch.setTransposeSemitones (0.0f);
            stretch.setFormantFactor (1.0f, true);

            const int totalSamples = static_cast<int> (sampleRate); // 1秒分
            const auto input = vc::test::makeSine (220.0, sampleRate, totalSamples, 0.5f);
            std::vector<float> output (static_cast<size_t> (totalSamples), 0.0f);

            const int chunk = blockSamples / 4;

            for (int pos = 0; pos + chunk <= totalSamples; pos += chunk)
            {
                const float* inChannels[1] = { input.data() + pos };
                float* outChannels[1] = { output.data() + pos };
                stretch.process (inChannels, chunk, outChannels, chunk);
            }

            const bool allFinite = vc::test::allFinite (output.data(), totalSamples);
            expect (allFinite, "shifter output contained non-finite values");
        }
    }
};

static ShifterSmokeTests shifterSmokeTests;

} // namespace
