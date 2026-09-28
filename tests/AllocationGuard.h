#pragma once

#include <cstddef>

// ===== SECTION: AllocationGuard =====
// グローバルoperator new/new[]/aligned版/nothrow版（対応するdelete含む）の置き換えはTestMain.cppで行う。
// ここでは各テストファイルから使う薄いRAIIガードだけを公開する（docs/plan.md 5章）。
// 使い方: ScopedAllocationGuardでpush/pull/Engine::process等の呼び出しを囲み、guard.count()を
// ガードが生きている間に読み取って変数へ控え、expect()自体はガードのスコープを抜けた後に呼ぶ
// （expectが内部でString生成等のアロケーションをする可能性があるため、計測に混ぜない）。

namespace vc::test
{

// TestMain.cppが定義する。有効な間だけグローバルoperator new/deleteの呼び出し回数を数える。
extern thread_local bool g_allocationGuardActive;
extern thread_local std::size_t g_allocationCount;

class ScopedAllocationGuard
{
public:
    ScopedAllocationGuard() noexcept
        : previousActive (g_allocationGuardActive), previousCount (g_allocationCount)
    {
        g_allocationGuardActive = true;
        g_allocationCount = 0;
    }

    ~ScopedAllocationGuard() noexcept
    {
        g_allocationGuardActive = previousActive;
        g_allocationCount = previousCount;
    }

    ScopedAllocationGuard (const ScopedAllocationGuard&) = delete;
    ScopedAllocationGuard& operator= (const ScopedAllocationGuard&) = delete;

    // ガードが生きている間に呼ぶこと。
    std::size_t count() const noexcept { return g_allocationCount; }

private:
    bool previousActive;
    std::size_t previousCount;
};

} // namespace vc::test
