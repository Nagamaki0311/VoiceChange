#pragma once

#include <array>

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

// AtomicParams / SavedSettings / sanitize / presetFromId / shifterShouldRun は
// T-004以降（層1・ピッチシフター実装時）に追加する（docs/plan.md 3章 T-004参照）。

} // namespace vc
