#pragma once

#include <juce_core/juce_core.h>

#include "core/Params.h"

#include <array>
#include <cstdint>
#include <optional>
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

// T-018（プリセット強化の試聴サンプル作成）で足した項目は、すべて既定で従来どおり（ノーマル・ゲイン0・ピッチ0・リバーブ0・上書きなし）。
// 製品のプリセット表・ピッチ範囲は変えない。範囲外の移調と実験用のキャリアは、このツールの中だけで許す（試聴の候補探索用）。
// 通称（コマンドの値）と内部識別子: follow = TalkboxCarrier::Follow / fixed = TalkboxCarrier::Fixed。
// quantized = TalkboxCarrier::Quantized（T-018第2段。検出f0を音階に量子化して補正時間ゼロの階段状に動かす）。
// sequence = TalkboxCarrier::Sequence（T-018第2段の続き。キャリアの音程を声の高さではなく、あらかじめ決めたフレーズ（音列）に従って動かす。声は包絡と音量だけを与える）。
enum class TalkboxCarrier { Follow, Fixed, Quantized, Sequence }; // Follow = 検出したf0に追従（D-028より前の製品の動作。実験用）/ Fixed = 固定の高さ（平坦なロボット声）
enum class TalkboxAdvance { Free, Syllable }; // フレーズの進め方（free = テンポに従って自走、syllable = 声の立ち上がり（音節）ごとに次の音へ）
enum class TalkboxScale { Chromatic, Major, Minor }; // 量子化の音階（chromatic = 半音、major/minor = 主音（talkboxKey）からの長音階・自然短音階）

struct ProcessSettings
{
    bool nrEnabled = true;
    float nrBackground = kNrBackgroundDefault;
    float nrImpact = kNrImpactDefault;
    bool eqEnabled = false;
    std::array<EqBandSettings, kEqBands> eqBands = kEqDefaults;
    float eqOutputGainDb = kEqOutputGainDefaultDb;

    // 層1・層2（--preset・--gain・--pitch・--reverb）。pitchは層1ピッチ（半音、整数）。製品の±24を超えてよい（kMaxToolPitch）。
    Preset preset = Preset::Normal;
    float gainDb = 0.0f;
    int pitch = 0;
    float reverb = 0.0f;

    // プリセットの移調量（半音）・フォルマント係数の上書き（--semitones・--formant）。片方だけでもよい（もう片方はプリセット表の値）。
    std::optional<float> semitonesOverride;
    std::optional<float> formantOverride;

    // 製品のトークボックス（固定フレーズS4。D-028）の音域（--talkbox-range）。実験の設定（下の項目）がないときだけ有効（実験用のキャリアには音域を掛けない）。
    TalkboxRange talkboxRange = TalkboxRange::Low;

    // トークボックスの実験（--talkbox-carrier・--talkbox-hz・--talkbox-chord）。キャリアはf0（またはfixedHz）× 2^(chord[i]/12)の和（各1/sqrt(個数)）。
    // 実験の設定が1つもなければ、製品のトークボックス（固定フレーズ）になる。--talkbox-carrierを明示するとfollow（検出f0に追従。D-028より前の製品の動作）も実験用のボコーダーを通る。
    TalkboxCarrier talkboxCarrier = TalkboxCarrier::Follow;
    float talkboxFixedHz = 110.0f;
    std::vector<float> talkboxChord { 0.0f };
    float talkboxVoicingFloor = 0.0f; // キャリアの有声度（鋸波の割合）の下限。製品は検出器の値そのまま（0 = 下限なし）。低い声で検出器が無声寄りに判定するときの確認用

    // トークボックスの実験その2（T-018第2段。--talkbox-scale・--talkbox-key・--talkbox-detune・--talkbox-octave-up・--talkbox-bands・--talkbox-high-hz・
    // --talkbox-air-db・--talkbox-consonant）。製品のTalkboxで表せない設定（デチューン・オクターブ上・バンド数・高域・強調・子音）を指定するか、talkboxVocoder = true にすると、製品のTalkboxの代わりにこのツール内のExperimentVocoder
    // （製品と同じ帯域構成で、キャリア合成・バンド数・高域強調だけを変えられる）を使う。キャリアは和音の音ごと×デチューンごとの鋸波の和（正規化は1/sqrt(本数)）。
    bool talkboxVocoder = false;
    TalkboxScale talkboxScale = TalkboxScale::Chromatic;
    int talkboxKey = 0;                              // 主音のピッチクラス（0 = C … 11 = B）。major/minorのとき
    float talkboxSpread = 1.0f;                      // 抑揚の拡大率。検出音高の、ゆっくり追従する中心（時定数3秒）からの差をこの倍率にしてから量子化する（1 = そのまま。話し声の音高の動きは小さく、そのままだと数音にしか動かないため）
    std::vector<float> talkboxDetuneCents { 0.0f };  // 鋸波を重ねる本数と各本のデチューン（セント）
    std::optional<float> talkboxOctaveUpDb;          // オクターブ上の重ねのレベル（dB。未指定 = 重ねない）
    int talkboxBands = 20;                           // バンド数。変調側・キャリア側のQはバンド間隔に合わせて(bands-1)/19倍にする
    float talkboxHighHz = 7000.0f;                   // 最上バンドの中心周波数
    float talkboxAirDb = 0.0f;                       // 3kHz以上の高域の強調（3kHzで0dB → 8kHz以上でこの値。対数周波数で直線）
    float talkboxConsonant = 0.0f;                   // 2.5kHz以上のバンドのキャリアに混ぜる雑音の量。0 = 低域と同じ（下限適用後の有声度）、1 = 検出器の有声度そのまま（無声子音の摩擦音が雑音で鳴る）

    // トークボックスの実験その3（固定フレーズ・キャリア。--talkbox-carrier sequence・--talkbox-seq・--talkbox-bpm・--talkbox-step・--talkbox-advance・--talkbox-glide）。
    // 音列は、ステップごとの音（MIDI番号。C4 = 60、A4 = 440Hz = 69）の並び。1ステップに複数の音（和音）を入れてよい。--talkbox-chordは各音にさらに重ねる。
    // キャリアの音程は検出したf0に依存しない（有声度だけ検出器を使う）。freeはテンポ（bpm × stepBeats）だけで決まり、syllableは声の立ち上がり（低域の包絡の上昇）で進む。
    std::vector<std::vector<float>> talkboxSeq;
    float talkboxBpm = 120.0f;
    float talkboxStepBeats = 0.5f;                   // 1ステップの長さ（拍）。0.25 = 16分音符、0.5 = 8分音符、1 = 4分音符
    TalkboxAdvance talkboxAdvance = TalkboxAdvance::Free;
    float talkboxGlideMs = 0.0f;                     // 音の切り替えのグライド（MIDI音高の直線）。0 = 階段
    bool traceSequence = false;                      // trueなら、ProcessResultへキャリア音程のサンプルごとの軌跡を残す（テスト用）
};

// このツールが受け付ける移調量とフォルマント係数の範囲（製品の範囲ではない）。
constexpr int kMaxToolPitch = 36;
constexpr float kMaxToolSemitones = 48.0f;
constexpr float kMinToolFormant = 0.25f, kMaxToolFormant = 4.0f;

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
    int shifterLatencySamples = 0; // 終了時にピッチシフターが稼働していたときの遅延（出力から取り除いてある。休止中は0）
    std::uint32_t errorFlags = 0;
    int underflowCount = 0;
    std::vector<GateBlockStat> blocks;

    // 固定フレーズ・キャリア（sequence）のとき: 声の立ち上がり（音節）で次の音へ進めた位置（入力のサンプル番号。freeでも空）。
    std::vector<int> sequenceOnsets;
    // traceSequenceのとき、スロット（和音の音）ごとのキャリアのMIDI音高の軌跡（入力のサンプルごと。処理の時間軸。ノイズ除去ONの遅延は補正していない）。
    std::vector<std::vector<float>> sequenceSlotMidi;
};

// 音列の文字列（例: "G2,Bb2,D3,Bb2"、"43,46,50"、和音は'+'でつなぐ "G2+Bb2+D3,Eb2+G2+Bb2"）をステップごとのMIDI番号へ。
// 音名はC〜B＋#またはb＋オクターブ（C4 = 60）、または整数のMIDI番号。範囲はMIDI 24〜96、1〜64ステップ、1ステップ1〜6音。不正ならfalse。
bool parseNoteSequence (const juce::String& text, std::vector<std::vector<float>>& steps);

// Engine（設定のプリセット・層1。既定はノーマル、ゲイン0）に通す。ブロック長は10ms。出力は遅延を補正して入力と同じ長さにする。
// 移調量・フォルマントの上書きとトークボックスの実験があるときは、Engineを「マイク処理」と「層1」の2つに分け、間にPitchShifter・
// PitchDetector・Talkboxを直接置いた試聴用の経路を通す（Engineのプリセット表に依らないため。製品のEngineは変えない）。
ProcessResult processAudio (const std::vector<float>& input, double sampleRate, const ProcessSettings& settings);

// ===== SECTION: コマンド =====
// argsは`--process-wav`／`--compare`の後ろの引数。戻り値は終了コード（0: 成功、2: 使い方・入力の誤り）。
int runProcessWav (const juce::StringArray& args);
int runCompare (const juce::StringArray& args);

} // namespace vc::rectool
