#include <juce_core/juce_core.h>

#include "AllocationGuard.h"
#include "TestSignals.h"
#include "core/Engine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// ===== SECTION: EngineTests =====
// E1〜E7（カテゴリEngine、quick）。docs/plan.md 3章T-004参照。

namespace
{

constexpr double kFs = 48000.0;
constexpr int kMaxBlock = 512;
constexpr int kStretchBlock = 960; // 20ms

class EngineTests final : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("Engine", "Engine") {}

    void runTest() override
    {
        runE1();
        runE2();
        runE3();
        runE4();
        runE5();
        runE6();
        runE7();
    }

private:
    // Engineはstd::atomicメンバを持つためコピー・ムーブ不可。呼び出し側で構築し、これで準備する。
    static void prepareEngine (vc::Engine& engine)
    {
        engine.prepare ({ kFs, kMaxBlock, kStretchBlock });
    }

    // ----- E1: ゲイン -----
    void runE1()
    {
        beginTest ("E1: ゲイン+6dBで補間後RMSが+6dB±0.1dB、クリックなし");

        vc::Engine engine;
        prepareEngine (engine);

        const float amp = (float) std::pow (10.0, -20.0 / 20.0); // -20dBFS
        constexpr int steadyLen = (int) kFs; // 1秒
        constexpr int rampSamples = (int) 0.05 * (int) kFs; // 50ms(参考、実際はlround使用)
        const int actualRamp = (int) std::lround (0.05 * kFs);
        constexpr int margin = 2400;
        const int totalLen = steadyLen + actualRamp + margin + steadyLen;

        auto signal = vc::test::makeSine (1000.0, kFs, totalLen, amp);
        std::vector<float> out (signal);

        engine.process (out.data(), steadyLen);
        const int transitionStart = steadyLen;

        engine.params().gainDb.store (6.0f);

        const int transitionEnd = transitionStart + actualRamp + margin;
        engine.process (out.data() + transitionStart, transitionEnd - transitionStart);
        engine.process (out.data() + transitionEnd, steadyLen);

        const bool noClick = vc::test::checkNoClick (out.data() + (transitionStart - steadyLen), steadyLen,
                                                       out.data() + transitionStart, transitionEnd - transitionStart,
                                                       out.data() + transitionEnd);
        expect (noClick, "click detected on gain change");

        const double rmsBefore = vc::test::rms (out.data() + (transitionStart - steadyLen), steadyLen);
        const double rmsAfter = vc::test::rms (out.data() + transitionEnd, steadyLen);
        const double ratioDb = 20.0 * std::log10 (rmsAfter / rmsBefore);

        expectWithinAbsoluteError (ratioDb, 6.0, 0.1);
        juce::ignoreUnused (rampSamples);
    }

    // ----- E2: リミッター -----
    void runE2()
    {
        beginTest ("E2a: ゲイン+20dBの0dBFS級正弦がすべて|x|<=1.0");

        {
            vc::Engine engine;
            prepareEngine (engine);
            engine.params().gainDb.store (20.0f);

            const int totalLen = (int) kFs * 2;
            auto signal = vc::test::makeSine (1000.0, kFs, totalLen, 0.9f);
            std::vector<float> out (signal);

            engine.process (out.data(), (int) out.size());

            const int guard = (int) std::lround (0.05 * kFs) + 2400;
            const double peak = vc::test::peakAbs (out.data() + guard, (int) out.size() - guard);
            expect (peak <= 1.0 + 1.0e-6, "peak " + juce::String (peak) + " exceeds 1.0");
        }

        beginTest ("E2b: -20dBFS・ゲイン0dBの透過性(RMS変化<=0.1dB)");

        {
            vc::Engine engine;
            prepareEngine (engine);

            const float amp = (float) std::pow (10.0, -20.0 / 20.0);
            const int totalLen = (int) kFs * 2;
            auto signal = vc::test::makeSine (1000.0, kFs, totalLen, amp);
            std::vector<float> out (signal);

            engine.process (out.data(), (int) out.size());

            const int guard = 4800;
            const double inRms = vc::test::rms (signal.data() + guard, (int) signal.size() - guard);
            const double outRms = vc::test::rms (out.data() + guard, (int) out.size() - guard);
            const double ratioDb = 20.0 * std::log10 (outRms / inRms);

            expectWithinAbsoluteError (ratioDb, 0.0, 0.1);
        }
    }

    // ----- E3: リバーブ -----
    void runE3()
    {
        beginTest ("E3a: r:0→0.01でRMS変化<=0.5dB");
        {
            vc::Engine engine;
            prepareEngine (engine);

            const int steadyLen = (int) kFs;
            auto signalBefore = vc::test::makeSine (300.0, kFs, steadyLen, 0.4f);
            std::vector<float> outBefore (signalBefore);
            engine.process (outBefore.data(), (int) outBefore.size());
            const double rmsBefore = vc::test::rms (outBefore.data() + steadyLen / 2, steadyLen / 2);

            engine.params().reverb.store (0.01f);

            const int rampSamples = (int) std::lround (0.020 * kFs);
            const int margin = 2400;
            const int totalAfter = rampSamples + margin + steadyLen;
            auto signalAfter = vc::test::makeSine (300.0, kFs, totalAfter, 0.4f);
            std::vector<float> outAfter (signalAfter);
            engine.process (outAfter.data(), (int) outAfter.size());

            const double rmsAfter = vc::test::rms (outAfter.data() + rampSamples + margin, steadyLen);
            const double ratioDb = 20.0 * std::log10 (rmsAfter / rmsBefore);

            expectWithinAbsoluteError (ratioDb, 0.0, 0.5);
        }

        beginTest ("E3b: r=0.5で入力停止後200msのRMSが入力比-40dB超");
        {
            vc::Engine engine;
            prepareEngine (engine);
            engine.params().reverb.store (0.5f);

            const int fillLen = (int) kFs * 2;
            auto signal = vc::test::makeSine (300.0, kFs, fillLen, 0.5f);
            std::vector<float> out (signal);
            engine.process (out.data(), (int) out.size());

            const double inRms = vc::test::rms (signal.data() + (int) kFs, (int) kFs);

            const int silenceLen = (int) (kFs * 0.3);
            std::vector<float> tail (silenceLen, 0.0f);
            engine.process (tail.data(), silenceLen);

            const int winStart = (int) (kFs * 0.19);
            const int winLen = (int) (kFs * 0.02);
            const double tailRms = vc::test::rms (tail.data() + winStart, winLen);

            const double ratioDb = 20.0 * std::log10 (tailRms / inRms);
            expect (ratioDb > -40.0, "tail decayed too much: " + juce::String (ratioDb) + "dB");
        }

        beginTest ("E3c: r→0の補間完了後はリバーブ停止(残留しない)");
        {
            vc::Engine engine;
            prepareEngine (engine);
            engine.params().reverb.store (0.5f);

            auto signal = vc::test::makeSine (300.0, kFs, (int) kFs, 0.5f);
            std::vector<float> out (signal);
            engine.process (out.data(), (int) out.size());

            engine.params().reverb.store (0.0f);

            const int rampSamples = (int) std::lround (0.020 * kFs);
            const int margin = 4800;
            auto transitionSignal = vc::test::makeSine (300.0, kFs, rampSamples + margin, 0.5f);
            engine.process (transitionSignal.data(), (int) transitionSignal.size());

            const int silenceLen = (int) (kFs * 0.2);
            std::vector<float> silence (silenceLen, 0.0f);
            engine.process (silence.data(), silenceLen);

            const double peak = vc::test::peakAbs (silence.data(), silenceLen);
            expect (peak <= 1.0e-6, "residual output " + juce::String (peak) + " after reverb should have stopped");
        }
    }

    // ----- E4: バイパス -----
    void runE4()
    {
        beginTest ("E4a: 完全バイパス中は入力とビット一致");
        {
            vc::Engine engine;
            engine.params().enabled.store (false); // prepare前に設定 → chainGainが0から始まる
            engine.prepare ({ kFs, kMaxBlock, kStretchBlock });

            auto signal = vc::test::makeSine (300.0, kFs, (int) kFs, 0.4f);
            std::vector<float> out (signal);
            engine.process (out.data(), (int) out.size());

            bool bitExact = true;
            for (size_t i = 0; i < signal.size(); ++i)
            {
                if (out[i] != signal[i])
                {
                    bitExact = false;
                    break;
                }
            }
            expect (bitExact, "bypassed output did not match input bit-for-bit");
        }

        beginTest ("E4b: ON/OFF切替でクリックなし");
        {
            vc::Engine engine;
            prepareEngine (engine); // enabled default true

            const int steadyLen = (int) kFs;
            const int rampSamples = (int) std::lround (0.020 * kFs);
            const int margin = 2400;
            const int totalLen = steadyLen + rampSamples + margin + steadyLen;

            auto signal = vc::test::makeSine (300.0, kFs, totalLen, 0.4f);
            std::vector<float> out (signal);

            engine.process (out.data(), steadyLen);
            const int transitionStart = steadyLen;

            engine.params().enabled.store (false);

            const int transitionEnd = transitionStart + rampSamples + margin;
            engine.process (out.data() + transitionStart, transitionEnd - transitionStart);
            engine.process (out.data() + transitionEnd, steadyLen);

            const bool noClick = vc::test::checkNoClick (out.data() + (transitionStart - steadyLen), steadyLen,
                                                           out.data() + transitionStart, transitionEnd - transitionStart,
                                                           out.data() + transitionEnd);
            expect (noClick, "click detected on bypass toggle");
        }
    }

    // ----- E5: NaN/Inf注入 -----
    void runE5()
    {
        runE5Case (std::numeric_limits<float>::quiet_NaN(), "NaN");
        runE5Case (std::numeric_limits<float>::infinity(), "Inf");

        beginTest ("E5c: バイパス中のNaN注入で出力0・フラグ");
        {
            vc::Engine engine;
            engine.params().enabled.store (false);
            engine.prepare ({ kFs, kMaxBlock, kStretchBlock });

            std::vector<float> block ((size_t) kMaxBlock, 0.3f);
            block[5] = std::numeric_limits<float>::quiet_NaN();
            engine.process (block.data(), (int) block.size());

            expect (vc::test::peakAbs (block.data(), (int) block.size()) == 0.0, "block was not silenced");
            expect ((engine.getErrorFlags() & 0x1u) != 0, "bit0 should be set");
        }
    }

    void runE5Case (float badValue, const juce::String& label)
    {
        beginTest ("E5: " + label + "注入(ミニオン稼働中)からの復帰");

        vc::Engine engine;
        prepareEngine (engine);
        engine.params().preset.store ((int) vc::Preset::Minion);
        engine.params().pitch.store (0);

        auto lead = vc::test::makeSine (220.0, kFs, (int) kFs * 2, 0.3f);
        std::vector<float> leadOut (lead);
        engine.process (leadOut.data(), (int) leadOut.size());

        const int expectedL = engine.getShifterLatencySamples();
        expect (expectedL > 0, "shifter should be active for Minion preset");

        std::vector<float> badBlock ((size_t) kMaxBlock, 0.3f);
        badBlock[10] = badValue;
        engine.process (badBlock.data(), (int) badBlock.size());

        expect (vc::test::peakAbs (badBlock.data(), (int) badBlock.size()) == 0.0, "block was not silenced");
        expect ((engine.getErrorFlags() & 0x1u) != 0, "bit0 should be set");
        engine.clearErrorFlags();

        const int normalLen = expectedL + kMaxBlock * 4 + 4000;
        auto normal = vc::test::makeSine (220.0, kFs, normalLen, 0.3f);

        int firstNonZeroAt = -1;
        int pos = 0;

        while (pos + kMaxBlock <= (int) normal.size())
        {
            engine.process (normal.data() + pos, kMaxBlock);
            pos += kMaxBlock;

            if (firstNonZeroAt < 0 && vc::test::peakAbs (normal.data() + pos - kMaxBlock, kMaxBlock) > 1.0e-6)
                firstNonZeroAt = pos;
        }

        expect (firstNonZeroAt > 0, "did not recover to non-zero output");
        expect (firstNonZeroAt <= expectedL + 2 * kMaxBlock,
                "recovery took too long: " + juce::String (firstNonZeroAt) + " > " + juce::String (expectedL + 2 * kMaxBlock));

        expect (vc::test::allFinite (normal.data(), (int) normal.size()), "non-finite values found after recovery");
    }

    // ----- E6: 分割処理 -----
    void runE6()
    {
        beginTest ("E6: n=3*maxBlock+17の一括処理とmaxBlockごとの処理が一致");

        vc::Engine engineA;
        vc::Engine engineB;
        prepareEngine (engineA);
        prepareEngine (engineB);

        engineA.params().preset.store ((int) vc::Preset::Minion);
        engineB.params().preset.store ((int) vc::Preset::Minion);

        const int n = 3 * kMaxBlock + 17;
        auto signal = vc::test::makeSine (220.0, kFs, n, 0.3f);

        std::vector<float> bufA (signal), bufB (signal);

        engineA.process (bufA.data(), n);

        int pos = 0;
        while (pos < n)
        {
            const int chunk = std::min (kMaxBlock, n - pos);
            engineB.process (bufB.data() + pos, chunk);
            pos += chunk;
        }

        bool bitExact = true;
        for (int i = 0; i < n; ++i)
        {
            if (bufA[(size_t) i] != bufB[(size_t) i])
            {
                bitExact = false;
                break;
            }
        }

        expect (bitExact, "one-shot and chunked processing did not match bit-for-bit");
    }

    // ----- E7: アロケーション -----
    void runE7()
    {
        beginTest ("E7: 全プリセット・全遷移でのアロケーション0回");

        vc::Engine engine;
        prepareEngine (engine);
        engine.params().reverb.store (0.3f); // リバーブの実処理経路も対象に含める

        auto signal = vc::test::makeSine (220.0, kFs, kMaxBlock * 50, 0.3f);
        std::vector<float> buf (signal);

        // ウォームアップ: 各プリセット・ピッチ・バイパスを一通り回し、遅延確保等を先に済ませる。
        for (int p = 0; p < (int) vc::kPresets.size(); ++p)
        {
            engine.params().preset.store (p);

            for (const int pitch : { -12, 0, 5, 12 })
            {
                engine.params().pitch.store (pitch);
                buf = signal;
                engine.process (buf.data(), (int) buf.size());
            }
        }

        engine.params().enabled.store (false);
        buf = signal;
        engine.process (buf.data(), (int) buf.size());
        engine.params().enabled.store (true);
        buf = signal;
        engine.process (buf.data(), (int) buf.size());

        // 本計測。
        std::size_t totalAllocations = 0;
        {
            vc::test::ScopedAllocationGuard guard;

            for (int p = 0; p < (int) vc::kPresets.size(); ++p)
            {
                engine.params().preset.store (p);

                for (const int pitch : { -12, 0, 5, 12 })
                {
                    engine.params().pitch.store (pitch);
                    engine.process (buf.data(), (int) buf.size());
                }
            }

            engine.params().enabled.store (false);
            engine.process (buf.data(), (int) buf.size());
            engine.params().enabled.store (true);
            engine.process (buf.data(), (int) buf.size());

            engine.params().reverb.store (0.0f);
            engine.process (buf.data(), (int) buf.size()); // リバーブ停止への遷移も含める

            totalAllocations = guard.count();
        }

        expect (totalAllocations == 0, "allocations detected during process(): " + juce::String ((int) totalAllocations));
    }
};

static EngineTests engineTests;

} // namespace
