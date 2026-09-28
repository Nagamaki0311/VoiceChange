#pragma once

#include <cmath>
#include <vector>

// ===== SECTION: TestSignals (土台) =====
// テスト全体で使う信号生成・解析のヘルパー。T-002時点では正弦生成と有限値検査のみ。
// クリック判定器・フォルマント解析・FFTピーク検出はT-004/T-005で追加する（docs/plan.md 3章参照）。

namespace vc::test
{

// 振幅ampのfreqHz正弦波をnサンプル生成する（位相は0から開始）。
inline std::vector<float> makeSine (double freqHz, double sampleRate, int n, float amp = 1.0f)
{
    std::vector<float> out (static_cast<size_t> (n));
    constexpr double twoPi = 6.283185307179586476925286766559;
    const double phaseInc = twoPi * freqHz / sampleRate;
    double phase = 0.0;

    for (int i = 0; i < n; ++i)
    {
        out[static_cast<size_t> (i)] = static_cast<float> (amp * std::sin (phase));
        phase += phaseInc;
    }

    return out;
}

// バッファ全体が有限値かどうかを調べる。
inline bool allFinite (const float* data, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (data[i]))
            return false;

    return true;
}

} // namespace vc::test
