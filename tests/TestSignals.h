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

// ===== SECTION: マイク処理用の合成信号・測定（docs/plan.md 8.3 T-010） =====

// 話し声に近い合成音節（N0c・N0e・N6用）: f0 110〜140Hz・ビブラート±2%（5Hz）の母音（フォルマント700/1200/2600Hz）を、
// 長さの異なる音節（半波の包絡）と50msの無音を交互に並べる。包絡が不規則でf0が揺れるため、相互相関のピークが
// 真の遅延の1点にだけ立つ（f0の周期ずれや包絡の周期性による誤ったピークを避ける）。値はint16の値域。
inline std::vector<float> makeSyllables (int n, double sampleRate = 48000.0)
{
    static constexpr int kSyllableLengths[] = { 7200, 4800, 9600, 3600, 8400, 6000, 10800, 5400, 7800, 4200, 9000, 6600 };
    constexpr double twoPi = 6.283185307179586476925286766559;

    // 長さは48kHzでのサンプル数。他のレートでは同じ時間長にする。
    const double scale = sampleRate / 48000.0;
    const int gap = (int) std::lround (2400.0 * scale);

    std::vector<float> out ((size_t) n, 0.0f);
    double phase = 0.0;
    int pos = 0;
    int syllable = 0;

    while (pos < n)
    {
        const int len = (int) std::lround (kSyllableLengths[syllable % 12] * scale);
        const double f0Base = 110.0 + 15.0 * (syllable % 3);
        ++syllable;

        for (int i = 0; i < len && pos + i < n; ++i)
        {
            const double t = (pos + i) / sampleRate;
            const double f0 = f0Base * (1.0 + 0.02 * std::sin (twoPi * 5.0 * t));
            phase += twoPi * f0 / sampleRate;

            double sum = 0.0;

            for (int h = 1; h <= 30; ++h)
            {
                const double f = h * f0;
                const double g = std::exp (-std::pow ((f - 700.0) / 300.0, 2.0))
                                 + 0.6 * std::exp (-std::pow ((f - 1200.0) / 400.0, 2.0))
                                 + 0.2 * std::exp (-std::pow ((f - 2600.0) / 600.0, 2.0));
                sum += g * std::sin (h * phase);
            }

            out[(size_t) (pos + i)] = (float) (4000.0 * sum * std::sin (juce::MathConstants<double>::pi * i / len));
        }

        pos += len + gap;
    }

    return out;
}

// 途切れない話し声に近い母音: f0のビブラート±2%（5Hz）、4Hzの音節抑揚（振幅 0.7 ± 0.3）、フォルマント730/1090/2440Hz。
// 倍音の振幅は3つの2極共振器のk*f0での応答の積（makeSyntheticVowelと同じ）。ピークがampになるよう正規化する。
inline std::vector<float> makeSpeechLikeVowel (double f0Hz, double sampleRate, int n, float amp = 0.3f)
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    constexpr std::array<double, 3> formantHz { 730.0, 1090.0, 2440.0 };
    constexpr std::array<double, 3> formantBw { 80.0, 90.0, 120.0 };

    // 6kHz以上は共振器の応答が小さいため、倍音は6kHzまでにする（計算量の削減）。
    const int maxHarmonic = std::max (1, (int) std::floor (std::min (6000.0, 0.45 * sampleRate) / f0Hz));
    std::vector<double> harmonicAmp ((size_t) maxHarmonic);

    for (int k = 1; k <= maxHarmonic; ++k)
    {
        double a = 1.0;

        for (size_t i = 0; i < 3; ++i)
            a *= resonatorMagnitude ((double) k * f0Hz, formantHz[i], formantBw[i], sampleRate);

        harmonicAmp[(size_t) (k - 1)] = a;
    }

    std::vector<float> out ((size_t) n);
    double phase = 0.0;
    double peak = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate;
        phase += twoPi * f0Hz * (1.0 + 0.02 * std::sin (twoPi * 5.0 * t)) / sampleRate;

        double sum = 0.0;

        for (int k = 1; k <= maxHarmonic; ++k)
            sum += harmonicAmp[(size_t) (k - 1)] * std::sin ((double) k * phase);

        sum *= 0.7 + 0.3 * std::sin (twoPi * 4.0 * t);
        out[(size_t) i] = (float) sum;
        peak = std::max (peak, std::abs (sum));
    }

    if (peak > 0.0)
        for (auto& s : out)
            s = (float) ((double) s * (double) amp / peak);

    return out;
}

// RMSがrmsTargetになるよう全体を定数倍する。
inline void normalizeRms (std::vector<float>& buf, float rmsTarget)
{
    const double r = rms (buf.data(), (int) buf.size());

    if (r > 0.0)
        for (auto& s : buf)
            s = (float) ((double) s * (double) rmsTarget / r);
}

// ピンク雑音（Paul Kellet's refined method）。RMSがrmsになるよう正規化する。
inline std::vector<float> makePinkNoise (int n, float rmsLevel, int seed = 1)
{
    std::vector<float> out ((size_t) n);
    juce::Random rng (seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    for (auto& s : out)
    {
        const double white = (double) rng.nextFloat() * 2.0 - 1.0;
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        s = (float) (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362);
        b6 = white * 0.115926;
    }

    normalizeRms (out, rmsLevel);
    return out;
}

// ブラウン雑音（白色雑音の漏れ積分）。RMSがrmsLevelになるよう正規化する。
inline std::vector<float> makeBrownNoise (int n, float rmsLevel, int seed = 2)
{
    std::vector<float> out ((size_t) n);
    juce::Random rng (seed);
    double y = 0.0;

    for (auto& s : out)
    {
        y = 0.998 * y + ((double) rng.nextFloat() * 2.0 - 1.0);
        s = (float) y;
    }

    normalizeRms (out, rmsLevel);
    return out;
}

// bufのstartSampleから、freqHzの正弦をdecayMs（時定数）で指数減衰させたクリックを足す（打鍵・マウスクリックの模擬）。
inline void addClick (std::vector<float>& buf, int startSample, double sampleRate, double freqHz, double decayMs, float peak)
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    const double tau = decayMs * 1.0e-3 * sampleRate;
    const int len = (int) (tau * 8.0);

    for (int i = 0; i < len && startSample + i < (int) buf.size(); ++i)
        buf[(size_t) (startSample + i)] += (float) ((double) peak * std::exp (-(double) i / tau) * std::sin (twoPi * freqHz * (double) i / sampleRate));
}

// 相互相関による遅延の測定。ref[i]とtest[i + lag]の相関が最大のlag（放物線補間つき、サンプル）を返す。
// 探索範囲は[minLag, maxLag]。相関は両端を除いた区間（[maxLag, n - maxLag)）で取る。
inline double measureDelaySamples (const std::vector<float>& ref, const std::vector<float>& test, int minLag, int maxLag)
{
    const int n = (int) std::min (ref.size(), test.size());
    const int begin = std::max (0, maxLag);
    const int end = n - maxLag;

    std::vector<double> corr ((size_t) (maxLag - minLag + 1), 0.0);

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double c = 0.0;

        for (int i = begin; i < end; ++i)
            c += (double) ref[(size_t) i] * (double) test[(size_t) (i + lag)];

        corr[(size_t) (lag - minLag)] = c;
    }

    const auto best = (int) (std::max_element (corr.begin(), corr.end()) - corr.begin());
    double delta = 0.0;

    if (best > 0 && best + 1 < (int) corr.size())
    {
        const double a = corr[(size_t) (best - 1)];
        const double b = corr[(size_t) best];
        const double c = corr[(size_t) (best + 1)];
        const double denom = a - 2.0 * b + c;
        delta = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
    }

    return (double) (minLag + best) + delta;
}

// [loHz, hiHz]のパワー（平均スペクトルの振幅の2乗和。dB、任意の基準）。
inline double bandPowerDb (const std::vector<double>& avgMag, double sampleRate, int fftSize, double loHz, double hiHz)
{
    const double binHz = sampleRate / (double) fftSize;
    double p = 0.0;

    for (int b = std::max (1, (int) std::ceil (loHz / binHz)); b < (int) avgMag.size() && (double) b * binHz <= hiHz; ++b)
        p += avgMag[(size_t) b] * avgMag[(size_t) b];

    return 10.0 * std::log10 (std::max (p, 1.0e-30));
}


} // namespace vc::test
