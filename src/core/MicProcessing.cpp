#include "MicProcessing.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vc
{

namespace
{
constexpr double kNativeRate = 48000.0;
constexpr int kFrame = 480;            // RNNoiseの1フレーム（48kHzで10ms）
constexpr int kRnnoiseDelay = 960;     // RNNoise固有の遅延（サンプル@48kHz）。T-009のN0cの実測値
// 同じ遅延の相互相関による実測（放物線補間）は959.71。整数に丸めた誤差はレート比に比例して大きくなる（192kHzで1サンプル超）
// ため、48kHz以外ではこちらを出力レートへ換算する。
constexpr double kRnnoiseDelayMeasured = 959.7;
constexpr int kInterpLatency = 2;      // juce::LagrangeInterpolatorのアルゴリズム遅延（入力側のサンプル数）
constexpr int kPrefillMargin = 4;      // 48kHz以外の出力FIFOの初期充填の余裕。これに 2×ceil(fs/48000) を足す（下げ側の持ち越し［最大 downNeed 個の48kHzサンプル = downNeed×fs/48000 サンプル］がレート比に比例するため）
constexpr double kFadeSeconds = 0.020;
constexpr double kMixRampSeconds = 0.050;
constexpr float kInt16Scale = 32768.0f;
constexpr float kInt16ScaleInv = 1.0f / 32768.0f;

// ゲート（spec.md「マイク処理: ノイズ除去」5）
constexpr float kGateMaxAttenuationDb = 18.0f;
constexpr float kGateOpenVad = 0.6f;
constexpr float kGateCloseVad = 0.3f;
constexpr double kGateOpenPower = 15.848931924611133;  // 12dB（パワー比）
// ゲートを閉じる側の余裕（雑音床+8dB。パワー比）。開く側（+12dB）との間に4dBのヒステリシスを持つ。
// 当初の+6dBは、ピンク雑音のように10ms窓のパワーが揺れる雑音で閉じにくかった（T-011の実測）。
constexpr double kGateClosePower = 6.3095734448019325;
constexpr double kGateRmsWindowSeconds = 0.010;
constexpr double kGateHoldSeconds = 0.200;
constexpr double kGateOpenSeconds = 0.005;
constexpr double kGateCloseSeconds = 0.150;
constexpr double kGateSegmentSeconds = 0.100;          // 雑音床の2秒間の最小値 = 100msごとの最小値20個
constexpr double kGateFloorRiseDbPerSecond = 3.0;
// ponytail: 雑音床の下限（-90dBFS）。デジタル無音［マイクのミュート等］で床が0になると、以後3dB/秒でしか上がらず
// ゲートが長く開いたままになるため。改善案: 床の上昇を、下限付近だけ速くする。
constexpr double kGateFloorMinPower = 1.0e-9;
constexpr double kHuge = 1.0e300;

// インパクト抑制（同6）
constexpr double kImpactLookaheadSeconds = 0.002;
constexpr double kImpactFastAttackSeconds = 0.0001;
constexpr double kImpactFastReleaseSeconds = 0.002;
constexpr double kImpactSlowAttackSeconds = 0.020;
constexpr double kImpactSlowReleaseSeconds = 0.200;
constexpr double kImpactAttackSeconds = 0.001;
// 当初の15msは、立ち上がり10msの母音の最初の数msを検出したときに母音がピークに達する頃まで減衰が続き、N5bが閾値を外れた（T-011の実測）。
constexpr double kImpactHoldSeconds = 0.003;
constexpr double kImpactReleaseSeconds = 0.010;
constexpr float kImpactThresholdBaseDb = 30.0f;
constexpr float kImpactThresholdSlopeDb = 18.0f;
constexpr float kImpactGateOpenExtraDb = 6.0f;
constexpr float kImpactDepthDb = 24.0f;
// ponytail: 遅い包絡の下限（-80dBFS）。無音の直後の発話や、パイプライン開始直後に、比が無限大になって誤検出しないため。
// 副作用として、-80dBFSより十分小さい音の立ち上がりは検出しない（聞こえない）。
constexpr float kImpactSlowFloor = 1.0e-4f;

float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

// 1次の追従係数（時定数tauの指数）。
float followCoef (double tauSeconds, double fs) noexcept
{
    return (float) (1.0 - std::exp (-1.0 / (tauSeconds * fs)));
}

bool allFinite (const float* data, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (data[i]))
            return false;

    return true;
}
} // namespace

void NoiseReducer::RnnoiseDeleter::operator() (DenoiseState* st) const noexcept
{
    rnnoise_destroy (st);
}

// ===== SECTION: VadGate =====

void NoiseReducer::VadGate::prepare (double fs)
{
    window = std::max (1, (int) std::lround (kGateRmsWindowSeconds * fs));
    squares.assign ((size_t) window, 0.0);
    segmentLength = std::max (1, (int) std::lround (kGateSegmentSeconds * fs));
    holdSamples = std::max (1, (int) std::lround (kGateHoldSeconds * fs));
    openStep = (float) (1.0 / (kGateOpenSeconds * fs));
    closeStep = (float) (1.0 / (kGateCloseSeconds * fs));
    floorRise = std::pow (10.0, kGateFloorRiseDbPerSecond / 10.0 / fs); // パワーの上昇率
    reset();
}

void NoiseReducer::VadGate::reset() noexcept
{
    std::fill (squares.begin(), squares.end(), 0.0);
    segmentMins.fill (kHuge);
    sumSquares = 0.0;
    segmentMin = kHuge;
    minOfSegments = kHuge;
    floorPower = 0.0;
    gateGain = 1.0f;
    windowPos = 0;
    windowFill = 0;
    segmentCount = 0;
    segmentIndex = 0;
    holdCount = 0;
    floorValid = false;
    open = true;
    closedSamples = 0;
}

float NoiseReducer::VadGate::process (float dry, float vad, float closedGain, bool valid) noexcept
{
    if (! valid)
        return 1.0f;

    // 直近10msの短時間パワー。窓が埋まるまでは（遅延線から届いた分だけ）開いたままにする。
    const double sq = (double) dry * (double) dry;
    sumSquares += sq - squares[(size_t) windowPos];
    squares[(size_t) windowPos] = sq;

    if (++windowPos >= window)
        windowPos = 0;

    if (windowFill < window)
    {
        ++windowFill;
        return 1.0f;
    }

    const double power = std::max (sumSquares, 0.0) / (double) window;

    // 雑音床 = 短時間パワーの2秒間の最小値（100msごとの最小値の最小）。下がるときは即座に、上がるときは3dB/秒まで。
    segmentMin = std::min (segmentMin, power);

    if (++segmentCount >= segmentLength)
    {
        segmentMins[(size_t) segmentIndex] = segmentMin;
        segmentIndex = (segmentIndex + 1) % kSegments;
        segmentMin = kHuge;
        segmentCount = 0;
        minOfSegments = *std::min_element (segmentMins.begin(), segmentMins.end());

        // 加減算の丸めが積もらないよう、窓の合計を定期的に取り直す。
        sumSquares = 0.0;
        for (const double v : squares)
            sumSquares += v;
    }

    const double candidate = std::min (minOfSegments, segmentMin);

    if (! floorValid)
    {
        floorPower = candidate;
        floorValid = true;
    }
    else
    {
        floorPower = candidate < floorPower ? candidate : std::min (candidate, floorPower * floorRise);
    }

    floorPower = std::max (floorPower, kGateFloorMinPower);

    // 開閉の判定。開く条件はすぐ効き、閉じる条件はホールド（200ms）続いたときだけ効く。
    if (vad >= kGateOpenVad || power > floorPower * kGateOpenPower)
    {
        open = true;
        holdCount = 0;
    }
    else if (open)
    {
        if (vad < kGateCloseVad && power < floorPower * kGateClosePower)
        {
            if (++holdCount >= holdSamples)
                open = false;
        }
        else
        {
            holdCount = 0;
        }
    }

    if (! open && closedSamples < std::numeric_limits<int>::max())
        ++closedSamples; // テスト用の計数。長時間でも符号付き整数がオーバーフローしないよう頭打ちにする

    const float target = open ? 1.0f : closedGain;
    gateGain = target > gateGain ? std::min (target, gateGain + openStep) : std::max (target, gateGain - closeStep);
    return gateGain;
}

// ===== SECTION: ImpactSuppressor =====

void NoiseReducer::ImpactSuppressor::prepare (double fs)
{
    fastAttack = followCoef (kImpactFastAttackSeconds, fs);
    fastRelease = followCoef (kImpactFastReleaseSeconds, fs);
    slowAttack = followCoef (kImpactSlowAttackSeconds, fs);
    slowRelease = followCoef (kImpactSlowReleaseSeconds, fs);
    attackStep = (float) (1.0 / (kImpactAttackSeconds * fs));
    releaseStep = (float) (1.0 / (kImpactReleaseSeconds * fs));
    holdSamples = std::max (1, (int) std::lround (kImpactHoldSeconds * fs));
    depthGain.reset (fs, kMixRampSeconds);
    depthGain.setCurrentAndTargetValue (1.0f);
    reset();
}

void NoiseReducer::ImpactSuppressor::reset() noexcept
{
    fast = 0.0f;
    slow = 0.0f;
    latched = 1.0f;
    envelope = 1.0f;
    holdCount = 0;
    primed = false;
    attenuatedSamples = 0;
}

void NoiseReducer::ImpactSuppressor::setTarget (float impact, bool immediate) noexcept
{
    const float s = juce::jlimit (0.0f, 1.0f, impact);
    active = s > 0.0f;

    const float thresholdDb = kImpactThresholdBaseDb - kImpactThresholdSlopeDb * s;
    thresholdClosed = dbToGain (thresholdDb);
    thresholdOpen = dbToGain (thresholdDb + kImpactGateOpenExtraDb);

    const float depth = dbToGain (-kImpactDepthDb * s);

    if (immediate)
        depthGain.setCurrentAndTargetValue (depth); // 休止中の値の変化は補間しない
    else
        depthGain.setTargetValue (depth);
}

float NoiseReducer::ImpactSuppressor::process (float tap, bool gateOpen, bool valid) noexcept
{
    if (! valid)
        return 1.0f;

    const float a = std::abs (tap);

    if (! primed)
    {
        fast = a; // 最初の1サンプルで包絡を合わせる（0から立ち上がって誤検出しない）
        slow = a;
        primed = true;
    }
    else
    {
        fast += (a > fast ? fastAttack : fastRelease) * (a - fast);
        slow += (a > slow ? slowAttack : slowRelease) * (a - slow);
    }

    const float depth = depthGain.getNextValue();

    if (active && fast > std::max (slow, kImpactSlowFloor) * (gateOpen ? thresholdOpen : thresholdClosed))
    {
        const float wanted = gateOpen ? std::sqrt (depth) : depth; // 発話中は減衰量（dB）を半分
        latched = holdCount > 0 ? std::min (latched, wanted) : wanted;
        holdCount = holdSamples;
    }

    float target = 1.0f;

    if (active && holdCount > 0)
    {
        --holdCount;
        target = latched;
    }
    else
    {
        holdCount = 0;
    }

    envelope = target < envelope ? std::max (target, envelope - attackStep) : std::min (target, envelope + releaseStep);

    if (envelope < 1.0f && attenuatedSamples < std::numeric_limits<int>::max())
        ++attenuatedSamples;

    return envelope;
}

// ===== SECTION: NoiseReducer =====

void NoiseReducer::prepare (double newSampleRate, int newMaxBlockSamples)
{
    jassert (newSampleRate > 0.0 && newMaxBlockSamples > 0);

    sampleRate = newSampleRate;
    maxBlockSamples = newMaxBlockSamples;

    if (rnnoise == nullptr)
        rnnoise.reset (rnnoise_create (nullptr)); // 失敗時はnullptrのまま。setTarget()が稼働要求を無視する

    native = std::abs (sampleRate - kNativeRate) < 0.5;
    const double ratio = sampleRate / kNativeRate;
    upRatio = native ? 1.0 : ratio;
    downRatio = native ? 1.0 : 1.0 / ratio;

    // 1フレームを作るのに要る入力の上限（上げ側は480個の出力で最大 ceil(480*ratio) 個進む。余裕2）。
    upNeed = native ? kFrame : (int) std::ceil (kFrame * ratio) + 2;
    // 下げ側の1個の出力で進む入力の上限（補間位置は 1 + downRatio 未満。余裕1）。
    downNeed = (int) std::ceil (downRatio) + 1;

    if (native)
    {
        prefill = kFrame;
        delaySamples = prefill + kRnnoiseDelay;
    }
    else
    {
        prefill = (int) std::ceil (kFrame * ratio) + kPrefillMargin + 2 * (int) std::ceil (ratio);
        delaySamples = prefill + (int) std::lround (kRnnoiseDelayMeasured * ratio + kInterpLatency + kInterpLatency * ratio);
    }

    const int downOutCapacity = (int) std::ceil ((kFrame + downNeed) / downRatio) + 4;

    inQueue.allocate (upNeed + maxBlockSamples + 8);
    outQueue.allocate (prefill + maxBlockSamples + downOutCapacity + 8);
    vadQueue.allocate (prefill + maxBlockSamples + downOutCapacity + 8);
    dryLine.assign ((size_t) delaySamples, 0.0f);
    wetScratch.assign ((size_t) maxBlockSamples, 0.0f);
    vadScratch.assign ((size_t) maxBlockSamples, 0.0f);
    frameIn.assign ((size_t) kFrame, 0.0f);
    frameOut.assign ((size_t) kFrame, 0.0f);
    downStage.assign ((size_t) (kFrame + downNeed + 8), 0.0f);
    downOut.assign ((size_t) downOutCapacity, 0.0f);

    fadeLenSamples = std::max (1, (int) std::lround (kFadeSeconds * sampleRate));
    dSmoothed.reset (sampleRate, kMixRampSeconds);
    dSmoothed.setCurrentAndTargetValue (0.0f);
    closedGainSmoothed.reset (sampleRate, kMixRampSeconds);
    closedGainSmoothed.setCurrentAndTargetValue (1.0f);
    lookaheadSamples = std::min ((int) std::lround (kImpactLookaheadSeconds * sampleRate), delaySamples - 1);
    gate.prepare (sampleRate);
    impact.prepare (sampleRate);

    underflowCount = 0;
    reset();
}

void NoiseReducer::setTarget (bool run, float background, float impactAmount) noexcept
{
    runTarget = run && rnnoise != nullptr;

    // 原音の混合比 d = (1 - 2s)^2（s <= 0.5）。s > 0.5は完全なRNNoise出力（d = 0）でゲートが働く
    // （閉じたときの減衰量 = 18dB × (2s - 1)）。s <= 0.5はゲートなし（閉じたときのゲイン = 1）。
    const float s = juce::jlimit (0.0f, 1.0f, background);
    const float d = s <= 0.5f ? (1.0f - 2.0f * s) * (1.0f - 2.0f * s) : 0.0f;
    const float closedGain = dbToGain (-kGateMaxAttenuationDb * std::max (0.0f, 2.0f * s - 1.0f));
    const bool resting = state == State::Resting;

    if (resting) // 休止中の値の変化は補間しない（起動時に古い値から動かさない）
    {
        dSmoothed.setCurrentAndTargetValue (d);
        closedGainSmoothed.setCurrentAndTargetValue (closedGain);
    }
    else
    {
        dSmoothed.setTargetValue (d);
        closedGainSmoothed.setTargetValue (closedGain);
    }

    impact.setTarget (impactAmount, resting);
}

void NoiseReducer::clearPipeline() noexcept
{
    if (rnnoise != nullptr)
        rnnoise_init (rnnoise.get(), nullptr); // 確保しない（T-009のN0d）

    inQueue.clear();
    outQueue.clear();
    outQueue.push (nullptr, prefill);
    vadQueue.clear();
    vadQueue.fill (0.0f, prefill);
    frameVad = 0.0f;
    std::fill (dryLine.begin(), dryLine.end(), 0.0f);
    dryPos = 0;
    pipelineSamples = 0;
    gate.reset();
    impact.reset();
    upInterp.reset();
    downInterp.reset();
    downStaged = 0;
}

void NoiseReducer::reset() noexcept
{
    clearPipeline();
    state = State::Resting;
    gain = 0.0;
    primedSamples = 0;
    updateLatencyForUi();
}

void NoiseReducer::updateLatencyForUi() noexcept
{
    const int latency = (state == State::FadingIn || state == State::Active || state == State::FadingOut) ? delaySamples : 0;
    latencySamplesForUi.store (latency, std::memory_order_relaxed);
}

// 入力をFIFOへ入れ、フレームがそろうごとにRNNoiseを通して出力FIFOへ足す。
void NoiseReducer::feed (const float* in, int n) noexcept
{
    inQueue.push (in, n);

    if (native)
    {
        while (inQueue.size() >= kFrame)
        {
            const float* src = inQueue.readPtr();

            for (int i = 0; i < kFrame; ++i)
                frameIn[(size_t) i] = src[i] * kInt16Scale;

            inQueue.drop (kFrame);
            processFrame();
            outQueue.push (frameOut.data(), kFrame);
            vadQueue.fill (frameVad, kFrame);
        }

        return;
    }

    while (inQueue.size() >= upNeed)
    {
        const int used = upInterp.process (upRatio, inQueue.readPtr(), frameIn.data(), kFrame);
        inQueue.drop (used);

        for (int i = 0; i < kFrame; ++i)
            frameIn[(size_t) i] *= kInt16Scale;

        processFrame();

        std::memcpy (downStage.data() + downStaged, frameOut.data(), sizeof (float) * (size_t) kFrame);
        downStaged += kFrame;

        // 1個ずつ出し、進む入力が足りなくなる手前で止める。端数は次のフレームへ持ち越す。
        int consumed = 0;
        int produced = 0;

        while (downStaged - consumed >= downNeed)
        {
            consumed += downInterp.process (downRatio, downStage.data() + consumed, downOut.data() + produced, 1);
            ++produced;
        }

        downStaged -= consumed;
        std::memmove (downStage.data(), downStage.data() + consumed, sizeof (float) * (size_t) downStaged);
        outQueue.push (downOut.data(), produced);
        vadQueue.fill (frameVad, produced);
    }
}

// frameInの480サンプル（int16の値域）をRNNoiseへ通し、frameOutへ書く（-1〜1へ戻す）。
void NoiseReducer::processFrame() noexcept
{
    frameVad = rnnoise_process_frame (rnnoise.get(), frameOut.data(), frameIn.data());

    for (int i = 0; i < kFrame; ++i)
        frameOut[(size_t) i] *= kInt16ScaleInv;
}

// 出力FIFOからn個取り出してwetScratchへ書く（足りなければ無音で埋め、underflowCountを数える）。
void NoiseReducer::pullMixed (int n) noexcept
{
    const int take = std::min (outQueue.size(), n);

    if (take < n)
        ++underflowCount;

    std::memcpy (wetScratch.data(), outQueue.readPtr(), sizeof (float) * (size_t) take);
    std::fill (wetScratch.begin() + take, wetScratch.begin() + n, 0.0f);
    std::memcpy (vadScratch.data(), vadQueue.readPtr(), sizeof (float) * (size_t) take);
    std::fill (vadScratch.begin() + take, vadScratch.begin() + n, 0.0f);
    outQueue.drop (take);
    vadQueue.drop (take);
}

bool NoiseReducer::process (float* buf, int n) noexcept
{
    jassert (n <= maxBlockSamples);

    // ----- 状態遷移（ブロックの先頭で、稼働要求に応じて確定させる） -----
    switch (state)
    {
        case State::Resting:
            if (runTarget)
            {
                clearPipeline();
                primedSamples = 0;
                state = State::Priming;
            }
            break;

        case State::Priming:
            if (! runTarget)
                state = State::Resting;
            break;

        case State::FadingIn:
        case State::Active:
            if (! runTarget)
                state = State::FadingOut;
            break;

        case State::FadingOut:
            if (runTarget)
                state = State::FadingIn;
            break;
    }

    if (state == State::Resting)
    {
        updateLatencyForUi();
        return true;
    }

    if (! allFinite (buf, n))
        return false;

    // ----- パイプライン（Priming以降は常に進める） -----
    feed (buf, n);
    pullMixed (n);

    // 原音を遅延線へ通し、混合する: 出力 = (1 - d) * RNNoise + d * 原音（Dサンプル遅れ）。続けてゲート、インパクト抑制。
    // インパクト抑制の検出は、遅延線の出力より先読み分だけ新しい原音で行う（出力に届く2ms前に検出できる）。
    const int tapOffset = lookaheadSamples;

    for (int i = 0; i < n; ++i)
    {
        const float d = dSmoothed.getNextValue();
        const float delayedDry = dryLine[(size_t) dryPos];
        const int tapPos = dryPos + tapOffset >= delaySamples ? dryPos + tapOffset - delaySamples : dryPos + tapOffset;
        const float tap = dryLine[(size_t) tapPos];
        dryLine[(size_t) dryPos] = buf[i];

        if (++dryPos >= delaySamples)
            dryPos = 0;

        const bool dryValid = pipelineSamples >= delaySamples;
        const bool tapValid = pipelineSamples >= delaySamples - tapOffset;

        if (! dryValid)
            ++pipelineSamples;

        float mixed = wetScratch[(size_t) i] * (1.0f - d) + delayedDry * d;

        mixed *= gate.process (delayedDry, vadScratch[(size_t) i], closedGainSmoothed.getNextValue(), dryValid);
        mixed *= impact.process (tap, gate.open, tapValid);
        wetScratch[(size_t) i] = mixed;
    }

    // ----- 出力（状態の切り替わりはサンプル単位。ブロック長に依存しない） -----
    int pos = 0;

    while (pos < n)
    {
        switch (state)
        {
            case State::Resting: // FadingOutの完了後。残りは入力のまま
                pos = n;
                break;

            case State::Priming:
            {
                const int seg = std::min (n - pos, delaySamples - primedSamples);
                primedSamples += seg;
                pos += seg;

                if (primedSamples >= delaySamples)
                    state = State::FadingIn;

                break;
            }

            case State::Active:
                std::memcpy (buf + pos, wetScratch.data() + pos, sizeof (float) * (size_t) (n - pos));
                pos = n;
                break;

            case State::FadingIn:
            case State::FadingOut:
            {
                const bool ascending = (state == State::FadingIn);
                const double step = 1.0 / (double) fadeLenSamples;

                while (pos < n)
                {
                    buf[pos] = (float) ((double) buf[pos] * (1.0 - gain) + (double) wetScratch[(size_t) pos] * gain);
                    ++pos;
                    gain += ascending ? step : -step;
                    gain = juce::jlimit (0.0, 1.0, gain);

                    if (ascending && gain >= 1.0)
                    {
                        state = State::Active;
                        break;
                    }

                    if (! ascending && gain <= 0.0)
                    {
                        state = State::Resting;
                        break;
                    }
                }

                break;
            }
        }
    }

    updateLatencyForUi();
    return true;
}

} // namespace vc
