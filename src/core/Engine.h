#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "Effects.h"
#include "MicProcessing.h"
#include "Params.h"
#include "PitchDetector.h"
#include "PitchShifter.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// ===== SECTION: Engine =====
// 音声処理チェーン全体（マイク処理: ノイズ除去 + EQ + ピッチ検出 + ピッチシフター + 層2の効果 + 層1: リバーブ・ゲイン・リミッター）。
// GUI・デバイスに依存しない（vc_core）。docs/spec.md「音声処理チェーン」「層1: 音響卓」、
// docs/decisions.md D-007・D-008・D-010、docs/plan.md 2.5節「Engine」参照。
// 層2の効果（エコー/ロボット/トークボックス）の切替は20msのクロスフェード、ケロケロの補正量は
// ピッチ検出の結果から毎ブロック計算する（docs/spec.md「層2: 特殊効果プリセット」）。

namespace vc
{

struct EngineConfig
{
    double sampleRate = 48000.0;
    int maxBlock = 0;
};

class Engine
{
public:
    // メッセージスレッドのみ。デバイス停止中に呼ぶ。
    void prepare (const EngineConfig& config);

    AtomicParams& params() noexcept { return atomicParams; }

    // 出力スレッドのみ。n > maxBlockならmaxBlockごとに分割する。
    void process (float* monoInOut, int n) noexcept;

    // exchange(0)。UIスレッドから読む。
    float takeInputPeak() noexcept;

    int getShifterLatencySamples() const noexcept { return shifter.getLatencySamples(); }

    // ノイズ除去の遅延（サンプル）。稼働中（FadingIn/Active/FadingOut）のみ非0。UIスレッドから読める。
    int getNoiseReducerLatencySamples() const noexcept { return noiseReducer.getLatencySamples(); }

    // テスト専用。音声スレッドの本処理からは使わない。
    const NoiseReducer& debugNoiseReducer() const noexcept { return noiseReducer; }

    // ケロケロの直前の補正量（半音）。音声スレッド（またはテスト）専用で、UIスレッドからは読まない。
    float getKerokeroCorrectionSemitones() const noexcept { return kerokeroCorrection; }

    // bit0: 非有限値、bit1: 例外（AudioIOが立てる）。
    std::uint32_t getErrorFlags() const noexcept { return errorFlags.load (std::memory_order_relaxed); }
    void clearErrorFlags() noexcept { errorFlags.store (0, std::memory_order_relaxed); }

    // AudioIOの出力コールバックがtry/catchで例外を捕まえたときに立てる（bit1）。
    void raiseExceptionErrorFlag() noexcept { errorFlags.fetch_or (0x2u, std::memory_order_relaxed); }

private:
    void processChunk (float* buf, int n) noexcept;
    void processChain (float* buf, int n, int presetIdx, int pitchSemis, float gainDb, float reverbAmt, TalkboxRange talkboxRange) noexcept;
    void processLayer2 (float* buf, int n, Effect desired, float pitchRatio, TalkboxRange range, float voicing) noexcept;
    void runEffect (Effect effect, const float* in, float* out, int n, float pitchRatio, TalkboxRange range, float voicing) noexcept;
    void processReverb (float* buf, int n, float reverbAmt) noexcept;
    void applyLimiter (float* buf, int n) noexcept;
    void handleNonFinite (float* buf, int n) noexcept;
    bool checkAndHandleFinalNonFinite (float* buf, int n) noexcept;
    void resetChain() noexcept; // 層1・層2（ピッチ検出・シフター・効果・リバーブ・リミッター）。マイク処理は含まない
    void resetMic() noexcept;   // マイク処理（ノイズ除去・EQ）
    void updateInputPeak (const float* buf, int n) noexcept;

    AtomicParams atomicParams;

    NoiseReducer noiseReducer;
    Equalizer equalizer;
    PitchDetector detector;
    PitchShifter shifter;
    Echo echo;
    RingModulator ringMod;
    Talkbox talkbox;
    juce::Reverb reverb;
    juce::dsp::Compressor<float> compressor;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gainSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> presetGainSmoothed; // プリセットの声量補正（D-027）。層1のゲインとは別
    juce::SmoothedValue<float> reverbGainSmoothed;
    bool reverbActive = false;
    bool reverbParamsStale = true; // 補間中と、prepare/reset後の最初の1回だけreverb.setParametersを呼ぶ

    // 層2の効果の切替状態。fading == trueの間は旧効果(fadeFrom)と新効果(activeEffect)を並行処理する。
    // Effect::Noneは「効果なし」という有効な効果なので、フェード中かどうかはfadingで別に持つ。
    Effect activeEffect = Effect::None;
    Effect fadeFrom = Effect::None;
    bool fading = false;
    int fadePos = 0;
    int fadeLen = 1;
    bool detectorRunning = false;
    float kerokeroCorrection = 0.0f; // 直前の補正量（半音）。無声・無音区間はこれを保つ

    double sampleRate = 48000.0;
    int maxBlockSamples = 0;

    std::vector<float> dryScratch;
    std::vector<float> fxInScratch;
    std::vector<float> fxOldScratch;

    // 1=チェーン全体を通す、0=完全バイパス。20msでクロスフェードする（D-010）。
    double chainGain = 1.0;

    std::atomic<float> inputPeak { 0.0f };
    std::atomic<std::uint32_t> errorFlags { 0 };
};

} // namespace vc
