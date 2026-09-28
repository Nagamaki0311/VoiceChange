#include <juce_core/juce_core.h>

#include "AllocationGuard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

// ===== SECTION: アロケーション検出フック =====
// グローバルのoperator new/new[]/aligned版/nothrow版と対応するdeleteを置き換え、
// ScopedAllocationGuardが有効な間（thread_local）の呼び出し回数を数える（docs/plan.md 5章）。
// 実際のメモリ確保・解放はstd::malloc/std::freeで行い、再帰しないようにする。
// アラインメント付き確保は、要求サイズの前後に管理領域を足して手動でアラインする
// （プラットフォーム差のあるstd::aligned_alloc/_aligned_mallocに依存しない）。

namespace vc::test
{
thread_local bool g_allocationGuardActive = false;
thread_local std::size_t g_allocationCount = 0;
} // namespace vc::test

namespace
{
inline void noteAllocationEvent() noexcept
{
    if (vc::test::g_allocationGuardActive)
        ++vc::test::g_allocationCount;
}

// アラインメント付き確保: [パディング][元ポインタ][アラインされた領域...] の形で1回のmallocにまとめる。
void* alignedAllocate (std::size_t size, std::size_t alignment)
{
    if (alignment < sizeof (void*))
        alignment = sizeof (void*);

    const std::size_t total = size + alignment + sizeof (void*);
    void* raw = std::malloc (total);

    if (raw == nullptr)
        return nullptr;

    const auto rawAddr = reinterpret_cast<std::uintptr_t> (raw) + sizeof (void*);
    const auto alignedAddr = (rawAddr + alignment - 1) & ~(alignment - 1);
    auto* aligned = reinterpret_cast<void*> (alignedAddr);
    reinterpret_cast<void**> (aligned)[-1] = raw;
    return aligned;
}

void alignedFree (void* p) noexcept
{
    if (p == nullptr)
        return;

    void* raw = reinterpret_cast<void**> (p)[-1];
    std::free (raw);
}
} // namespace

void* operator new (std::size_t size)
{
    noteAllocationEvent();

    if (void* p = std::malloc (size > 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    return std::malloc (size > 0 ? size : 1);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    return std::malloc (size > 0 ? size : 1);
}

void* operator new (std::size_t size, std::align_val_t alignment)
{
    noteAllocationEvent();

    if (void* p = alignedAllocate (size, static_cast<std::size_t> (alignment)))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return ::operator new (size, alignment);
}

void* operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    return alignedAllocate (size, static_cast<std::size_t> (alignment));
}

void* operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    return alignedAllocate (size, static_cast<std::size_t> (alignment));
}

void operator delete (void* p) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete[] (void* p) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete (void* p, std::size_t) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete[] (void* p, std::size_t) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete (void* p, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete[] (void* p, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    std::free (p);
}

void operator delete (void* p, std::align_val_t) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

void operator delete[] (void* p, std::align_val_t) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

void operator delete (void* p, std::size_t, std::align_val_t) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    noteAllocationEvent();
    alignedFree (p);
}

// ===== SECTION: TestMain =====
// juce::UnitTestランナー。`--category <名前>`で対象カテゴリを絞ってctestに複数登録する。

int main (int argc, char* argv[])
{
    juce::String category;

    for (int i = 1; i < argc; ++i)
    {
        if (juce::String (argv[i]) == "--category" && i + 1 < argc)
        {
            category = juce::String (argv[++i]);
        }
    }

    if (category.isEmpty())
    {
        std::fprintf (stderr, "usage: %s --category <name>\n", argv[0]);
        return 1;
    }

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory (category);

    const int numResults = runner.getNumResults();

    if (numResults == 0)
    {
        std::fprintf (stderr, "no tests ran for category '%s'\n", category.toRawUTF8());
        return 1;
    }

    int totalFailures = 0;

    for (int i = 0; i < numResults; ++i)
    {
        if (const auto* result = runner.getResult (i))
            totalFailures += result->failures;
    }

    return totalFailures == 0 ? 0 : 1;
}
