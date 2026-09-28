#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vc
{

namespace
{
constexpr double kTargetRate = 12000.0;
constexpr double kMinHz = 70.0;
constexpr double kMaxHz = 1000.0;
constexpr double kWindowSeconds = 0.021;
constexpr double kHopSeconds = 0.005;
constexpr float kThreshold = 0.15f;
constexpr double kSegmentRatioMin = 0.6; // 区間RMSの最小/最大がこれ未満なら推定を更新しない
constexpr float kRmsThreshold = 0.003f; // 約-50dBFS。これ未満は無声
// 6次Butterworthの各2次段のQ
constexpr std::array<float, 3> kButterQ { 0.5176381f, 0.7071068f, 1.9318517f };
} // namespace

void PitchDetector::prepare (double sampleRate, int maxBlockSamples)
{
    juce::ignoreUnused (maxBlockSamples);

    decimation = std::max (1, (int) std::lround (sampleRate / kTargetRate));
    decimatedRate = sampleRate / (double) decimation;

    windowLen = (int) std::lround (kWindowSeconds * decimatedRate);
    tauMin = std::max (2, (int) std::floor (decimatedRate / kMaxHz));
    tauMax = (int) std::ceil (decimatedRate / kMinHz);
    hopLen = std::max (1, (int) std::lround (kHopSeconds * decimatedRate));

    const int frameLen = windowLen + tauMax + 1;
    int ringLen = 1;
    while (ringLen < frameLen)
        ringLen <<= 1;

    ring.assign ((size_t) ringLen, 0.0f);
    ringMask = ringLen - 1;
    frame.assign ((size_t) frameLen, 0.0f);
    cmnd.assign ((size_t) tauMax + 2, 0.0f);

    // カットオフは間引き後のレートの0.25倍（12kHzで約3kHz）。基本波と低次倍音は残り、折り返しを抑える。
    const double cutoff = 0.25 * decimatedRate;
    for (size_t i = 0; i < antiAlias.size(); ++i)
        antiAlias[i].coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, (float) cutoff, kButterQ[i]);

    reset();
}

void PitchDetector::reset() noexcept
{
    for (auto& f : antiAlias)
        f.reset();

    std::fill (ring.begin(), ring.end(), 0.0f);
    ringPos = 0;
    ringFilled = 0;
    decimCounter = 0;
    hopCounter = 0;
    frequencyHz = 0.0f;
    voicing = 0.0f;
}

void PitchDetector::process (const float* in, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        float x = in[i];
        for (auto& f : antiAlias)
            x = f.processSample (x);

        if (++decimCounter < decimation)
            continue;

        decimCounter = 0;
        ring[(size_t) ringPos] = x;
        ringPos = (ringPos + 1) & ringMask;
        ringFilled = std::min (ringFilled + 1, (int) frame.size());

        if (++hopCounter >= hopLen)
        {
            hopCounter = 0;

            if (ringFilled >= (int) frame.size())
                estimate();
        }
    }
}

void PitchDetector::estimate() noexcept
{
    const int frameLen = (int) frame.size();

    for (int i = 0; i < frameLen; ++i)
        frame[(size_t) i] = ring[(size_t) ((ringPos - frameLen + i) & ringMask)];

    const float* x = frame.data();

    // 最新の積分窓のRMS。閾値未満は無声（有声度0）。
    // 窓を前半・後半の2分割にし、区間ごとのRMSが大きく違う（無音への減衰・発声の立ち上がり）フレームは、
    // 窓に半端な無音が混ざって周期推定が偏るため、推定を更新せず前回の値を保つ。
    constexpr int kSegments = 2;
    const int segLen = windowLen / kSegments;
    double totalSq = 0.0;
    double segMin = 1.0e30, segMax = 0.0;

    for (int seg = 0; seg < kSegments; ++seg)
    {
        const int start = frameLen - windowLen + seg * segLen;
        const int end = seg == kSegments - 1 ? frameLen : start + segLen;
        double sumSq = 0.0;
        for (int j = start; j < end; ++j)
            sumSq += (double) x[j] * (double) x[j];

        totalSq += sumSq;
        const double segMs = sumSq / (double) (end - start);
        segMin = std::min (segMin, segMs);
        segMax = std::max (segMax, segMs);
    }

    if (std::sqrt (totalSq / (double) windowLen) < (double) kRmsThreshold)
    {
        voicing = 0.0f;
        return;
    }

    if (segMin < kSegmentRatioMin * kSegmentRatioMin * segMax)
        return;

    // 差分関数 d(tau) → 累積平均正規化差分関数 d'(tau)。放物線補間のためtauMax+1まで求める。
    cmnd[0] = 1.0f;
    double runningSum = 0.0;

    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < windowLen; ++j)
        {
            const double diff = (double) x[j] - (double) x[j + tau];
            d += diff * diff;
        }

        runningSum += d;
        cmnd[(size_t) tau] = runningSum > 0.0 ? (float) (d * (double) tau / runningSum) : 1.0f;
    }

    // 絶対閾値: tauMin以降で最初にd'が閾値を下回った点から局所最小まで進む。なければ範囲内の最小値。
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (cmnd[(size_t) tau] < kThreshold)
        {
            while (tau + 1 <= tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }

    const bool voiced = best > 0;

    if (! voiced)
    {
        best = tauMin;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    const float dBest = cmnd[(size_t) best];
    const float rawVoicing = juce::jlimit (0.0f, 1.0f, 1.0f - dBest);
    voicing = voiced ? rawVoicing : 0.5f * rawVoicing; // 無声は必ず0.5未満

    if (! voiced)
        return; // 周波数は直前の値を保持

    // 放物線補間
    const float a = cmnd[(size_t) best - 1];
    const float b = cmnd[(size_t) best];
    const float c = cmnd[(size_t) best + 1];
    const float denom = a - 2.0f * b + c;
    const float delta = denom > 1.0e-12f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (a - c) / denom) : 0.0f;

    frequencyHz = (float) (decimatedRate / ((double) best + (double) delta));
}

} // namespace vc
