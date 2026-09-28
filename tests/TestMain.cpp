#include <juce_core/juce_core.h>

#include <cstdio>

// ===== SECTION: TestMain =====
// juce::UnitTestランナー。`--category <名前>`で対象カテゴリを絞ってctestに複数登録する。
// アロケーション検出フック（operator new/delete置き換え）はT-003で追加する（docs/plan.md 5章参照）。

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
