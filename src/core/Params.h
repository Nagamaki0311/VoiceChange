#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>

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
constexpr float kNrBackgroundDefault = 0.7f;

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

// 非有限値は初期値（fallback）へ、範囲外は端へ丸める。juce::jlimitはNaNをそのまま返すため、範囲チェックの前に非有限値を除く。
// Equalizer::setTargetが毎ブロックこれを通す（設定ファイルの読み込み側［T-013のsanitize］も同じ関数を使う）。
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
    std::atomic<float> nrImpact { 0.0f };                     // インパクトノイズ 0〜1

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

    return s;
}

} // namespace vc
