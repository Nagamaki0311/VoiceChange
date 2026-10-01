#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>
#include <optional>

// ===== SECTION: Params =====
// 層1・層2の共通パラメータ定義。GUI・デバイスに依存しない（vc_core）。
// docs/spec.md「層2: 特殊効果プリセット」の表と対応する。

namespace vc
{

// UI表示名（design.md）と内部識別子の対照表:
//   Normal   = ノーマル
//   Echo     = エコー
//   Helium   = ヘリウム
//   Minion   = ミニオン
//   Giant    = ジャイアント
//   Kerokero = ケロケロ
//   Robot    = ロボット
//   Talkbox  = トークボックス
enum class Preset
{
    Normal = 0,
    Echo,
    Helium,
    Minion,
    Giant,
    Kerokero,
    Robot,
    Talkbox
};

// 層2のピッチ以外の効果。
enum class Effect
{
    None = 0,
    Echo,
    Robot,
    Talkbox
};

struct PresetSpec
{
    const char* id;
    float semitones;
    float formant;
    Effect effect;
    bool needsDetector;
};

// docs/spec.md「層2: 特殊効果プリセット」の表のとおり。
constexpr std::array<PresetSpec, 8> kPresets { {
    { "normal",   0.0f, 1.00f, Effect::None,    false },
    { "echo",     0.0f, 1.00f, Effect::Echo,    false },
    { "helium",   0.0f, 1.60f, Effect::None,    false },
    { "minion",   8.0f, 1.40f, Effect::None,    false },
    { "giant",   -6.0f, 0.75f, Effect::None,    false },
    { "kerokero", 0.0f, 1.00f, Effect::None,    true  },
    { "robot",    0.0f, 1.00f, Effect::Robot,   false },
    { "talkbox",  0.0f, 1.00f, Effect::Talkbox, true  },
} };

// マイク処理（ノイズ除去）の背景ノイズの初期値。AtomicParamsの初期値と、Engineが非有限値を読んだときの代替値。
constexpr float kNrBackgroundDefault = 0.65f;
constexpr float kNrImpactDefault = 0.15f;

// ===== SECTION: EQ（T-012） =====
// マイク処理のEQ（5バンド）。docs/spec.md「マイク処理: EQ」、docs/decisions.md D-024、docs/plan.md 8.2節参照。
// UI表示名（design.md）と内部識別子の対照表:
//   Peak = ピーキング / LowShelf = ローシェルフ / HighShelf = ハイシェルフ
//   LowCut = ローカット（ハイパスフィルタ。IIR::makeHighPass） / HighCut = ハイカット（ローパスフィルタ。IIR::makeLowPass）
enum class EqType
{
    Peak = 0,
    LowShelf,
    HighShelf,
    LowCut,
    HighCut
};

constexpr int kEqBands = 5;

struct EqBandSettings
{
    bool on;
    EqType type;
    float hz;
    float gainDb;
    float q;
};

// 範囲（spec.mdの表）。周波数の実効上限は、さらに0.45×出力レートで頭打ちにする（Equalizer側）。
constexpr float kEqMinHz = 20.0f, kEqMaxHz = 20000.0f;
constexpr float kEqMaxGainDb = 18.0f;
constexpr float kEqMinQ = 0.10f, kEqMaxQ = 10.0f;

// 初期値（仮）。spec.mdの初期値のとおり。T-015でSonarの設定を再現した値に置き換える。
constexpr std::array<EqBandSettings, kEqBands> kEqDefaults { {
    { true, EqType::LowShelf,    100.0f, 0.0f, 0.71f },
    { true, EqType::Peak,        250.0f, 0.0f, 0.71f },
    { true, EqType::Peak,       1000.0f, 0.0f, 0.71f },
    { true, EqType::Peak,       3000.0f, 0.0f, 0.71f },
    { true, EqType::HighShelf,  8000.0f, 0.0f, 0.71f },
} };

// int値をEqTypeへ。範囲外ならfallback。
inline EqType eqTypeFromInt (int value, EqType fallback) noexcept
{
    return value >= (int) EqType::Peak && value <= (int) EqType::HighCut ? static_cast<EqType> (value) : fallback;
}

// 設定ファイルのタイプ名（plan.md 8.2節・T-013）。不明な名前・大文字小文字違いはeqTypeFromIdで扱う。
constexpr std::array<const char*, 5> kEqTypeIds { "peak", "lowshelf", "highshelf", "lowcut", "highcut" };

inline const char* eqTypeId (EqType t) noexcept
{
    return kEqTypeIds[(size_t) t];
}

// 不明な名前はPeak。大文字小文字は区別しない（presetFromIdと同じ理由: 手編集された設定ファイルを壊れた状態にしない）。
inline EqType eqTypeFromId (const juce::String& id) noexcept
{
    for (size_t i = 0; i < kEqTypeIds.size(); ++i)
        if (id.equalsIgnoreCase (kEqTypeIds[i]))
            return static_cast<EqType> (i);

    return EqType::Peak;
}

// 非有限値は初期値（fallback）へ、範囲外は端へ丸める。juce::jlimitはNaNをそのまま返すため、範囲チェックの前に非有限値を除く。
// Equalizer::setTargetが毎ブロックこれを通す（設定ファイルの読み込み側のsanitizeも同じ関数を使う）。
inline EqBandSettings sanitizeEqBand (EqBandSettings s, const EqBandSettings& fallback) noexcept
{
    if (! std::isfinite (s.hz))
        s.hz = fallback.hz;
    if (! std::isfinite (s.gainDb))
        s.gainDb = fallback.gainDb;
    if (! std::isfinite (s.q))
        s.q = fallback.q;

    s.hz = juce::jlimit (kEqMinHz, kEqMaxHz, s.hz);
    s.gainDb = juce::jlimit (-kEqMaxGainDb, kEqMaxGainDb, s.gainDb);
    s.q = juce::jlimit (kEqMinQ, kEqMaxQ, s.q);

    return s;
}

// UIスレッドと音声スレッド間で受け渡すEQ1バンドの値。各フィールドは独立にatomicで読み書きする
// （周波数とゲインが1ブロックだけ新旧混在しうるが、どちらも有効値で補間されるため許容する。plan.md 8.2）。
struct EqBandAtomic
{
    std::atomic<bool> on { true };
    std::atomic<int> type { (int) EqType::Peak }; // EqTypeのint値
    std::atomic<float> hz { 1000.0f };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> q { 0.71f };

    void store (const EqBandSettings& s) noexcept
    {
        on.store (s.on, std::memory_order_relaxed);
        type.store ((int) s.type, std::memory_order_relaxed);
        hz.store (s.hz, std::memory_order_relaxed);
        gainDb.store (s.gainDb, std::memory_order_relaxed);
        q.store (s.q, std::memory_order_relaxed);
    }

    // UIスレッド（メッセージスレッド）が現在値を読む。不正なtypeはfallbackのtypeにする。丸めはしない（書き込み側が丸め済み）。
    EqBandSettings load (const EqBandSettings& fallback) const noexcept
    {
        return { on.load (std::memory_order_relaxed), eqTypeFromInt (type.load (std::memory_order_relaxed), fallback.type),
                 hz.load (std::memory_order_relaxed), gainDb.load (std::memory_order_relaxed), q.load (std::memory_order_relaxed) };
    }
};

// UIスレッドと音声スレッド間で受け渡す層1パラメータ。すべてatomicのみ（音声スレッドはロックしない）。
struct AtomicParams
{
    AtomicParams()
    {
        for (size_t i = 0; i < eqBands.size(); ++i)
            eqBands[i].store (kEqDefaults[i]);
    }

    std::atomic<float> gainDb { 0.0f };  // -20〜+20dB
    std::atomic<float> reverb { 0.0f };  // 0〜1（0〜100%）
    std::atomic<int> pitch { 0 };        // -12〜+12半音（層1ピッチ、1半音刻み）
    std::atomic<int> preset { 0 };       // Presetのint値
    std::atomic<bool> enabled { true };  // 全体ON/OFF（false = バイパス）

    // マイク処理（ノイズ除去）。全体ON/OFFの対象外（D-020）。docs/spec.md「マイク処理: ノイズ除去」。
    std::atomic<bool> nrEnabled { false };
    std::atomic<float> nrBackground { kNrBackgroundDefault }; // 背景ノイズ 0〜1（0〜100%）
    std::atomic<float> nrImpact { kNrImpactDefault };         // インパクトノイズ 0〜1

    // マイク処理（EQ）。同じく全体ON/OFFの対象外（D-020）。docs/spec.md「マイク処理: EQ」。
    std::atomic<bool> eqEnabled { false };
    std::array<EqBandAtomic, kEqBands> eqBands;               // 初期値はkEqDefaults（コンストラクタで設定）
};

static_assert (std::atomic<float>::is_always_lock_free);

// ピッチシフターを稼働させるかどうか（docs/spec.md「ピッチシフターの休止」）。
// 合計移調量が0かつフォルマント係数が1のとき（層1ピッチも0）は休止する。
// ケロケロは補正量が0付近でも常に稼働させ、休止と稼働の往復を防ぐ（docs/plan.md 2.5節）。
inline bool shifterShouldRun (Preset preset, int layer1PitchSemitones) noexcept
{
    if (preset == Preset::Kerokero)
        return true;

    const auto& spec = kPresets[(size_t) preset];
    return spec.semitones != 0.0f || spec.formant != 1.0f || layer1PitchSemitones != 0;
}

// ===== SECTION: 設定の保存・復元（T-006） =====
// docs/spec.md「設定の保存」、docs/plan.md 2.5節「Params.h」参照。
// PropertiesFileへ読み書きする値の集合。範囲外の値はsanitize()で範囲内へ丸める。
struct SavedSettings
{
    juce::String inputDevice;
    juce::String outputDevice;
    float gainDb = 0.0f;   // -20〜+20dB
    int pitch = 0;         // -12〜+12半音
    float reverb = 0.0f;   // 0〜1（AtomicParams::reverbと同じ単位。0〜100%はUI表示のみの変換）
    Preset preset = Preset::Normal;
    bool enabled = true;
    bool trayNoticeShown = false; // 初回のトレイ格納通知を表示済みか

    // マイク処理（T-013）。全体ON/OFFの対象外（D-020）。AtomicParamsと同じ単位（背景ノイズ・インパクトは0〜1）。
    bool nrEnabled = false;
    float nrBackground = kNrBackgroundDefault;
    float nrImpact = kNrImpactDefault;
    bool eqEnabled = false;
    std::array<EqBandSettings, kEqBands> eqBands = kEqDefaults;
};

// プリセットのUI表示名(design.md、対照表は本ファイル冒頭のPreset enum定義直上のコメントを正とする)。
// MainComponentのプリセットボタン(表示順は種類別。src/app/MainComponent.cppのkOrder参照)と
// Main.cppのトレイのツールチップ(design.md 7.1節)の両方から参照する、名前の単一の出典。
inline juce::String presetDisplayName (Preset p) noexcept
{
    switch (p)
    {
        case Preset::Normal:   return juce::String::fromUTF8 ("ノーマル");
        case Preset::Echo:     return juce::String::fromUTF8 ("エコー");
        case Preset::Helium:   return juce::String::fromUTF8 ("ヘリウム");
        case Preset::Minion:   return juce::String::fromUTF8 ("ミニオン");
        case Preset::Giant:    return juce::String::fromUTF8 ("ジャイアント");
        case Preset::Kerokero: return juce::String::fromUTF8 ("ケロケロ");
        case Preset::Robot:    return juce::String::fromUTF8 ("ロボット");
        case Preset::Talkbox:  return juce::String::fromUTF8 ("トークボックス");
    }

    return juce::String::fromUTF8 ("ノーマル");
}

// 不明なプリセット名はNormalにする。大文字小文字は区別しない
// （設定ファイルを手で編集された場合でも壊れた状態にしないため）。
inline Preset presetFromId (const juce::String& id) noexcept
{
    for (std::size_t i = 0; i < kPresets.size(); ++i)
        if (id.equalsIgnoreCase (kPresets[i].id))
            return static_cast<Preset> (i);

    return Preset::Normal;
}

// 範囲外の値を範囲の端へ丸める。プリセットの文字列からのデコードは呼び出し側で
// presetFromId()を通してから渡す（このため引数のpresetは常に有効な値として扱う）。
// レビュー指摘2: juce::jlimitはNaNをそのまま返す（NaN < loもNaN > hiも false のため）。
// 設定ファイルが手編集等で"nan"を含んでいた場合にAtomicParamsへNaNが伝播すると、
// Engineが毎ブロック非有限値検査に落ちて恒久的に無音になるため、範囲チェックの前に
// 非有限値を初期値へ丸める。
inline SavedSettings sanitize (SavedSettings s) noexcept
{
    if (! std::isfinite (s.gainDb))
        s.gainDb = 0.0f;
    if (! std::isfinite (s.reverb))
        s.reverb = 0.0f;

    s.gainDb = juce::jlimit (-20.0f, 20.0f, s.gainDb);
    s.pitch = juce::jlimit (-12, 12, s.pitch);
    s.reverb = juce::jlimit (0.0f, 1.0f, s.reverb);

    if (! std::isfinite (s.nrBackground))
        s.nrBackground = kNrBackgroundDefault;
    if (! std::isfinite (s.nrImpact))
        s.nrImpact = kNrImpactDefault;

    s.nrBackground = juce::jlimit (0.0f, 1.0f, s.nrBackground);
    s.nrImpact = juce::jlimit (0.0f, 1.0f, s.nrImpact);

    for (size_t i = 0; i < s.eqBands.size(); ++i)
    {
        s.eqBands[i] = sanitizeEqBand (s.eqBands[i], kEqDefaults[i]);
        s.eqBands[i].type = eqTypeFromInt ((int) s.eqBands[i].type, kEqDefaults[i].type);
    }

    return s;
}

// 設定ファイル（PropertiesFile。PropertySetはjuce_coreにあるため、テストからも直接使える）との読み書き。
// キー: nrEnabled / nrBackground / nrImpact / eqEnabled / eqBand{1..5}{On,Type,Hz,GainDb,Q}（plan.md 8.3 T-013）。
inline juce::String eqBandKey (int band, const char* item)
{
    return "eqBand" + juce::String (band + 1) + item;
}

// 設定値のfloat読み込み。キーがなければfallback、非有限（nan・inf）もfallback。有限な値は、doubleのまま範囲へ丸めてからfloatにする
// （`1e300`のようなfloatを超える値をfloatへ先にキャストするとinfになり、端ではなく初期値になってしまうため）。
inline float readClampedFloat (const juce::PropertySet& props, const juce::String& key, float fallback, float lo, float hi)
{
    const double v = props.getDoubleValue (key, (double) fallback);

    // `1e400`のように数字を含む文字列がdoubleでもinfになった場合は、オーバーフローなので符号の側の端へ。数字を含まない`inf`・`nan`はfallback。
    if (std::isinf (v) && props.getValue (key).containsAnyOf ("0123456789"))
        return v > 0.0 ? hi : lo;

    return std::isfinite (v) ? (float) juce::jlimit ((double) lo, (double) hi, v) : fallback;
}

// キーがなければ初期値。範囲外などの丸めはsanitize()が行う。
inline SavedSettings loadSettings (const juce::PropertySet& props)
{
    SavedSettings s;
    s.inputDevice = props.getValue ("inputDevice");
    s.outputDevice = props.getValue ("outputDevice");
    s.gainDb = readClampedFloat (props, "gainDb", 0.0f, -20.0f, 20.0f);
    // pitchは設定ファイルの手編集等で小数になっている可能性があるため、丸めてから整数として読む。
    s.pitch = (int) std::lround (props.getDoubleValue ("pitch", 0.0));
    s.reverb = readClampedFloat (props, "reverb", 0.0f, 0.0f, 1.0f);
    s.preset = presetFromId (props.getValue ("preset", "normal")); // 不明な名前・キー無し→Normal
    s.enabled = props.getBoolValue ("enabled", true);
    s.trayNoticeShown = props.getBoolValue ("trayNoticeShown", false);

    s.nrEnabled = props.getBoolValue ("nrEnabled", s.nrEnabled);
    s.nrBackground = readClampedFloat (props, "nrBackground", s.nrBackground, 0.0f, 1.0f);
    s.nrImpact = readClampedFloat (props, "nrImpact", s.nrImpact, 0.0f, 1.0f);
    s.eqEnabled = props.getBoolValue ("eqEnabled", s.eqEnabled);

    for (int i = 0; i < kEqBands; ++i)
    {
        auto& b = s.eqBands[(size_t) i]; // 初期値はkEqDefaults
        b.on = props.getBoolValue (eqBandKey (i, "On"), b.on);
        b.type = props.containsKey (eqBandKey (i, "Type")) ? eqTypeFromId (props.getValue (eqBandKey (i, "Type"))) : b.type;
        b.hz = readClampedFloat (props, eqBandKey (i, "Hz"), b.hz, kEqMinHz, kEqMaxHz);
        b.gainDb = readClampedFloat (props, eqBandKey (i, "GainDb"), b.gainDb, -kEqMaxGainDb, kEqMaxGainDb);
        b.q = readClampedFloat (props, eqBandKey (i, "Q"), b.q, kEqMinQ, kEqMaxQ);
    }

    return sanitize (s); // 範囲外の値を範囲の端へ丸める
}

// マイク処理の項目（ノイズ除去3項目＋EQ 1＋5×5項目）をすべて書く。値が変わらないキーは、PropertySetが変更扱いにしない。
inline void storeMicSettings (juce::PropertySet& props, const SavedSettings& s)
{
    props.setValue ("nrEnabled", s.nrEnabled);
    props.setValue ("nrBackground", (double) s.nrBackground);
    props.setValue ("nrImpact", (double) s.nrImpact);
    props.setValue ("eqEnabled", s.eqEnabled);

    for (int i = 0; i < kEqBands; ++i)
    {
        const auto& b = s.eqBands[(size_t) i];
        props.setValue (eqBandKey (i, "On"), b.on);
        props.setValue (eqBandKey (i, "Type"), juce::String (eqTypeId (b.type)));
        props.setValue (eqBandKey (i, "Hz"), (double) b.hz);
        props.setValue (eqBandKey (i, "GainDb"), (double) b.gainDb);
        props.setValue (eqBandKey (i, "Q"), (double) b.q);
    }
}

// ===== SECTION: EQの数値欄（T-013） =====
// マイク処理ウィンドウの数値欄（周波数・ゲイン・Q）の入力解釈・表示・ホイール/矢印キーの刻み。
// Labelなどに依存しない純粋な関数にして、AppLogicテストで検証する（docs/design.md 10.3節）。
enum class EqField
{
    Hz,
    GainDb,
    Q
};

// 表示形式: 周波数は整数のHz（`1200 Hz`）、ゲインは小数1桁で符号付き（`+3.0 dB` / `0.0 dB` / `-4.5 dB`）、Qは小数2桁（`0.71`）。
inline juce::String formatEqValue (EqField field, float value)
{
    switch (field)
    {
        case EqField::Hz:
            return juce::String ((int) std::lround (value)) + " Hz";

        case EqField::GainDb:
        {
            const double r = std::round ((double) value * 10.0) / 10.0;
            return (r > 0.0 ? juce::String ("+") : juce::String()) + juce::String (std::abs (r) < 0.05 ? 0.0 : r, 1) + " dB"; // -0.0を出さない
        }

        case EqField::Q:
            return juce::String ((double) value, 2);
    }

    return {};
}

// 1つの値を表示の精度（周波数1Hz、ゲイン0.1dB、Q 0.01）へ丸めて、範囲の端へ収める。
inline float roundEqValue (EqField field, double value) noexcept
{
    switch (field)
    {
        case EqField::Hz:     return juce::jlimit (kEqMinHz, kEqMaxHz, (float) std::round (value));
        case EqField::GainDb: return juce::jlimit (-kEqMaxGainDb, kEqMaxGainDb, (float) (std::round (value * 10.0) / 10.0));
        case EqField::Q:      return juce::jlimit (kEqMinQ, kEqMaxQ, (float) (std::round (value * 100.0) / 100.0));
    }

    return (float) value;
}

constexpr int kEqInputMaxLength = 32; // 数値欄が解釈する文字列の最大長（編集用TextEditorの入力制限は16文字）

// 入力文字列の解釈。全角の数字・記号・英字は半角に直し、空白と単位（Hz・dB）は無視する。周波数は「k」付きなら1000倍
// （`1.2k` → 1200）。丸めと範囲外の端への丸めはroundEqValue。数値として読めなければnullopt（呼び出し側は元の値へ戻す）。
inline std::optional<float> parseEqInput (EqField field, const juce::String& text)
{
    // 数値として意味のある長さは高々十数文字。長い入力は不正とする（下の文字列処理が入力長の二乗の時間になるため）。
    if (text.length() > kEqInputMaxLength)
        return std::nullopt;

    juce::String t;

    for (auto c : text)
    {
        if (c >= 0xFF01 && c <= 0xFF5E) // 全角の ！〜～ を半角へ（０〜９、．、＋、－、ｋ、Ｈｚ など）
            c -= 0xFEE0;
        else if (c == 0x3000)           // 全角の空白
            c = ' ';
        else if (c == 0x2212)           // 「−」（U+2212）
            c = '-';

        t += juce::String::charToString (juce::CharacterFunctions::toLowerCase (c));
    }

    t = t.removeCharacters (" \t");

    for (const char* unit : { "hz", "db" })
        if (t.endsWith (unit))
        {
            t = t.dropLastCharacters (2);
            break;
        }

    double scale = 1.0;

    if (field == EqField::Hz && t.endsWith ("k"))
    {
        scale = 1000.0;
        t = t.dropLastCharacters (1);
    }

    // [+-]?(数字と、高々1つの小数点)。数字が1つもなければ不正（"1e3"や"0x10"なども読まない）。
    bool sawDigit = false, sawDot = false;

    for (int i = 0; i < t.length(); ++i)
    {
        const auto c = t[i];

        if (c >= '0' && c <= '9')
            sawDigit = true;
        else if (c == '.' && ! sawDot)
            sawDot = true;
        else if (! (i == 0 && (c == '+' || c == '-')))
            return std::nullopt;
    }

    if (! sawDigit)
        return std::nullopt;

    return roundEqValue (field, t.getDoubleValue() * scale);
}

// ホイール・矢印キー1回ぶんの変更。direction > 0で増やす。周波数とQは×2^(±1/12)（丸めた結果が変わらなければ最小刻み）、
// ゲインは±0.5dB。範囲の端では止まる。
inline float stepEqValue (EqField field, float current, int direction) noexcept
{
    const double sign = direction > 0 ? 1.0 : -1.0;

    if (field == EqField::GainDb)
        return roundEqValue (field, (double) current + sign * 0.5);

    const double quantum = field == EqField::Hz ? 1.0 : 0.01;
    const double scaled = (double) current * std::pow (2.0, sign / 12.0);
    const double before = std::round ((double) current / quantum) * quantum;
    double next = std::round (scaled / quantum) * quantum;

    if (std::abs (next - before) < quantum * 0.5)
        next = before + sign * quantum;

    return roundEqValue (field, next);
}

} // namespace vc
