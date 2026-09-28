#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

// ===== SECTION: StatsLog =====
// 60秒ごとの統計ログ行の整形とファイルの1MBローテーション（core）。
// docs/spec.md「計測とログ」、docs/plan.md 2.5節「StatsLog」参照。
// 60秒`juce::Timer`での呼び出し・ファイルへの追記(appendText)はMain.cpp（メッセージスレッドのみ）。
// プロセスのメモリ使用量の取得(Windowsの`GetProcessMemoryInfo`)はOS依存のためMain.cpp側で行い、
// ここでは受け取った値を整形するだけにする。

namespace vc
{

struct StatsSnapshot
{
    double elapsedSeconds = 0.0;      // 起動からの経過時間
    double latencyMs = 0.0;           // 合計レイテンシ(デバイス入出力+リングバッファ+ピッチシフター)
    double fillMs = 0.0;              // リングバッファ充填量(平滑化、ms換算)
    std::uint32_t underruns = 0;
    std::uint32_t overruns = 0;
    double speedCorrectionPpm = 0.0;  // クロックずれ補正量
    double cpuPercent = 0.0;
    std::int64_t memoryBytes = 0;     // プロセスのメモリ使用量(Windows以外は0)
};

// 1時間連続動作テストの判定材料にする行(docs/spec.md「計測とログ」)。全項目を1行に含む。
juce::String formatStatsLine (const StatsSnapshot& s);

// fileの現在サイズがlimitBytesを超えていたら削除して作り直す(以後のappendTextで新規作成される)。
// limitBytes以下、またはfileが存在しない場合は何もしない。
void resetIfLarger (const juce::File& file, juce::int64 limitBytes);

} // namespace vc
