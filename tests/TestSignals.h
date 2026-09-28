#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

// ===== SECTION: TestSignals (土台) =====
// テスト全体で使う信号生成・解析のヘルパー。
// クリック判定器・フォルマント解析はT-004/T-005で追加する（docs/plan.md 3章参照）。
// FFTピーク検出（放物線補間つき）はT-003のR5（44.1k→48kHzのリサンプリング精度）のために先行追加した。

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

// n個のサンプル(定常区間)にHann窓をかけてFFTし、最大振幅の位置を放物線補間で求めた周波数(Hz)を返す。
// nは2の累乗である必要はない（内側でそれ以上の最小の2の累乗にゼロ埋めする）。
inline double findFftPeakHz (const float* data, int n, double sampleRate)
{
    int order = 1;
    while ((1 << order) < n)
        ++order;

    const int fftSize = 1 << order;

    juce::dsp::FFT fft (order);
    std::vector<float> buffer ((size_t) fftSize * 2, 0.0f);

    for (int i = 0; i < n && i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) (n - 1));
        buffer[(size_t) i] = (float) ((double) data[i] * w);
    }

    fft.performRealOnlyForwardTransform (buffer.data());

    const int numBins = fftSize / 2 + 1;

    const auto magAt = [&] (int bin)
    {
        const float re = buffer[(size_t) bin * 2];
        const float im = buffer[(size_t) bin * 2 + 1];
        return std::sqrt (re * re + im * im);
    };

    int peakBin = 1;
    float peakMag = 0.0f;

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        const float mag = magAt (bin);

        if (mag > peakMag)
        {
            peakMag = mag;
            peakBin = bin;
        }
    }

    const double alpha = magAt (peakBin - 1);
    const double beta = magAt (peakBin);
    const double gamma = magAt (peakBin + 1);
    const double denom = alpha - 2.0 * beta + gamma;
    const double delta = denom != 0.0 ? 0.5 * (alpha - gamma) / denom : 0.0;

    return ((double) peakBin + delta) * sampleRate / (double) fftSize;
}

} // namespace vc::test
