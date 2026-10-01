#pragma once

#include <juce_core/juce_core.h>

#include "core/Params.h"

#include <array>
#include <cstdint>
#include <vector>

// ===== SECTION: RecordingTool（T-014） =====
// 実録音の比較用オフラインツール。VoiceChangeTestsの`--process-wav`・`--compare`モードの本体（tests/TestMain.cppから呼ぶ）。
// docs/plan.md 8.3 T-014、README「実録音と比較する手順」参照。テスト（tests/MicTests.cpp M1）も同じ関数を呼ぶ。
// 指標は「区間」（無音 / 発話 / 打鍵 / 発話＋打鍵）ごとに出す。区間は台本の時刻ファイル、省略時は音声のエネルギーで自動判定する。

namespace vc::rectool
{

// ===== SECTION: WAV入出力 =====
struct Audio
{
    std::vector<float> samples; // 多チャンネルなら第1チャンネル
    double sampleRate = 0.0;
    int numChannels = 0;
};

bool readWav (const juce::File& file, Audio& out, juce::String& error);
// 16bit PCM（float32だと再生できないプレーヤーがあるため）。範囲外は±1へ丸める。既存ファイルは上書きする。
// float32（floatFormat）は--compareで測る用（16bitの量子化雑音は約-90dBFSで、それ以下の残留雑音は測れない）。
bool writeWav (const juce::File& file, const std::vector<float>& samples, double sampleRate, bool floatFormat, juce::String& error);
inline bool writeWav16 (const juce::File& file, const std::vector<float>& samples, double sampleRate, juce::String& error)
{
    return writeWav (file, samples, sampleRate, false, error);
}

// ===== SECTION: 区間 =====
enum class SegKind
{
    Silence,      // 無声区間（雑音のみ）
    Speech,       // 発話
    Impact,       // 打鍵・クリックなどの短い打撃音（発話なし）
    SpeechImpact  // 発話しながらの打鍵
};

// UI表示名と識別子の対照表: Silence="silence"(無音) / Speech="speech"(発話) / Impact="impact"(打鍵のみ) / SpeechImpact="speech+impact"(発話＋打鍵)
const char* segKindId (SegKind kind) noexcept;

struct Segment
{
    double startSec = 0.0;
    double endSec = 0.0;
    SegKind kind = SegKind::Silence;
};

// 区間ファイルの書式: 1行に「開始秒 終了秒 種類」。`#`以降はコメント。空行は無視。
bool parseSegments (const juce::String& text, std::vector<Segment>& out, juce::String& error);

// 音声のエネルギー（10msフレームのRMS）から区間を自動判定する。雑音床F = フレームdBの20パーセンタイル（下限-75dBFS）。
//   発話フレーム: F + 20dB以上。隙間200ms未満を結合し、120ms以上なら発話、未満なら打鍵。
//   無音: F + 10dB以下のフレームが、発話・打鍵から100ms以上離れて300ms以上続く区間。
// 粗い判定で、台本の時刻ファイルがあればそちらを使う。
std::vector<Segment> detectSegments (const std::vector<float>& x, double sampleRate);

// ===== SECTION: 指標 =====
constexpr int kNumBands = 22; // 1/3オクターブ、63Hz〜8kHz（中心 1000Hz×2^((k-12)/3)。表示は公称値）
constexpr std::array<int, kNumBands> kBandNominalHz { 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                                      800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000 };
constexpr double kDigitalSilenceDb = -100.0; // これ未満は「デジタル無音」として区別する

using BandDb = std::array<double, kNumBands>;

// 位置合わせ: testはrefよりlagSamplesだけ遅れている（test[n + lag] ~ ref[n]）。correlationは正規化した相互相関の最大値（0〜1）。
struct Alignment
{
    int lagSamples = 0;
    double correlation = 0.0;
    bool confident = false; // correlation >= 0.3。別テイク（無関係な録音）では偽
};

// K特性（BS.1770のプレフィルタ＋RLB高域通過）を通した全体の平均パワー [dB]。任意のレートで係数を計算する（48kHzで規格の係数と一致）。
double kWeightedLevelDb (const std::vector<float>& x, double sampleRate);

Alignment measureAlignment (const std::vector<float>& ref, const std::vector<float>& test, double sampleRate, double maxLagSec = 0.5);

struct SegmentStat
{
    double rmsDb = -200.0;
    double peakDb = -200.0;
};

struct FileStats
{
    juce::String name;
    Alignment alignment;              // rawに対して（raw自身は0）
    bool sharesSegments = true;       // rawの区間をそのまま使った（false: この音声自身のエネルギーで自動判定）
    std::vector<Segment> segments;    // この音声の時間軸での区間（lag補正済み）
    std::vector<SegmentStat> segmentStats;

    double silenceSec = 0.0;
    double silenceRmsDb = -200.0;
    double silenceMedianFrameDb = -200.0; // 10msフレームのdBの中央値
    double silenceDigitalFraction = 0.0;  // フレームのうち-100dBFS未満の割合
    BandDb silenceBands {};               // 1/3オクターブごとのパワー[dBFS]。計算できなければ全て-200

    double speechSec = 0.0;
    double speechRmsDb = -200.0;
    double speechKDb = -200.0; // 発話区間の平均パワーをITU-R BS.1770のK特性で重み付けした値 [dB]（ゲートなしの簡易版。生との差を主に見る）
    BandDb speechBands {};
    bool silenceBandsValid = false, speechBandsValid = false;

    // 発話と無声区間の雑音の比 [dB]。silenceSec > 0 && speechSec > 0 のときだけ意味を持つ。
    double snrDb() const noexcept { return speechRmsDb - silenceRmsDb; }
};

struct CompareResult
{
    double sampleRate = 0.0;
    std::array<FileStats, 3> files; // raw, sonar, ours
    bool segmentsFromFile = false;
    bool sonarSegmentsFromFile = false;
};

// segmentsがnullptrなら自動判定。oursはrawから作った音声として、rawの区間を共有する。sonarは相互相関で位置合わせでき
// （同時録音）れば共有し、できなければ（別テイク）自身の区間を自動判定する。
// sonarSegmentsは、sonarが別テイクのとき（相互相関で位置合わせできない）にsonar用の区間（sonar自身の時間軸）を与える。nullptrなら自動判定。
CompareResult compareRecordings (const std::vector<float>& raw, const std::vector<float>& sonar, const std::vector<float>& ours,
                                 double sampleRate, const std::vector<Segment>* segments, const std::vector<Segment>* sonarSegments = nullptr);

juce::String formatReport (const CompareResult& result);

// ===== SECTION: 処理 =====
// EQのプリセット（--eqの値）。A2・A3の値は本アプリのプリセット（Params.hのkEqPresets）。Sonarはユーザーの現在のSonarのEQを5バンドで近似した値（比較用。出力ゲインなし）。
enum class EqPreset { Off, On, A2, A3, Sonar };

struct ProcessSettings
{
    bool nrEnabled = true;
    float nrBackground = kNrBackgroundDefault;
    float nrImpact = kNrImpactDefault;
    bool eqEnabled = false;
    std::array<EqBandSettings, kEqBands> eqBands = kEqDefaults;
    float eqOutputGainDb = kEqOutputGainDefaultDb;
};

// プリセットを適用（OffはEQをOFFにするだけ。On以外のA2・A3・Sonarはバンドと出力ゲインを置き換えてEQをON）。
void applyEqPreset (ProcessSettings& settings, EqPreset preset);

struct GateBlockStat
{
    double startSec = 0.0;     // 入力の時間軸での開始（遅延補正済み。負なら処理開始直後の遅延分）
    int blockSamples = 0;
    int gateClosedSamples = 0; // ブロック内でゲートが閉じていたサンプル
    int impactSamples = 0;     // ブロック内でインパクト抑制が減衰していたサンプル
};

struct ProcessResult
{
    std::vector<float> output;     // 入力と同じ長さ。遅延（latencySamples）を取り除いて入力に位置を合わせてある
    int latencySamples = 0;        // ノイズ除去の遅延（OFFなら0）
    std::uint32_t errorFlags = 0;
    int underflowCount = 0;
    std::vector<GateBlockStat> blocks;
};

// Engine（プリセット ノーマル、ゲイン0）に通す。ブロック長は10ms。出力は遅延を補正して入力と同じ長さにする。
ProcessResult processAudio (const std::vector<float>& input, double sampleRate, const ProcessSettings& settings);

// ===== SECTION: コマンド =====
// argsは`--process-wav`／`--compare`の後ろの引数。戻り値は終了コード（0: 成功、2: 使い方・入力の誤り）。
int runProcessWav (const juce::StringArray& args);
int runCompare (const juce::StringArray& args);

} // namespace vc::rectool
