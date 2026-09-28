#include <juce_core/juce_core.h>

#include "core/ConnectionMonitor.h"
#include "core/Params.h"
#include "core/StatsLog.h"

#include <cmath>
#include <limits>

// ===== SECTION: AppLogicTests =====
// カテゴリAppLogic（quick）。docs/plan.md 3章T-006「テスト AppLogic（設定）」・
// T-007「テスト ConnectionMonitor・StatsLog」参照。
// sanitize()・presetFromId()・ConnectionMonitor・StatsLogはvc_core（GUI・デバイス非依存）にあるため、
// ここで直接検証できる。

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
        runConnectionMonitorTests();
        runStatsLogTests();
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

    // docs/plan.md 3章T-007「ConnectionMonitor: カウンタが2.0秒止まる→CloseAndFail、1.9秒→None。
    // 異常状態でTryReopenがt=+2,+4,+6秒。一覧変更通知あり・デバイスあり→即TryReopen。
    // エラーフラグ→即CloseAndFail。再オープン要求→即CloseAndFailの後TryReopen。」
    void runConnectionMonitorTests()
    {
        using Action = vc::ConnectionMonitor::Action;

        beginTest ("ConnectionMonitor: カウンタが2.0秒止まるとCloseAndFail、1.9秒ではNone");
        {
            vc::ConnectionMonitor mon;
            // 初回呼び出しは基準を記録するだけ(比較対象がまだない)。
            expect (mon.update (0, 0, 0, false, false, true, false) == Action::None);
            expect (! mon.isFailed());

            expect (mon.update (1900, 0, 0, false, false, true, false) == Action::None);
            expect (! mon.isFailed());

            expect (mon.update (2000, 0, 0, false, false, true, false) == Action::CloseAndFail);
            expect (mon.isFailed());
        }

        beginTest ("ConnectionMonitor: カウンタが進み続けていれば異常にならない");
        {
            vc::ConnectionMonitor mon;
            expect (mon.update (0, 0, 0, false, false, true, false) == Action::None);

            juce::uint64 in = 0, out = 0;
            for (juce::int64 t = 100; t <= 5000; t += 100)
            {
                ++in; ++out;
                expect (mon.update (t, in, out, false, false, true, false) == Action::None);
            }

            expect (! mon.isFailed());
        }

        beginTest ("ConnectionMonitor: 異常状態でのTryReopenはt=+2,+4,+6秒(2秒ごと)");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false); // 初回
            expect (mon.update (2000, 0, 0, false, false, true, false) == Action::CloseAndFail); // 起点t0=2000

            expect (mon.update (3000, 0, 0, false, false, false, false) == Action::None); // +1秒: まだ
            expect (mon.update (4000, 0, 0, false, false, false, false) == Action::TryReopen); // +2秒
            expect (mon.update (5000, 0, 0, false, false, false, false) == Action::None);
            expect (mon.update (6000, 0, 0, false, false, false, false) == Action::TryReopen); // +4秒
            expect (mon.update (7000, 0, 0, false, false, false, false) == Action::None);
            expect (mon.update (8000, 0, 0, false, false, false, false) == Action::TryReopen); // +6秒
        }

        beginTest ("ConnectionMonitor: 一覧変更通知あり・デバイスありなら即TryReopen");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            mon.update (2000, 0, 0, false, false, true, false); // CloseAndFail(次回予定はt=4000)

            // まだ2秒経っていない(t=2100)が、一覧変更通知でデバイスが戻っていれば即TryReopen。
            expect (mon.update (2100, 0, 0, false, false, true, true) == Action::TryReopen);
        }

        beginTest ("ConnectionMonitor: エラーフラグは即CloseAndFail");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            expect (mon.update (100, 0, 0, true, false, true, false) == Action::CloseAndFail);
            expect (mon.isFailed());
        }

        beginTest ("ConnectionMonitor: 再オープン要求は即CloseAndFail。その後はスケジュールどおりTryReopen");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            expect (mon.update (100, 0, 0, false, true, true, false) == Action::CloseAndFail);
            expect (mon.isFailed());

            expect (mon.update (2000, 0, 0, false, false, false, false) == Action::None);     // 100+2000=2100未満
            expect (mon.update (2100, 0, 0, false, false, false, false) == Action::TryReopen);
        }
    }

    // docs/plan.md 3章T-007「StatsLog: 整形した行が全項目...を含む。1MB超の一時ファイル→
    // 作り直し後のサイズ0。1MB以下→変化なし。」
    void runStatsLogTests()
    {
        beginTest ("StatsLog: formatStatsLineが全項目(経過時間・レイテンシ・充填量・アンダーラン/オーバーラン・速度比補正・CPU・メモリ)を含む");
        {
            vc::StatsSnapshot s;
            s.elapsedSeconds = 3661.0;
            s.latencyMs = 45.2;
            s.fillMs = 12.3;
            s.underruns = 3;
            s.overruns = 1;
            s.speedCorrectionPpm = -15.5;
            s.cpuPercent = 0.8;
            s.memoryBytes = (std::int64_t) 42 * 1024 * 1024;

            const auto line = vc::formatStatsLine (s);

            expect (line.contains ("3661"));        // 経過時間
            expect (line.contains ("45.2"));        // レイテンシ
            expect (line.contains ("12.3"));        // 充填量
            expect (line.contains ("underruns=3"));
            expect (line.contains ("overruns=1"));
            expect (line.contains ("-15.5"));       // 速度比補正
            expect (line.contains ("0.8"));         // CPU使用率
            expect (line.contains ("42.0"));        // メモリ(MB)
        }

        beginTest ("StatsLog: 速度比補正が正のときは符号を付ける");
        {
            vc::StatsSnapshot s;
            s.speedCorrectionPpm = 12.0;
            expect (vc::formatStatsLine (s).contains ("+12.0"));
        }

        beginTest ("StatsLog: resetIfLarger 1MB超の一時ファイルは作り直し後にサイズ0");
        {
            juce::File tmp = juce::File::createTempFile ("statslog_big");
            const juce::String big = juce::String::repeatedString ("0123456789", 110000); // 約1.1MB
            tmp.replaceWithText (big);
            expect (tmp.getSize() > 1024 * 1024);

            vc::resetIfLarger (tmp, 1024 * 1024);
            expect (tmp.getSize() == 0);

            tmp.deleteFile();
        }

        beginTest ("StatsLog: resetIfLarger 1MB以下は変化なし");
        {
            juce::File tmp = juce::File::createTempFile ("statslog_small");
            const juce::String content = "hello";
            tmp.replaceWithText (content);

            vc::resetIfLarger (tmp, 1024 * 1024);

            expect (tmp.existsAsFile());
            expect (tmp.loadFileAsString() == content);

            tmp.deleteFile();
        }
    }
};

static AppLogicTests appLogicTests;

} // namespace
