#include <juce_core/juce_core.h>

#include "core/ConnectionMonitor.h"
#include "core/Params.h"
#include "core/StatsLog.h"

#include <cmath>
#include <limits>
#include <optional>

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
        runMicSettingsTests();
        runEqInputTests();
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

    // docs/plan.md 8.3 T-013「テスト AppLogic」: マイク処理（ノイズ除去・EQ）の設定の丸め・キーなし・往復。
    void runMicSettingsTests()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();

        beginTest ("MicSettings: 範囲外（背景2.0/-1、周波数5/50000、ゲイン±100、Q 0/100）は範囲の端へ丸められる");
        {
            vc::SavedSettings s;
            s.nrBackground = 2.0f;
            s.nrImpact = 5.0f;
            s.eqBands[0].hz = 5.0f;      s.eqBands[0].gainDb = 100.0f;  s.eqBands[0].q = 0.0f;
            s.eqBands[1].hz = 50000.0f;  s.eqBands[1].gainDb = -100.0f; s.eqBands[1].q = 100.0f;
            auto out = vc::sanitize (s);
            expectEquals (out.nrBackground, 1.0f);
            expectEquals (out.nrImpact, 1.0f);
            expectEquals (out.eqBands[0].hz, 20.0f);
            expectEquals (out.eqBands[0].gainDb, 18.0f);
            expectEquals (out.eqBands[0].q, 0.1f);
            expectEquals (out.eqBands[1].hz, 20000.0f);
            expectEquals (out.eqBands[1].gainDb, -18.0f);
            expectEquals (out.eqBands[1].q, 10.0f);

            s = vc::SavedSettings();
            s.nrBackground = -1.0f;
            s.nrImpact = -0.5f;
            out = vc::sanitize (s);
            expectEquals (out.nrBackground, 0.0f);
            expectEquals (out.nrImpact, 0.0f);
        }

        beginTest ("MicSettings: NaN/Infは、そのバンド・項目の初期値になる");
        {
            vc::SavedSettings s;
            s.nrBackground = nan;
            s.nrImpact = inf;
            s.eqBands[2].hz = nan;
            s.eqBands[2].gainDb = inf;
            s.eqBands[2].q = -inf;
            s.eqBands[4].hz = -inf;

            const auto out = vc::sanitize (s);
            expectEquals (out.nrBackground, vc::kNrBackgroundDefault);
            expectEquals (out.nrImpact, vc::kNrImpactDefault);
            expectEquals (out.eqBands[2].hz, vc::kEqDefaults[2].hz);
            expectEquals (out.eqBands[2].gainDb, vc::kEqDefaults[2].gainDb);
            expectEquals (out.eqBands[2].q, vc::kEqDefaults[2].q);
            expectEquals (out.eqBands[4].hz, vc::kEqDefaults[4].hz);
        }

        beginTest ("MicSettings: 設定ファイルの値が文字列のnan/inf/不正でも、初期値になる（有限値だけがAtomicParamsへ渡る）");
        {
            juce::PropertySet props;
            props.setValue ("nrBackground", "nan");
            props.setValue ("nrImpact", "inf");
            props.setValue (vc::eqBandKey (2, "Hz"), "nan");
            props.setValue (vc::eqBandKey (2, "GainDb"), "-inf");
            props.setValue (vc::eqBandKey (2, "Q"), "abc");

            const auto s = vc::loadSettings (props);
            expect (std::isfinite (s.nrBackground) && std::isfinite (s.nrImpact));
            expectEquals (s.nrBackground, vc::kNrBackgroundDefault);
            expectEquals (s.nrImpact, vc::kNrImpactDefault);

            // Qは"abc"を数値0として読み、範囲の下端へ丸められる（不正な文字列を有効な値として扱わないための下限）。
            expectEquals (s.eqBands[2].hz, vc::kEqDefaults[2].hz);
            expectEquals (s.eqBands[2].gainDb, vc::kEqDefaults[2].gainDb);
            expect (s.eqBands[2].q >= vc::kEqMinQ && s.eqBands[2].q <= vc::kEqMaxQ);
        }

        beginTest ("MicSettings: 設定ファイルの有限な巨大値（floatの範囲外を含む）は、初期値ではなく範囲の端へ丸められる");
        {
            juce::PropertySet props;
            props.setValue ("nrBackground", "9e99");
            props.setValue ("nrImpact", "-1e300");
            props.setValue ("gainDb", "3.5e38");
            props.setValue ("reverb", "1e300");
            props.setValue (vc::eqBandKey (0, "Hz"), "1e300");
            props.setValue (vc::eqBandKey (0, "GainDb"), "-9e99");
            props.setValue (vc::eqBandKey (0, "Q"), "3.5e38");
            props.setValue (vc::eqBandKey (1, "Hz"), "-1e300");
            props.setValue (vc::eqBandKey (1, "Q"), "-9e99");
            props.setValue (vc::eqBandKey (2, "Hz"), "1e400");

            const auto s = vc::loadSettings (props);
            expectEquals (s.nrBackground, 1.0f);
            expectEquals (s.nrImpact, 0.0f);
            expectEquals (s.gainDb, 20.0f);
            expectEquals (s.reverb, 1.0f);
            expectEquals (s.eqBands[0].hz, 20000.0f);
            expectEquals (s.eqBands[0].gainDb, -18.0f);
            expectEquals (s.eqBands[0].q, 10.0f);
            expectEquals (s.eqBands[1].hz, 20.0f);
            expectEquals (s.eqBands[1].q, 0.1f);
            expectEquals (s.eqBands[2].hz, 20000.0f); // 1e400（doubleでもinfになる値）も端へ

            // 非有限（inf・nan）は従来どおり初期値。
            props.setValue ("nrBackground", "inf");
            props.setValue (vc::eqBandKey (0, "Hz"), "-inf");
            const auto t = vc::loadSettings (props);
            expectEquals (t.nrBackground, vc::kNrBackgroundDefault);
            expectEquals (t.eqBands[0].hz, vc::kEqDefaults[0].hz);
        }

        beginTest ("MicSettings: 無効なEqTypeの整数値はsanitizeで初期値のタイプになる");
        {
            vc::SavedSettings s;
            s.eqBands[1].type = static_cast<vc::EqType> (99);
            s.eqBands[3].type = static_cast<vc::EqType> (-1);
            const auto out = vc::sanitize (s);
            expect (out.eqBands[1].type == vc::kEqDefaults[1].type);
            expect (out.eqBands[3].type == vc::kEqDefaults[3].type);
            expect (out.eqBands[0].type == vc::kEqDefaults[0].type, "有効なタイプは変えない");
        }

        beginTest ("MicSettings: タイプ名（eqTypeId / eqTypeFromId）");
        {
            const char* ids[] = { "peak", "lowshelf", "highshelf", "lowcut", "highcut" };
            const vc::EqType types[] = { vc::EqType::Peak, vc::EqType::LowShelf, vc::EqType::HighShelf,
                                         vc::EqType::LowCut, vc::EqType::HighCut };

            for (int i = 0; i < 5; ++i)
            {
                expectEquals (juce::String (vc::eqTypeId (types[i])), juce::String (ids[i]));
                expect (vc::eqTypeFromId (ids[i]) == types[i]);
            }

            expect (vc::eqTypeFromId ("HighShelf") == vc::EqType::HighShelf, "大文字小文字は区別しない");
            expect (vc::eqTypeFromId ("notch") == vc::EqType::Peak, "不明な名前はpeak");
            expect (vc::eqTypeFromId ({}) == vc::EqType::Peak);

            juce::PropertySet props;
            props.setValue (vc::eqBandKey (0, "Type"), "unknown");
            expect (vc::loadSettings (props).eqBands[0].type == vc::EqType::Peak, "キーはあるが不明なタイプ名はpeak（初期値のlowshelfではない）");
        }

        beginTest ("MicSettings: キーがない場合は初期値（ノイズ除去OFF・EQ OFF・背景65%・インパクト15%・kEqDefaults）");
        {
            const juce::PropertySet empty;
            const auto s = vc::loadSettings (empty);
            expect (! s.nrEnabled);
            expectEquals (s.nrBackground, 0.65f);
            expectEquals (s.nrImpact, 0.15f);
            expect (! s.eqEnabled);

            for (size_t i = 0; i < vc::kEqDefaults.size(); ++i)
            {
                expect (s.eqBands[i].on == vc::kEqDefaults[i].on);
                expect (s.eqBands[i].type == vc::kEqDefaults[i].type);
                expectEquals (s.eqBands[i].hz, vc::kEqDefaults[i].hz);
                expectEquals (s.eqBands[i].gainDb, vc::kEqDefaults[i].gainDb);
                expectEquals (s.eqBands[i].q, vc::kEqDefaults[i].q);
            }

            const vc::SavedSettings defaults;
            expect (! defaults.nrEnabled && ! defaults.eqEnabled); // D-020: 初回起動時はOFF
        }

        beginTest ("MicSettings: 保存→読み込みの往復が一致する（EQ 25項目＋ON/OFF＋ノイズ除去3項目）");
        {
            vc::SavedSettings s;
            s.nrEnabled = true;
            s.nrBackground = 0.35f;
            s.nrImpact = 0.8f;
            s.eqEnabled = true;
            s.eqBands[0] = { false, vc::EqType::HighCut,   16000.0f, -3.5f, 0.55f };
            s.eqBands[1] = { true,  vc::EqType::LowCut,       80.0f,  2.0f, 1.25f };
            s.eqBands[2] = { true,  vc::EqType::HighShelf,  2345.6f, 12.3f, 9.99f };
            s.eqBands[3] = { false, vc::EqType::LowShelf,     123.4f, -18.0f, 0.1f };
            s.eqBands[4] = { true,  vc::EqType::Peak,       19999.0f, 18.0f, 3.14f };

            juce::PropertySet props;
            vc::storeMicSettings (props, s);
            const auto out = vc::loadSettings (props);

            expect (out.nrEnabled == s.nrEnabled);
            expectEquals (out.nrBackground, s.nrBackground);
            expectEquals (out.nrImpact, s.nrImpact);
            expect (out.eqEnabled == s.eqEnabled);

            for (size_t i = 0; i < s.eqBands.size(); ++i)
            {
                const auto label = "band" + juce::String ((int) i + 1);
                expect (out.eqBands[i].on == s.eqBands[i].on, label + " on");
                expect (out.eqBands[i].type == s.eqBands[i].type, label + " type");
                expectEquals (out.eqBands[i].hz, s.eqBands[i].hz, label + " hz");
                expectEquals (out.eqBands[i].gainDb, s.eqBands[i].gainDb, label + " gainDb");
                expectEquals (out.eqBands[i].q, s.eqBands[i].q, label + " q");
            }

            // 設定ファイルのキー名（plan.md 8.3）。
            expectEquals (props.getValue ("eqBand2Type"), juce::String ("lowcut"));
            expectEquals (props.getValue ("eqBand5Type"), juce::String ("peak"));
            expect (props.containsKey ("nrEnabled") && props.containsKey ("nrBackground") && props.containsKey ("nrImpact")
                    && props.containsKey ("eqEnabled") && props.containsKey ("eqBand3GainDb") && props.containsKey ("eqBand1Q"));
        }

        beginTest ("MicSettings: 既定の設定（kEqDefaults・OFF）も往復で一致する");
        {
            juce::PropertySet props;
            vc::storeMicSettings (props, vc::SavedSettings());
            const auto out = vc::loadSettings (props);
            expect (! out.nrEnabled && ! out.eqEnabled);
            expectEquals (out.nrBackground, vc::kNrBackgroundDefault);

            for (size_t i = 0; i < vc::kEqDefaults.size(); ++i)
            {
                expectEquals (out.eqBands[i].hz, vc::kEqDefaults[i].hz);
                expectEquals (out.eqBands[i].q, vc::kEqDefaults[i].q);
                expect (out.eqBands[i].type == vc::kEqDefaults[i].type && out.eqBands[i].on == vc::kEqDefaults[i].on);
            }
        }
    }

    // docs/design.md 10.3節「数値欄」: 入力の解釈・表示形式・ホイールと矢印キーの刻み（UIから切り出した純粋関数）。
    void runEqInputTests()
    {
        using vc::EqField;

        auto parse = [] (EqField f, const char* utf8) { return vc::parseEqInput (f, juce::String::fromUTF8 (utf8)); };
        auto expectValue = [this] (const std::optional<float>& got, float want, const juce::String& label)
        {
            expect (got.has_value(), label + " が読めなかった");

            if (got.has_value())
                expectWithinAbsoluteError (*got, want, 0.0001f, label);
        };

        beginTest ("EQ数値欄の入力: 周波数（k付き・単位付き・全角・丸め・範囲外）");
        {
            expectValue (parse (EqField::Hz, "1000"), 1000.0f, "1000");
            expectValue (parse (EqField::Hz, "1.2k"), 1200.0f, "1.2k");
            expectValue (parse (EqField::Hz, "1.2K"), 1200.0f, "1.2K");
            expectValue (parse (EqField::Hz, "\xEF\xBC\x91\xEF\xBC\x8E\xEF\xBC\x92\xEF\xBD\x8B"), 1200.0f, "全角 １．２ｋ");
            expectValue (parse (EqField::Hz, "1200 Hz"), 1200.0f, "1200 Hz");
            expectValue (parse (EqField::Hz, "1.2 kHz"), 1200.0f, "1.2 kHz");
            expectValue (parse (EqField::Hz, "\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x90\xEF\xBC\xA8\xEF\xBD\x9A"), 1200.0f, "全角 １２００Ｈｚ");
            expectValue (parse (EqField::Hz, "  80  "), 80.0f, "前後の空白");
            expectValue (parse (EqField::Hz, "1234.6"), 1235.0f, "1234.6は1Hzへ丸め");
            expectValue (parse (EqField::Hz, "5"), 20.0f, "5は下端へ");
            expectValue (parse (EqField::Hz, "50000"), 20000.0f, "50000は上端へ");
            expectValue (parse (EqField::Hz, "99999k"), 20000.0f, "99999kは上端へ");
            expectValue (parse (EqField::Hz, "-5"), 20.0f, "負数は下端へ");
        }

        beginTest ("EQ数値欄の入力: ゲインとQ（符号・全角・丸め・範囲外）");
        {
            expectValue (parse (EqField::GainDb, "+3"), 3.0f, "+3");
            expectValue (parse (EqField::GainDb, "-4.5 dB"), -4.5f, "-4.5 dB");
            expectValue (parse (EqField::GainDb, "\xEF\xBC\x8D\xEF\xBC\x94\xEF\xBC\x8E\xEF\xBC\x95"), -4.5f, "全角 －４．５");
            expectValue (parse (EqField::GainDb, "\xEF\xBC\x8B\xEF\xBC\x93\xEF\xBC\x8E\xEF\xBC\x95\xEF\xBD\x84\xEF\xBC\xA2"), 3.5f, "全角 ＋３．５ｄＢ");
            expectValue (parse (EqField::GainDb, "\xE2\x88\x92" "2"), -2.0f, "U+2212のマイナス");
            expectValue (parse (EqField::GainDb, "1.26"), 1.3f, "0.1へ丸め");
            expectValue (parse (EqField::GainDb, "100"), 18.0f, "100は上端へ");
            expectValue (parse (EqField::GainDb, "-100"), -18.0f, "-100は下端へ");
            expectValue (parse (EqField::Q, "0.71"), 0.71f, "0.71");
            expectValue (parse (EqField::Q, "0.714"), 0.71f, "0.01へ丸め");
            expectValue (parse (EqField::Q, "0"), 0.1f, "0は下端へ");
            expectValue (parse (EqField::Q, "100"), 10.0f, "100は上端へ");
        }

        beginTest ("EQ数値欄の入力: 数値として読めない入力はnullopt（呼び出し側が元の値へ戻す）");
        {
            for (const char* bad : { "", "   ", "abc", "Hz", "k", "-", "+", ".", "1..2", "1.2.3", "1e3", "0x10", "12ab", "1k2", "1,000", "--5", "5-" })
            {
                expect (! parse (EqField::Hz, bad).has_value(), juce::String ("Hz: ") + bad);
                expect (! parse (EqField::GainDb, bad).has_value(), juce::String ("GainDb: ") + bad);
                expect (! parse (EqField::Q, bad).has_value(), juce::String ("Q: ") + bad);
            }

            // 長い入力は不正（33文字以上）。32文字までは解釈する。
            expect (! parse (EqField::Hz, juce::String::repeatedString ("1", 33).toRawUTF8()).has_value(), "33文字は不正");
            expect (! parse (EqField::Q, juce::String::repeatedString ("1", 100000).toRawUTF8()).has_value(), "10万文字は不正（時間もかからない）");
            expect (parse (EqField::Hz, juce::String::repeatedString ("0", 31).toRawUTF8()).has_value(), "31文字は解釈する");

            expect (! parse (EqField::GainDb, "1k").has_value(), "kは周波数だけ");
            expect (! parse (EqField::Q, "1k").has_value(), "kは周波数だけ");
        }

        beginTest ("EQ数値欄の表示形式");
        {
            expectEquals (vc::formatEqValue (EqField::Hz, 1200.0f), juce::String ("1200 Hz"));
            expectEquals (vc::formatEqValue (EqField::Hz, 80.4f), juce::String ("80 Hz"));
            expectEquals (vc::formatEqValue (EqField::Hz, 20000.0f), juce::String ("20000 Hz"));
            expectEquals (vc::formatEqValue (EqField::GainDb, 3.0f), juce::String ("+3.0 dB"));
            expectEquals (vc::formatEqValue (EqField::GainDb, 0.0f), juce::String ("0.0 dB"));
            expectEquals (vc::formatEqValue (EqField::GainDb, -4.5f), juce::String ("-4.5 dB"));
            expectEquals (vc::formatEqValue (EqField::GainDb, -0.04f), juce::String ("0.0 dB"));
            expectEquals (vc::formatEqValue (EqField::GainDb, 18.0f), juce::String ("+18.0 dB"));
            expectEquals (vc::formatEqValue (EqField::Q, 0.71f), juce::String ("0.71"));
            expectEquals (vc::formatEqValue (EqField::Q, 10.0f), juce::String ("10.00"));
            expectEquals (vc::formatEqValue (EqField::Q, 0.1f), juce::String ("0.10"));
        }

        beginTest ("EQ数値欄のホイール・矢印キーの刻み（周波数とQは×2^(1/12)、ゲインは0.5dB）");
        {
            expectEquals (vc::stepEqValue (EqField::Hz, 1000.0f, +1), 1059.0f);
            expectEquals (vc::stepEqValue (EqField::Hz, 1000.0f, -1), 944.0f);
            expectEquals (vc::stepEqValue (EqField::Hz, 20.0f, -1), 20.0f);       // 下端で止まる
            expectEquals (vc::stepEqValue (EqField::Hz, 20000.0f, +1), 20000.0f); // 上端で止まる
            expect (vc::stepEqValue (EqField::Hz, 20.0f, +1) > 20.0f, "下端から上へ動く");

            expectEquals (vc::stepEqValue (EqField::GainDb, 0.0f, +1), 0.5f);
            expectEquals (vc::stepEqValue (EqField::GainDb, 0.0f, -1), -0.5f);
            expectEquals (vc::stepEqValue (EqField::GainDb, 17.8f, +1), 18.0f);
            expectEquals (vc::stepEqValue (EqField::GainDb, -18.0f, -1), -18.0f);

            expectEquals (vc::stepEqValue (EqField::Q, 0.71f, +1), 0.75f);
            expectEquals (vc::stepEqValue (EqField::Q, 0.71f, -1), 0.67f);
            expectEquals (vc::stepEqValue (EqField::Q, 0.1f, +1), 0.11f);         // 丸めて変わらない前に最小刻みで動く
            expectEquals (vc::stepEqValue (EqField::Q, 10.0f, +1), 10.0f);

            // どの位置から上下へ1回ずつ動いても、範囲内で単調に動く（丸めで止まったり逆行したりしない）。
            for (float hz = 20.0f; hz < 20000.0f; hz *= 1.37f)
                expect (vc::stepEqValue (EqField::Hz, hz, +1) > std::round (hz) - 0.5f, "hz=" + juce::String (hz));
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

        // 片方だけ止まる異常(JUCE 9のWASAPI入力スレッドは1秒タイムアウトで通知なしに終了する)は、
        // もう一方が進み続けていても検出する。fixedIn=trueなら入力を固定・出力を進める。逆も同様。
        for (const bool fixedIn : { true, false })
        {
            const juce::String which = juce::String::fromUTF8 (fixedIn ? "入力が止まり出力は進行" : "出力が止まり入力は進行");

            beginTest ("ConnectionMonitor: " + which + juce::String::fromUTF8 (" → 2.0秒でCloseAndFail、1.9秒ではNone"));
            {
                auto run = [&] (juce::int64 stallMs)
                {
                    vc::ConnectionMonitor mon;
                    mon.update (0, 0, 0, false, false, true, false);

                    juce::uint64 moving = 0;
                    for (juce::int64 t = 100; t < stallMs; t += 100)
                    {
                        ++moving;
                        expect (mon.update (t, fixedIn ? 0 : moving, fixedIn ? moving : 0, false, false, true, false) == Action::None);
                    }

                    ++moving;
                    return mon.update (stallMs, fixedIn ? 0 : moving, fixedIn ? moving : 0, false, false, true, false);
                };

                expect (run (1900) == Action::None);
                expect (run (2000) == Action::CloseAndFail);
            }
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

        beginTest ("ConnectionMonitor: 一覧変更通知があってもデバイスが無ければ異常状態でもTryReopenしない");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            mon.update (2000, 0, 0, false, false, true, false); // CloseAndFail(次回予定はt=4000)

            expect (mon.update (2100, 0, 0, false, false, false, true) == Action::None);
            expect (mon.update (3900, 0, 0, false, false, false, true) == Action::None);
            expect (mon.update (4000, 0, 0, false, false, false, false) == Action::TryReopen); // 通常のスケジュールは有効
        }

        beginTest ("ConnectionMonitor: 一覧変更による再試行の次の再試行はそこから+2秒");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            mon.update (2000, 0, 0, false, false, true, false); // CloseAndFail
            expect (mon.update (2100, 0, 0, false, false, true, true) == Action::TryReopen); // 一覧変更で即時

            expect (mon.update (4000, 0, 0, false, false, false, false) == Action::None); // 元の予定(4000)ではない
            expect (mon.update (4099, 0, 0, false, false, false, false) == Action::None);
            expect (mon.update (4100, 0, 0, false, false, false, false) == Action::TryReopen); // 2100+2000
            expect (mon.update (6100, 0, 0, false, false, false, false) == Action::TryReopen); // さらに+2秒
        }

        beginTest ("ConnectionMonitor: 健常時の一覧変更通知は、デバイスがあれば無視する");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);

            juce::uint64 in = 0, out = 0;
            for (juce::int64 t = 100; t <= 1000; t += 100)
            {
                ++in; ++out;
                expect (mon.update (t, in, out, false, false, true, true) == Action::None);
            }

            expect (! mon.isFailed());
        }

        beginTest ("ConnectionMonitor: 健常時に一覧変更があり使用中デバイスが一覧に無ければ即CloseAndFail");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            expect (mon.update (100, 1, 1, false, false, false, false) == Action::None); // 通知なしなら一覧だけでは判定しない
            expect (mon.update (200, 2, 2, false, false, false, true) == Action::CloseAndFail);
            expect (mon.isFailed());
            expect (mon.update (2200, 2, 2, false, false, false, false) == Action::TryReopen); // 以後は+2秒ごと
        }

        beginTest ("ConnectionMonitor: 異常状態中のエラーフラグ・再オープン要求はCloseAndFailを重ねて返さない");
        {
            vc::ConnectionMonitor mon;
            mon.update (0, 0, 0, false, false, true, false);
            expect (mon.update (100, 0, 0, true, false, true, false) == Action::CloseAndFail);

            expect (mon.update (200, 0, 0, true, true, false, false) == Action::None);
            expect (mon.update (2100, 0, 0, true, false, false, false) == Action::TryReopen);
        }

        beginTest ("ConnectionMonitor: enterFailed(開くこと自体に失敗)後は2秒ごとにTryReopen");
        {
            vc::ConnectionMonitor mon;
            mon.enterFailed (1000);
            expect (mon.isFailed());

            expect (mon.update (2900, 0, 0, false, false, false, false) == Action::None);
            expect (mon.update (3000, 0, 0, false, false, false, false) == Action::TryReopen);
            expect (mon.update (4900, 0, 0, false, false, false, false) == Action::None);
            expect (mon.update (5000, 0, 0, false, false, false, false) == Action::TryReopen);
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

            // 値だけでなくラベルとの対応も検証する(値が別の項目に入れ替わる不具合を検出する)。
            expect (line.contains ("t=3661s"));          // 経過時間
            expect (line.contains ("latencyMs=45.2"));   // レイテンシ
            expect (line.contains ("fillMs=12.3"));      // 充填量
            expect (line.contains ("underruns=3"));
            expect (line.contains ("overruns=1"));
            expect (line.contains ("speedPpm=-15.5"));   // 速度比補正
            expect (line.contains ("cpu=0.8%"));         // CPU使用率
            expect (line.contains ("memMB=42.0"));       // メモリ(MB)
            expect (line.contains ("nr=OFF"));           // ノイズ除去（既定はOFF）
            expect (line.contains ("eq=OFF"));           // EQ
        }

        beginTest ("StatsLog: ノイズ除去とEQのON/OFFが行に出る（CPU要件の判定条件を区別するため）");
        {
            vc::StatsSnapshot s;
            s.nrEnabled = true;
            s.eqEnabled = false;
            auto line = vc::formatStatsLine (s);
            expect (line.contains ("nr=ON") && line.contains ("eq=OFF"), line);

            s.nrEnabled = false;
            s.eqEnabled = true;
            line = vc::formatStatsLine (s);
            expect (line.contains ("nr=OFF") && line.contains ("eq=ON"), line);

            s.nrEnabled = true;
            line = vc::formatStatsLine (s);
            expect (line.contains ("nr=ON") && line.contains ("eq=ON"), line);
        }

        beginTest ("StatsLog: 速度比補正が正のときは符号を付ける");
        {
            vc::StatsSnapshot s;
            s.speedCorrectionPpm = 12.0;
            expect (vc::formatStatsLine (s).contains ("speedPpm=+12.0"));
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
