#include "MicProcessing.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <complex>
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

// ===== SECTION: Equalizer =====

namespace
{
constexpr double kEqFadeSeconds = 0.020;   // EQ全体・バンドの有効/無効・タイプ変更のクロスフェード
constexpr double kEqSmoothSeconds = 0.050; // 周波数・ゲイン・Qの補間
constexpr float kEqMaxHzRatio = 0.45f;     // 周波数の実効上限 = 0.45 × 出力レート
} // namespace

void Equalizer::prepare (double newSampleRate)
{
    jassert (newSampleRate > 0.0);

    sampleRate = newSampleRate;
    maxHz = (float) (kEqMaxHzRatio * sampleRate);
    fadeStep = 1.0 / (double) std::max (1, (int) std::lround (kEqFadeSeconds * sampleRate));

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];

        // Coefficientsはここで作る（make*が確保する）。以後は音声スレッドで、同じオブジェクトへ配列を代入するだけ。
        b.filter.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f, 1.0f);
        b.filter.reset();

        b.hz.reset (sampleRate, kEqSmoothSeconds);
        b.q.reset (sampleRate, kEqSmoothSeconds);
        b.gainDb.reset (sampleRate, kEqSmoothSeconds);

        b.target = sanitizeEqBand (kEqDefaults[i], kEqDefaults[i]);
        b.target.hz = std::min (b.target.hz, maxHz);
        b.type = b.target.type;
        b.hz.setCurrentAndTargetValue (b.target.hz);
        b.q.setCurrentAndTargetValue (b.target.q);
        b.gainDb.setCurrentAndTargetValue (b.target.gainDb);
    }

    outputGainDb.reset (sampleRate, kEqSmoothSeconds);
    outputGainDb.setCurrentAndTargetValue (kEqOutputGainDefaultDb);

    runTarget = false;
    reset();
}

void Equalizer::setTarget (bool run, const std::array<EqBandSettings, kEqBands>& settings, float newOutputGainDb) noexcept
{
    runTarget = run;
    const bool resting = state == State::Resting;

    // 出力ゲイン: 非有限値は初期値、範囲外は端へ（juce::jlimitはNaNをそのまま返すため、先に非有限値を除く）。
    if (! std::isfinite (newOutputGainDb))
        newOutputGainDb = kEqOutputGainDefaultDb;

    newOutputGainDb = juce::jlimit (-kEqMaxOutputGainDb, kEqMaxOutputGainDb, newOutputGainDb);

    if (resting) // 休止中は補間しない（起動時に古い値から動かさない）
        outputGainDb.setCurrentAndTargetValue (newOutputGainDb);
    else
        outputGainDb.setTargetValue (newOutputGainDb);

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        b.target = sanitizeEqBand (settings[i], kEqDefaults[i]);
        b.target.hz = std::min (b.target.hz, maxHz);

        if (resting) // 休止中の値の変化は補間しない（起動時に古い値から動かさない）
        {
            b.hz.setCurrentAndTargetValue (b.target.hz);
            b.q.setCurrentAndTargetValue (b.target.q);
            b.gainDb.setCurrentAndTargetValue (b.target.gainDb);
        }
        else
        {
            b.hz.setTargetValue (b.target.hz);
            b.q.setTargetValue (b.target.q);
            b.gainDb.setTargetValue (b.target.gainDb);
        }
    }
}

void Equalizer::reset() noexcept
{
    state = State::Resting;
    gain = 0.0;
    phase = 0;
    coefficientsStale = true;

    for (auto& b : bands)
    {
        b.filter.reset(); // 次数は変わらないため確保しない
        b.mix = 0.0;
        b.idle = true;
    }
}

// Restingから稼働へ。バンドは目標の状態（有効なら処理後、無効なら素通し）で始め、全体のgainで0から20msかけて入れる。
void Equalizer::start() noexcept
{
    for (auto& b : bands)
    {
        b.type = b.target.type;
        b.filter.reset();
        b.mix = b.target.on ? 1.0 : 0.0;
        b.idle = ! b.target.on;
    }

    phase = 0;
    coefficientsStale = true;
    gain = 0.0;
    state = State::FadingIn;
}

std::array<float, 6> eqBandCoefficients (EqType type, double sampleRate, float hz, float q, float gainDb) noexcept
{
    hz = std::min (hz, (float) (kEqMaxHzRatio * sampleRate));
    const float gainFactor = juce::Decibels::decibelsToGain (gainDb);
    using Array = juce::dsp::IIR::ArrayCoefficients<float>;

    switch (type)
    {
        case EqType::Peak:      return Array::makePeakFilter (sampleRate, hz, q, gainFactor);
        case EqType::LowShelf:  return Array::makeLowShelf (sampleRate, hz, q, gainFactor);
        case EqType::HighShelf: return Array::makeHighShelf (sampleRate, hz, q, gainFactor);
        case EqType::LowCut:    return Array::makeHighPass (sampleRate, hz, q);  // ローカット = ハイパス。ゲインは使わない
        case EqType::HighCut:   return Array::makeLowPass (sampleRate, hz, q);   // ハイカット = ローパス。ゲインは使わない
    }

    return Array::makePeakFilter (sampleRate, hz, q, gainFactor);
}

double eqMagnitudeDb (const std::array<float, 6>& c, double sampleRate, double hz) noexcept
{
    const double w = 2.0 * juce::MathConstants<double>::pi * hz / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    const auto num = (double) c[0] + (double) c[1] * z1 + (double) c[2] * z2;
    const auto den = (double) c[3] + (double) c[4] * z1 + (double) c[5] * z2;

    return 20.0 * std::log10 (std::max (1.0e-12, std::abs (num / den)));
}

double eqCurveDb (const std::array<EqBandSettings, kEqBands>& settings, double sampleRate, double hz) noexcept
{
    double db = 0.0;

    for (const auto& b : settings)
        if (b.on)
            db += eqMagnitudeDb (eqBandCoefficients (b.type, sampleRate, b.hz, b.q, b.gainDb), sampleRate, hz);

    return db;
}

void Equalizer::updateCoefficients (Band& b) noexcept
{
    // 係数は、作成済みのCoefficientsへ配列を代入するだけ（確保しない）。計算はUIのグラフと共通のeqBandCoefficients。
    *b.filter.coefficients = eqBandCoefficients (b.type, sampleRate, b.hz.getCurrentValue(), b.q.getCurrentValue(),
                                                 b.gainDb.getCurrentValue());
}

// 32サンプルのグループの先頭。サンプル数で数えるため、ブロックの区切りに依存しない。
void Equalizer::beginGroup() noexcept
{
    bool smoothing = false;

    for (auto& b : bands)
    {
        // 素通しになったバンドで、タイプを切り替える（フィルタ状態は、処理を再開するときにリセットする）。
        if (b.idle && b.type != b.target.type)
        {
            b.type = b.target.type;
            coefficientsStale = true;
        }

        smoothing = smoothing || b.hz.isSmoothing() || b.q.isSmoothing() || b.gainDb.isSmoothing();
    }

    if (! smoothing && ! coefficientsStale)
        return;

    // このグループの係数は、グループの先頭の補間値から計算し、補間値を32サンプル進める。補間が終わった直後のグループでは、
    // 最後の補間値で計算したままなので、もう1グループ（目標値で）計算する。
    for (auto& b : bands)
    {
        updateCoefficients (b);
        b.hz.skip (kGroup);
        b.q.skip (kGroup);
        b.gainDb.skip (kGroup);
    }

    coefficientsStale = smoothing;
}

// 1つのバンドをsegの[0, len)へ掛ける（len <= kGroup）。mixは1サンプルごとに動く。
void Equalizer::processBand (Band& b, float* seg, int len) noexcept
{
    const bool up = b.target.on && b.type == b.target.type; // 処理後の側へ向かうか

    if (b.idle)
    {
        if (! up)
            return; // 素通しのまま（フィルタは動かさない）

        b.filter.reset(); // 処理を再開するときは、古い状態を持ち込まない
        b.idle = false;
    }

    // processSample（1サンプルずつ）を使う。Filter::process()は呼び出しごとに状態のsnapToZeroをするため、
    // ブロックの区切りによって結果が変わりうる。snapToZeroはグループの終わり（process()の側）で行う。
    float ch[kGroup];

    for (int i = 0; i < len; ++i)
        ch[i] = b.filter.processSample (seg[i]);

    if (phase + len == kGroup)
        b.filter.snapToZero();

    if (up && b.mix >= 1.0)
    {
        std::memcpy (seg, ch, sizeof (float) * (size_t) len);
        return;
    }

    for (int i = 0; i < len; ++i)
    {
        seg[i] = (float) ((double) seg[i] * (1.0 - b.mix) + (double) ch[i] * b.mix);
        b.mix = juce::jlimit (0.0, 1.0, b.mix + (up ? fadeStep : -fadeStep));
    }

    if (! up && b.mix <= 0.0)
        b.idle = true;
}

// 5バンドの後に出力ゲインを掛ける。補間中は1サンプルごとにdBから線形へ直し、補間が終わったら一定の係数（0dBなら何もしない）。
void Equalizer::applyOutputGain (float* seg, int len) noexcept
{
    if (outputGainDb.isSmoothing())
    {
        for (int i = 0; i < len; ++i)
            seg[i] *= juce::Decibels::decibelsToGain (outputGainDb.getNextValue());

        return;
    }

    const float g = outputGainDb.getTargetValue();

    if (! juce::exactlyEqual (g, 0.0f)) // 補間の終わったdB値。0dB（既定）では素通しのまま
        juce::FloatVectorOperations::multiply (seg, juce::Decibels::decibelsToGain (g), len);
}

// EQ全体のクロスフェード。dryは処理前のセグメント、segはバンドを通した後。状態の切り替わりはサンプル単位。
void Equalizer::mixGlobal (float* seg, const float* dry, int len) noexcept
{
    const bool ascending = state == State::FadingIn;

    for (int i = 0; i < len; ++i)
    {
        seg[i] = (float) ((double) dry[i] * (1.0 - gain) + (double) seg[i] * gain);
        gain = juce::jlimit (0.0, 1.0, gain + (ascending ? fadeStep : -fadeStep));

        if (ascending && gain >= 1.0)
        {
            state = State::Active; // 残りは処理後のまま
            return;
        }

        if (! ascending && gain <= 0.0)
        {
            state = State::Resting; // 残りは入力のまま
            std::memcpy (seg + i + 1, dry + i + 1, sizeof (float) * (size_t) (len - i - 1));
            return;
        }
    }
}

void Equalizer::process (float* buf, int n) noexcept
{
    switch (state)
    {
        case State::Resting:
            if (runTarget)
                start();
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

    int pos = 0;

    while (pos < n && state != State::Resting)
    {
        if (phase == 0)
            beginGroup();

        const int len = std::min (n - pos, kGroup - phase);
        float* seg = buf + pos;

        float dry[kGroup];
        const bool crossfading = state != State::Active;

        if (crossfading)
            std::memcpy (dry, seg, sizeof (float) * (size_t) len);

        for (auto& b : bands)
            processBand (b, seg, len);

        applyOutputGain (seg, len);

        if (crossfading)
            mixGlobal (seg, dry, len);

        phase = (phase + len) % kGroup;
        pos += len;
    }
}

} // namespace vc
