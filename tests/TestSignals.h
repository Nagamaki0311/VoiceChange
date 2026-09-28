#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

// ===== SECTION: TestSignals =====
// テスト全体で使う信号生成・解析のヘルパー。
// docs/plan.md 2.5節・3章T-004/T-005・5章参照。

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

inline double rms (const float* data, int n)
{
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i)
        sumSq += (double) data[i] * (double) data[i];
    return n > 0 ? std::sqrt (sumSq / (double) n) : 0.0;
}

inline double peakAbs (const float* data, int n)
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::abs ((double) data[i]));
    return m;
}

// -80dBFS程度の雑音を全体に混ぜる（Stretchのデジタル無音モード[入力エネルギー<1e-15が
// 2ブロック続く]を避けるため。docs/plan.md 3章T-004クリック判定器の節参照）。
inline void addNoiseFloor (std::vector<float>& buf, float ampDbfs = -80.0f, int seed = 999)
{
    const float amp = (float) std::pow (10.0, (double) ampDbfs / 20.0);
    juce::Random rng (seed);

    for (auto& s : buf)
        s += amp * (rng.nextFloat() * 2.0f - 1.0f);
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

// ===== SECTION: クリック判定器 =====
// 切替区間の隣接差最大値 <= 1.5 * max(切替前の定常区間の隣接差最大値, 切替後の同値)。
// 定常区間は各1秒程度を想定（シフターの遅延が落ち着いた後から取る）。

inline double maxAdjacentDiff (const float* data, int n)
{
    double m = 0.0;
    for (int i = 1; i < n; ++i)
        m = std::max (m, std::abs ((double) data[i] - (double) data[i - 1]));
    return m;
}

// steadyBefore/steadyAfterは長さsteadyLenの定常区間（切替の前後）、transitionは切替区間。
inline bool checkNoClick (const float* steadyBefore, int steadyLen,
                           const float* transition, int transitionLen,
                           const float* steadyAfter)
{
    const double before = maxAdjacentDiff (steadyBefore, steadyLen);
    const double after = maxAdjacentDiff (steadyAfter, steadyLen);
    const double threshold = 1.5 * std::max (before, after);
    const double transDiff = maxAdjacentDiff (transition, transitionLen);
    return transDiff <= threshold;
}

// ===== SECTION: 合成母音・フォルマント測定（docs/plan.md T-005節の測定法。P4のため先行実装） =====

// 2極共振器（direct-form: y[n] = x[n] + a1*y[n-1] + a2*y[n-2]）の周波数freqHzでの振幅応答。
inline double resonatorMagnitude (double freqHz, double centerHz, double bandwidthHz, double sampleRate)
{
    const double r = std::exp (-juce::MathConstants<double>::pi * bandwidthHz / sampleRate);
    const double theta = 2.0 * juce::MathConstants<double>::pi * centerHz / sampleRate;
    const double a1 = 2.0 * r * std::cos (theta);
    const double a2 = -r * r;

    const double omega = 2.0 * juce::MathConstants<double>::pi * freqHz / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -omega);
    const std::complex<double> z2 = std::polar (1.0, -2.0 * omega);
    const std::complex<double> denom = 1.0 - a1 * z1 - a2 * z2;
    return 1.0 / std::abs (denom);
}

// 加算合成の合成母音。k次倍音の振幅 = 3つの2極共振器(F1/F2/F3)のk*f0での振幅応答の積（plan.md T-005節）。
// 出力全体をピーク振幅ampへ正規化する。
inline std::vector<float> makeSyntheticVowel (double f0Hz, double sampleRate, int n,
                                               std::array<double, 3> formantHz = { 730.0, 1090.0, 2440.0 },
                                               std::array<double, 3> formantBw = { 80.0, 90.0, 120.0 },
                                               float amp = 0.3f)
{
    std::vector<float> out ((size_t) n, 0.0f);

    const double nyquist = sampleRate * 0.5;
    const int maxHarmonic = std::max (1, (int) std::floor (nyquist / f0Hz) - 1);

    std::vector<double> harmonicAmp ((size_t) maxHarmonic, 0.0);

    for (int k = 1; k <= maxHarmonic; ++k)
    {
        const double freq = (double) k * f0Hz;
        double a = 1.0;

        for (int i = 0; i < 3; ++i)
            a *= resonatorMagnitude (freq, formantHz[(size_t) i], formantBw[(size_t) i], sampleRate);

        harmonicAmp[(size_t) (k - 1)] = a;
    }

    constexpr double twoPi = 6.283185307179586476925286766559;

    for (int i = 0; i < n; ++i)
    {
        double sample = 0.0;
        const double phaseBase = twoPi * f0Hz * (double) i / sampleRate;

        for (int k = 1; k <= maxHarmonic; ++k)
            sample += harmonicAmp[(size_t) (k - 1)] * std::sin (phaseBase * (double) k);

        out[(size_t) i] = (float) sample;
    }

    const double peak = peakAbs (out.data(), n);
    if (peak > 0.0)
    {
        const float scale = (float) (amp / peak);
        for (auto& s : out)
            s *= scale;
    }

    return out;
}

// n サンプル(定常区間)を50%オーバーラップのHann窓・16384点FFT(既定)で平均した振幅スペクトルを返す
// （インデックス=bin、値=平均振幅。plan.md T-005節「定常区間1秒以上をHann窓の16384点FFTで平均」）。
inline std::vector<double> averagedMagnitudeSpectrum (const float* data, int n, int fftOrder = 14)
{
    const int fftSize = 1 << fftOrder;
    const int numBins = fftSize / 2 + 1;

    juce::dsp::FFT fft (fftOrder);
    std::vector<double> avgMag ((size_t) numBins, 0.0);
    int numFrames = 0;

    const int hop = fftSize / 2;
    const int usableLen = std::min (n, fftSize);

    const auto accumulateFrame = [&] (int start, int len)
    {
        std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
        const int denom = std::max (1, len - 1);

        for (int i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) denom);
            buf[(size_t) i] = (float) ((double) data[start + i] * w);
        }

        fft.performRealOnlyForwardTransform (buf.data());

        for (int b = 0; b < numBins; ++b)
        {
            const float re = buf[(size_t) b * 2];
            const float im = buf[(size_t) b * 2 + 1];
            avgMag[(size_t) b] += std::sqrt ((double) re * re + (double) im * im);
        }

        ++numFrames;
    };

    if (n >= fftSize)
    {
        for (int start = 0; start + fftSize <= n; start += hop)
            accumulateFrame (start, fftSize);
    }
    else
    {
        accumulateFrame (0, usableLen);
    }

    for (auto& m : avgMag)
        m /= (double) std::max (1, numFrames);

    return avgMag;
}

struct SpectralPeak
{
    double freqHz;
    double magLinear;
};

// avgMag（averagedMagnitudeSpectrumの戻り値）からapproxFreqHzの±toleranceRatio内で最大の
// ピークを探し、放物線補間で周波数・振幅を求める。見つからなければfreqHz<0を返す。
inline SpectralPeak findPeakNear (const std::vector<double>& avgMag, double sampleRate, int fftSize,
                                   double approxFreqHz, double toleranceRatio = 0.03)
{
    const double binHz = sampleRate / (double) fftSize;
    const int numBins = (int) avgMag.size();

    const double lo = approxFreqHz * (1.0 - toleranceRatio);
    const double hi = approxFreqHz * (1.0 + toleranceRatio);

    int binLo = std::max (1, (int) std::floor (lo / binHz));
    int binHi = std::min (numBins - 2, (int) std::ceil (hi / binHz));

    if (binLo > binHi)
        return { -1.0, 0.0 };

    int bestBin = -1;
    double bestMag = -1.0;

    for (int b = binLo; b <= binHi; ++b)
    {
        if (avgMag[(size_t) b] > bestMag)
        {
            bestMag = avgMag[(size_t) b];
            bestBin = b;
        }
    }

    if (bestBin <= 0 || bestBin >= numBins - 1)
        return { -1.0, 0.0 };

    const double alpha = avgMag[(size_t) (bestBin - 1)];
    const double beta = avgMag[(size_t) bestBin];
    const double gamma = avgMag[(size_t) (bestBin + 1)];
    const double denom = alpha - 2.0 * beta + gamma;
    const double delta = denom != 0.0 ? 0.5 * (alpha - gamma) / denom : 0.0;

    const double peakFreq = ((double) bestBin + delta) * binHz;
    const double peakMag = beta - 0.25 * (alpha - gamma) * delta;

    return { peakFreq, std::max (peakMag, 1.0e-12) };
}

// 基本波ピーク（放物線補間、plan.md「f0比は基本波ピークの放物線補間」）。
inline double measureFundamentalHz (const float* data, int n, double sampleRate, double approxF0Hz, int fftOrder = 14)
{
    const auto avgMag = averagedMagnitudeSpectrum (data, n, fftOrder);
    const auto peak = findPeakNear (avgMag, sampleRate, 1 << fftOrder, approxF0Hz, 0.1);
    return peak.freqHz > 0.0 ? peak.freqHz : approxF0Hz;
}

// 対数周波数軸上の区分線形包絡 E(f)（plan.md「対数振幅を対数周波数軸上で線形補間」）。
struct FormantEnvelope
{
    std::vector<double> logFreq;
    std::vector<double> logMag;

    double evaluate (double freqHz) const
    {
        if (logFreq.empty())
            return -300.0;

        const double lf = std::log (freqHz);

        if (lf <= logFreq.front())
            return logMag.front();
        if (lf >= logFreq.back())
            return logMag.back();

        for (size_t i = 1; i < logFreq.size(); ++i)
        {
            if (lf <= logFreq[i])
            {
                const double t = (lf - logFreq[i - 1]) / (logFreq[i] - logFreq[i - 1]);
                return logMag[i - 1] + t * (logMag[i] - logMag[i - 1]);
            }
        }

        return logMag.back();
    }
};

// f0の各倍音位置±3%でピークを取り、200〜4000Hzの範囲で包絡を作る（plan.md T-005節）。
inline FormantEnvelope measureFormantEnvelope (const float* data, int n, double sampleRate, double f0Hz,
                                                double freqLo = 200.0, double freqHi = 4000.0, int fftOrder = 14)
{
    const int fftSize = 1 << fftOrder;
    const auto avgMag = averagedMagnitudeSpectrum (data, n, fftOrder);

    FormantEnvelope env;

    for (int k = 1;; ++k)
    {
        const double nominal = (double) k * f0Hz;
        if (nominal > freqHi)
            break;

        if (nominal >= freqLo)
        {
            const auto peak = findPeakNear (avgMag, sampleRate, fftSize, nominal, 0.03);

            if (peak.freqHz > 0.0)
            {
                env.logFreq.push_back (std::log (peak.freqHz));
                env.logMag.push_back (std::log (peak.magLinear));
            }
        }
    }

    return env;
}

// 包絡スケールs：E_out(s*f)とE_in(f)の相関が最大になるsを[0.5, 2]で0.5%刻みに探す（plan.md T-005節）。
inline double measureEnvelopeScale (const FormantEnvelope& envIn, const FormantEnvelope& envOut,
                                     double freqLo = 200.0, double freqHi = 4000.0, int numPoints = 200)
{
    if (envIn.logFreq.empty() || envOut.logFreq.empty())
        return 1.0;

    std::vector<double> freqs ((size_t) numPoints);
    const double logLo = std::log (freqLo);
    const double logHi = std::log (freqHi);

    for (int i = 0; i < numPoints; ++i)
        freqs[(size_t) i] = std::exp (logLo + (logHi - logLo) * (double) i / (double) (numPoints - 1));

    double bestS = 1.0;
    double bestCorr = -1.0e300;

    for (double s = 0.5; s <= 2.0 + 1.0e-9; s += 0.005)
    {
        double meanA = 0.0, meanB = 0.0;
        std::vector<double> a ((size_t) numPoints), b ((size_t) numPoints);

        for (int i = 0; i < numPoints; ++i)
        {
            a[(size_t) i] = envIn.evaluate (freqs[(size_t) i]);
            b[(size_t) i] = envOut.evaluate (s * freqs[(size_t) i]);
            meanA += a[(size_t) i];
            meanB += b[(size_t) i];
        }

        meanA /= numPoints;
        meanB /= numPoints;

        double num = 0.0, denA = 0.0, denB = 0.0;

        for (int i = 0; i < numPoints; ++i)
        {
            const double da = a[(size_t) i] - meanA;
            const double db = b[(size_t) i] - meanB;
            num += da * db;
            denA += da * da;
            denB += db * db;
        }

        const double corr = (denA > 0.0 && denB > 0.0) ? num / std::sqrt (denA * denB) : -1.0;

        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestS = s;
        }
    }

    return bestS;
}

} // namespace vc::test
