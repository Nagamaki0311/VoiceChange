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

    // nowMsはラップしない単調時計(ミリ秒。(int64) Time::getMillisecondCounterHiRes()相当。32bitのgetMillisecondCounter()は
    // 約49.7日でラップするため使わない)。
    // inCount/outCountは入力・出力コールバックの呼び出し回数(ウォッチドッグ)。入力・出力それぞれの最終進捗時刻を持ち、
    // どちらか一方でも2秒進まなければ異常とみなす(JUCE 9のWASAPI入力スレッドは1秒タイムアウトで通知なしに終了するため、
    // 片方だけ止まる異常を「もう一方が進んでいる」ことで見逃さない)。
    // errorFlagはaudioDeviceError・例外等、reopenRequestedはレート/バッファ長の変化検出。
    // devicesPresentは現在の一覧に開きたいデバイス(入出力とも)が存在するか、listChangedは一覧変更通知の有無。
    // 健常時に「一覧変更 && 使用中のデバイスが一覧に無い」も異常(CloseAndFail)とする。
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
                lastInProgressMs = nowMs;
                lastOutProgressMs = nowMs;
                return Action::None;
            }

            if (inCount != lastInCount)
            {
                lastInCount = inCount;
                lastInProgressMs = nowMs;
            }

            if (outCount != lastOutCount)
            {
                lastOutCount = outCount;
                lastOutProgressMs = nowMs;
            }

            const bool stalled = (nowMs - lastInProgressMs) >= kWatchdogMs
                              || (nowMs - lastOutProgressMs) >= kWatchdogMs;
            const bool deviceGone = listChanged && ! devicesPresent;

            if (errorFlag || reopenRequested || stalled || deviceGone)
            {
                enterFailed (nowMs);
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

    // 開くこと自体に失敗した場合(コールバックがまだ一度も動いていない)に、ウォッチドッグを経由せず
    // 異常状態へ入れて、2秒ごとの再試行を続けさせる。
    void enterFailed (juce::int64 nowMs) noexcept
    {
        initialised = true;
        failed = true;
        nextRetryMs = nowMs + kRetryIntervalMs;
    }

    bool isFailed() const noexcept { return failed; }

private:
    static constexpr juce::int64 kWatchdogMs = 2000;
    static constexpr juce::int64 kRetryIntervalMs = 2000;

    bool initialised = false;
    bool failed = false;

    std::uint64_t lastInCount = 0;
    std::uint64_t lastOutCount = 0;
    juce::int64 lastInProgressMs = 0;
    juce::int64 lastOutProgressMs = 0;
    juce::int64 nextRetryMs = 0;
};

} // namespace vc
