#include <juce_core/juce_core.h>

#include "core/Params.h"

#include <cmath>
#include <limits>

// ===== SECTION: AppLogicTests =====
// カテゴリAppLogic（quick）。docs/plan.md 3章T-006「テスト AppLogic（設定）」参照。
// sanitize()・presetFromId()はvc_core（GUI・デバイス非依存）にあるため、ここで直接検証できる。

namespace
{

class AppLogicTests final : public juce::UnitTest
{
public:
    AppLogicTests() : juce::UnitTest ("AppLogic", "AppLogic") {}

    void runTest() override
    {
        runSanitizeTests();
        runPresetFromIdTests();
    }

private:
    void runSanitizeTests()
    {
        beginTest ("sanitize: 範囲内の値はそのまま");
        {
            vc::SavedSettings s;
            s.gainDb = 3.0f;
            s.pitch = -5;
            s.reverb = 0.4f;

            const auto out = vc::sanitize (s);
            expectEquals (out.gainDb, 3.0f);
            expectEquals (out.pitch, -5);
            expectEquals (out.reverb, 0.4f);
        }

        beginTest ("sanitize: ゲイン±100dBは±20dBへ丸められる");
        {
            vc::SavedSettings s;
            s.gainDb = 100.0f;
            expectEquals (vc::sanitize (s).gainDb, 20.0f);

            s.gainDb = -100.0f;
            expectEquals (vc::sanitize (s).gainDb, -20.0f);
        }

        beginTest ("sanitize: ピッチ±50は±12半音へ丸められ、整数のまま");
        {
            vc::SavedSettings s;
            s.pitch = 50;
            expectEquals (vc::sanitize (s).pitch, 12);

            s.pitch = -50;
            expectEquals (vc::sanitize (s).pitch, -12);
        }

        beginTest ("sanitize: リバーブ2.0/-1は0〜1の端へ丸められる");
        {
            vc::SavedSettings s;
            s.reverb = 2.0f;
            expectEquals (vc::sanitize (s).reverb, 1.0f);

            s.reverb = -1.0f;
            expectEquals (vc::sanitize (s).reverb, 0.0f);
        }

        beginTest ("sanitize: gainDb/reverbがNaN/Infなら初期値へ丸められる（レビュー指摘2）");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const float inf = std::numeric_limits<float>::infinity();

            vc::SavedSettings s;
            s.gainDb = nan;
            s.reverb = 0.4f;
            auto out = vc::sanitize (s);
            expect (std::isfinite (out.gainDb), "gainDb=NaNが有限値に丸められていない");
            expectEquals (out.gainDb, 0.0f);

            s = vc::SavedSettings();
            s.gainDb = inf;
            out = vc::sanitize (s);
            expect (std::isfinite (out.gainDb), "gainDb=+Infが有限値に丸められていない");
            expectEquals (out.gainDb, 0.0f);

            s = vc::SavedSettings();
            s.reverb = -inf;
            out = vc::sanitize (s);
            expect (std::isfinite (out.reverb), "reverb=-Infが有限値に丸められていない");
            expectEquals (out.reverb, 0.0f);

            s = vc::SavedSettings();
            s.gainDb = nan;
            s.reverb = nan;
            out = vc::sanitize (s);
            expect (std::isfinite (out.gainDb) && std::isfinite (out.reverb),
                    "gainDb/reverbが同時にNaNのとき有限値に丸められていない");
        }

        beginTest ("sanitize: キーがない場合(初期値のSavedSettings)は初期値のまま");
        {
            const vc::SavedSettings defaults;
            const auto out = vc::sanitize (defaults);
            expectEquals (out.gainDb, 0.0f);
            expectEquals (out.pitch, 0);
            expectEquals (out.reverb, 0.0f);
            expect (out.preset == vc::Preset::Normal);
            expect (out.enabled == true);
            expect (out.trayNoticeShown == false);
        }
    }

    void runPresetFromIdTests()
    {
        beginTest ("presetFromId: 既知のidは対応するPresetを返す");
        {
            expect (vc::presetFromId ("normal") == vc::Preset::Normal);
            expect (vc::presetFromId ("echo") == vc::Preset::Echo);
            expect (vc::presetFromId ("helium") == vc::Preset::Helium);
            expect (vc::presetFromId ("minion") == vc::Preset::Minion);
            expect (vc::presetFromId ("giant") == vc::Preset::Giant);
            expect (vc::presetFromId ("kerokero") == vc::Preset::Kerokero);
            expect (vc::presetFromId ("robot") == vc::Preset::Robot);
            expect (vc::presetFromId ("talkbox") == vc::Preset::Talkbox);
        }

        beginTest ("presetFromId: 大文字小文字は区別しない");
        {
            expect (vc::presetFromId ("ECHO") == vc::Preset::Echo);
            expect (vc::presetFromId ("Talkbox") == vc::Preset::Talkbox);
        }

        beginTest ("presetFromId: 不明なプリセット名はNormalになる");
        {
            expect (vc::presetFromId ("unknown-preset") == vc::Preset::Normal);
            expect (vc::presetFromId ({}) == vc::Preset::Normal);
        }
    }
};

static AppLogicTests appLogicTests;

} // namespace
