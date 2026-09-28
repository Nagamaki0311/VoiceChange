#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

// ===== SECTION: ConnectionMonitor =====
// デバイス異常の判定と再接続タイミング（純粋ロジック、音声パス非依存）。
// docs/plan.md 2.5節「ConnectionMonitor」、docs/spec.md「デバイス選択」、docs/decisions.md D-009参照。
// AudioIOが500msの`Timer`と一覧変更通知(audioDeviceListChanged)から呼ぶ。呼び出し元はこの戻り値に
// 従いCloseAndFailで両方のデバイスをclose()、TryReopenで両方をopen()し直す
// （片方だけの異常でも両方を閉じ、両方揃ってから開き直す。D-009。この対称性はAudioIO側の責務）。

namespace vc
{

class ConnectionMonitor
{
public:
    enum class Action { None, CloseAndFail, TryReopen };

    // nowMsは単調時計(ミリ秒、juce::Time::getMillisecondCounter()相当)。
    // inCount/outCountは入力・出力コールバックの呼び出し回数(ウォッチドッグ)。値が2秒進まなければ異常とみなす。
    // errorFlagはaudioDeviceError・無言終了・例外のいずれか、reopenRequestedはレート/バッファ長の変化検出。
    // devicesPresentは現在の一覧に開きたいデバイス(入出力とも)が存在するか、listChangedは一覧変更通知の有無。
    Action update (juce::int64 nowMs, std::uint64_t inCount, std::uint64_t outCount,
                   bool errorFlag, bool reopenRequested, bool devicesPresent, bool listChanged) noexcept
    {
        if (! failed)
        {
            if (! initialised)
            {
                // 初回呼び出しは基準を記録するだけ(比較対象がまだない)。
                initialised = true;
                lastInCount = inCount;
                lastOutCount = outCount;
                lastProgressMs = nowMs;
                return Action::None;
            }

            if (inCount != lastInCount || outCount != lastOutCount)
            {
                lastInCount = inCount;
                lastOutCount = outCount;
                lastProgressMs = nowMs;
            }

            const bool stalled = (nowMs - lastProgressMs) >= kWatchdogMs;

            if (errorFlag || reopenRequested || stalled)
            {
                failed = true;
                nextRetryMs = nowMs + kRetryIntervalMs;
                return Action::CloseAndFail;
            }

            return Action::None;
        }

        // 異常状態: 一覧変更通知が来てデバイスがあれば即座に、それ以外は2秒ごとに再接続を試みる。
        if (listChanged && devicesPresent)
        {
            nextRetryMs = nowMs + kRetryIntervalMs;
            return Action::TryReopen;
        }

        if (nowMs >= nextRetryMs)
        {
            nextRetryMs = nowMs + kRetryIntervalMs;
            return Action::TryReopen;
        }

        return Action::None;
    }

    bool isFailed() const noexcept { return failed; }

private:
    static constexpr juce::int64 kWatchdogMs = 2000;
    static constexpr juce::int64 kRetryIntervalMs = 2000;

    bool initialised = false;
    bool failed = false;

    std::uint64_t lastInCount = 0;
    std::uint64_t lastOutCount = 0;
    juce::int64 lastProgressMs = 0;
    juce::int64 nextRetryMs = 0;
};

} // namespace vc
