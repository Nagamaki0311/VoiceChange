#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "Params.h"
#include "PitchShifter.h"

#include <atomic>
#include <cstdint>
#include <vector>

// ===== SECTION: Engine =====
// 音声処理チェーン全体（ピッチシフター + 層1: リバーブ・ゲイン・リミッター）。
// GUI・デバイスに依存しない（vc_core）。docs/spec.md「音声処理チェーン」「層1: 音響卓」、
// docs/decisions.md D-007・D-008・D-010、docs/plan.md 2.5節「Engine」参照。
// 層2の効果（エコー/ロボット/トークボックス）とケロケロの補正量計算はT-005で追加する。

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

    // bit0: 非有限値、bit1: 例外（AudioIOが立てる）。
    std::uint32_t getErrorFlags() const noexcept { return errorFlags.load (std::memory_order_relaxed); }
    void clearErrorFlags() noexcept { errorFlags.store (0, std::memory_order_relaxed); }

    // AudioIOの出力コールバックがtry/catchで例外を捕まえたときに立てる（bit1）。
    void raiseExceptionErrorFlag() noexcept { errorFlags.fetch_or (0x2u, std::memory_order_relaxed); }

private:
    void processChunk (float* buf, int n) noexcept;
    void processChain (float* buf, int n, int presetIdx, int pitchSemis, float gainDb, float reverbAmt) noexcept;
    void processReverb (float* buf, int n, float reverbAmt) noexcept;
    void applyLimiter (float* buf, int n) noexcept;
    void handleNonFinite (float* buf, int n) noexcept;
    bool checkAndHandleFinalNonFinite (float* buf, int n) noexcept;
    void resetChain() noexcept;
    void updateInputPeak (const float* buf, int n) noexcept;

    AtomicParams atomicParams;

    PitchShifter shifter;
    juce::Reverb reverb;
    juce::dsp::Compressor<float> compressor;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gainSmoothed;
    juce::SmoothedValue<float> reverbGainSmoothed;
    bool reverbActive = false;

    double sampleRate = 48000.0;
    int maxBlockSamples = 0;

    std::vector<float> dryScratch;

    // 1=チェーン全体を通す、0=完全バイパス。20msでクロスフェードする（D-010）。
    double chainGain = 1.0;

    std::atomic<float> inputPeak { 0.0f };
    std::atomic<std::uint32_t> errorFlags { 0 };
};

} // namespace vc
