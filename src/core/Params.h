#pragma once

#include <array>
#include <atomic>

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

// UIスレッドと音声スレッド間で受け渡す層1パラメータ。すべてatomicのみ（音声スレッドはロックしない）。
struct AtomicParams
{
    std::atomic<float> gainDb { 0.0f };  // -20〜+20dB
    std::atomic<float> reverb { 0.0f };  // 0〜1（0〜100%）
    std::atomic<int> pitch { 0 };        // -12〜+12半音（層1ピッチ、1半音刻み）
    std::atomic<int> preset { 0 };       // Presetのint値
    std::atomic<bool> enabled { true };  // 全体ON/OFF（false = バイパス）
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

// SavedSettings / sanitize / presetFromId はT-006（UI結合・設定保存）で追加する
// （docs/plan.md 3章 T-006参照）。

} // namespace vc
