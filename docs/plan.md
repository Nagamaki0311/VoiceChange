# VoiceChange 実装計画（T-002〜T-015）

Plannerが作成し、Managerが7章の判断を確定した計画。要件はdocs/spec.md、UIはdocs/design.md、判断の経緯はdocs/decisions.mdを正とし、本書と食い違う場合はそちらを優先する。各タスクの完了時に、実際の構成と本書がずれた箇所は本書を更新する。T-009〜T-015（マイク処理）は8章に追加した（2026-09-29、要件はspec.md、判断はD-019〜D-025）。

## 1. 依存ソースから確認した事実（計画に影響するもの）

1. JUCEモジュールはINTERFACEライブラリで、ソースをINTERFACE_SOURCESとして持つ（`extras/Build/CMake/JUCEModuleSupport.cmake`）。リンクしたターゲットごとにJUCEのソースがコンパイルされる。
2. `juce::dsp::Limiter`は透過的な-1dBFSリミッターではない（`juce_Limiter.cpp`の`update()`）。第1段で閾値-10dB・4:1の圧縮を常にかけ、メイクアップ後に±1.0でクリップする。ゲイン0dBでも小さい音が約+2.9dB持ち上がる。
3. `juce::Reverb`はパラメータに内部倍率を掛ける（dryLevel×2、wetLevel×3、mono時のwetは`0.5*wet*(1+width)`）。
4. JUCE WASAPI（`juce_WASAPI_windows.cpp`）
   - 入力専用デバイスのスレッドは、イベント待ちが1秒タイムアウトすると終了し、エラー通知を出さない。ウォッチドッグが必須。
   - OS側でサンプルレートが変わると、JUCEが内部でclose→open→`start(callback)`を行い、`audioDeviceAboutToStart`がメッセージスレッドから新しいレートで再度呼ばれる。
   - 一覧では既定デバイスがindex 0。同名デバイスには番号が付く。
   - `scanForDevices()`の前に`createDevice`を呼ぶとjassert。
5. Signalsmith Stretch 1.4.0
   - 入力がデジタル無音（エネルギー<1e-15）で2ブロック続くと、入力を出力へ直接コピーするモードに入る（遅延0）。
   - `reset()`はアロケーションしない見込み。
   - `peaks.reserve(bands/2)`に対し最大ピーク数はceil(bands/2)になり得る。signalsmith-linearの`Temporary`にもfallbackでアロケーションする経路がある。稀に`process()`内で確保が起きる可能性がある。
6. signalsmith-linearの`/bigobj`は自ディレクトリにしか効かない。Stretchをincludeする自前ソースには別途`/bigobj`が要る。
7. JUCE 9.0.2のライセンスはAGPLv3と商用のデュアル。
8. `PropertiesFile::Options::millisecondsBeforeSaving`で「変更の1秒後に保存」ができる。`DialogWindow::LaunchOptions::create()`は非モーダル。`SystemTrayIconComponent::showInfoBubble`はLinux実装もありコンパイルが通る。

## 2. 実装方針

### 2.1 ファイル構成

```
CMakeLists.txt
.gitignore
.github/workflows/build.yml
README.md
src/core/                   GUI・デバイス非依存（juce_core / juce_audio_basics / juce_dsp / signalsmith-stretch）
  Params.h                  プリセット表・範囲・atomicパラメータ・設定値の丸め
  ResamplingFifo.h/.cpp     SPSCリングバッファ + ドリフト補正リサンプラ
  PitchShifter.h/.cpp       Stretchラッパ + 休止⇔稼働の状態遷移 + ブロック長決定関数
  PitchDetector.h/.cpp      YIN
  Effects.h/.cpp            Echo / RingModulator / Talkbox
  Engine.h/.cpp             チェーン全体・層1・プリセット切替・バイパス・NaN・統計
  ConnectionMonitor.h       デバイス異常の判定と再接続タイミング（T-007）
  StatsLog.h/.cpp           統計ログ行の整形と1MBローテーション（T-007）
src/app/
  Main.cpp                  JUCEApplication、MainWindow、トレイ、設定、60秒ログタイマー、--screenshot
  AudioIO.h/.cpp            2デバイスの列挙・開閉・フォールバック・コールバック・ウォッチドッグ・再接続
  MainComponent.h/.cpp      UI
tests/
  TestMain.cpp              ランナー（カテゴリ指定・終了コード）+ アロケーション検出フック
  TestSignals.h             信号生成と解析（FFTピーク・包絡スケール・隣接差・2階差分・クリック判定器）
  RingBufferTests.cpp       RingBuffer / RingBufferLong
  ShifterTests.cpp          Smoke / Shifter
  EffectsTests.cpp          Effects
  EngineTests.cpp           Engine
  AppLogicTests.cpp         AppLogic
  LongRunTests.cpp          LongRun
```

層1（ゲイン・リバーブ・リミッター）はEngine内に置く。トレイはMain.cppに同居させる。

### 2.2 CMakeターゲット

- **`vc_core`はINTERFACEライブラリ**（D-005）。`target_sources(vc_core INTERFACE src/core/*.cpp)`と`target_link_libraries(vc_core INTERFACE juce::juce_core juce::juce_audio_basics juce::juce_dsp signalsmith-stretch)`。使う側の実行ファイルごとにDSPソースとJUCEソースを1回ずつコンパイルする。テスト実行ファイルはvc_coreしかリンクしないので、DSPソースがGUI・デバイスのヘッダをincludeするとテストのビルドが失敗し、層の分離が担保される。
- **`VoiceChange`**: `juce_add_gui_app`。`vc_core`、`juce::juce_audio_devices`、`juce::juce_gui_extra`、`juce_recommended_config_flags`、`juce_recommended_warning_flags`。WIN32では`psapi`をリンク。
- **`VoiceChangeTests`**: `juce_add_console_app`。`vc_core`とconfig/warningフラグ。
- **テストは`juce::UnitTest`**。`UnitTestRunner::runTestsInCategory`で1つの実行ファイルをカテゴリ引数でctestに複数登録する。`JUCE_UNIT_TESTS`は定義しない。結果0件は失敗扱い。
- ctest: `add_test(NAME ring_buffer COMMAND VoiceChangeTests --category RingBuffer)`。RingBufferLong、LongRunは`LABELS long`・`TIMEOUT 1800`、それ以外は`LABELS quick`。

### 2.3 CMakeの要点

- `cmake_minimum_required(VERSION 3.25)`（FetchContentの`SYSTEM`用）、`project(VoiceChange VERSION 0.1.0 LANGUAGES C CXX)`、C++20。
- どのターゲットよりも先に`set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`。
- FetchContent: JUCE `9.0.2`（GIT_SHALLOW）、signalsmith-stretch `1.4.0`（GIT_SHALLOW、`SYSTEM`）。linear 0.6.4はstretch側に任せる。
- ローカルでは`-DFETCHCONTENT_SOURCE_DIR_JUCE=... -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH-STRETCH=... -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH-LINEAR=...`でscratchpadの取得済みソースを使える。
- vc_coreのINTERFACE定義: `JUCE_USE_CURL=0`、`JUCE_WEB_BROWSER=0`、`JUCE_USE_FLAC=0`、`JUCE_USE_OGGVORBIS=0`、`JUCE_USE_MP3AUDIOFORMAT=0`、`JUCE_USE_WINDOWS_MEDIA_FORMAT=0`、`JUCE_STRICT_REFCOUNTEDPOINTER=1`。アプリ側に`JUCE_DIRECTSOUND=0`、`JUCE_APPLICATION_NAME_STRING`、`JUCE_APPLICATION_VERSION_STRING`。
- MSVC: `target_compile_options(vc_core INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/bigobj /utf-8>)`。
- 日本語文字列は`juce::String::fromUTF8("...")`で作る（`String(const char*)`はASCII前提でDebugではjassert）。
- Linuxでアプリをビルドする条件: 上記の無効化、ALSA・X11系・freetype・fontconfig（開発環境にはインストール済み）。Windows固有の処理は`#if JUCE_WINDOWS`。

### 2.4 GitHub Actions（`.github/workflows/build.yml`）

- トリガ: push、pull_request、workflow_dispatch。`concurrency`で同じrefの古い実行を取り消す。
- windowsジョブ（windows-latest、timeout 60分）: checkout → `cmake -S . -B build -A x64` → `cmake --build build --config Release --parallel` → `ctest --test-dir build -C Release --output-on-failure` → vswhereで`dumpbin.exe`を探し、`dumpbin /dependents`をexeとテスト実行ファイルにかけて`(?i)(vcruntime|msvcp|ucrtbase|api-ms-win-crt|concrt|vccorlib)`に一致したら失敗 → exeを`actions/upload-artifact`。
- linuxジョブ（ubuntu-24.04）: aptでJUCEの依存とninja-buildを入れ、`-G Ninja -DCMAKE_BUILD_TYPE=Release`でアプリとテストをビルドし、ctestを全部実行。

### 2.5 クラスの責務と公開インターフェース

**Params.h**

```cpp
enum class Preset { Normal, Echo, Helium, Minion, Giant, Kerokero, Robot, Talkbox };
enum class Effect { None, Echo, Robot, Talkbox };
struct PresetSpec { const char* id; float semitones; float formant; Effect effect; bool needsDetector; };
constexpr std::array<PresetSpec, 8> kPresets;   // spec.mdの表どおり
struct AtomicParams { std::atomic<float> gainDb{0}, reverb{0}; std::atomic<int> pitch{0}, preset{0}; std::atomic<bool> enabled{true}; };
struct SavedSettings { juce::String inputDevice, outputDevice; float gainDb; int pitch; float reverb; Preset preset; bool enabled; bool trayNoticeShown; };
SavedSettings sanitize (SavedSettings) noexcept;
Preset presetFromId (const juce::String&) noexcept;       // 不明な名前はNormal
bool shifterShouldRun (Preset, int pitch) noexcept;       // Helium/Minion/Giant/Kerokero、またはpitch != 0
```

`static_assert(std::atomic<float>::is_always_lock_free)`を置く。

**ResamplingFifo（入力スレッドと出力スレッドのSPSC）**

```cpp
void prepare (double inRate, double outRate, int maxInBlock, int maxOutBlock, int inBlock, int outBlock); // メッセージスレッド
void push (const float* mono, int n) noexcept;   // 入力スレッドのみ。空きが足りない分は捨ててoverrun++
void pull (float* out, int n) noexcept;          // 出力スレッドのみ
struct Stats { std::atomic<float> fillSmoothedSamples, targetSamples, speedCorrectionPpm, jitterMarginMs; std::atomic<uint32_t> underruns, overruns, discards; };
const Stats& stats() const noexcept;
double getLatencyMs() const noexcept;            // 平滑化した読み出し直後の充填量 + Bo/2（入力レートで換算）
```

- 内部: `juce::AbstractFifo` + バッファ（入力レートで1秒）+ `juce::LagrangeInterpolator` + 線形化用スクラッチ（`ceil(maxOutBlock*公称比*1.001)+8`）。
- `pull()`: 必要量`needed = ceil(speed*n)+2` → `prepareToRead`の2領域をスクラッチへコピー（消費しない）→ `interp.process(speed, scratch, out, n, needed, 0)` → 戻り値の使用数だけ`finishedRead`。wrap引数は使わない。
- 充填量は**読み出し直後**に測る（D-006）。平滑化は1次IIR（時定数2秒）。
- 速度比 = 公称比 × (1 + clamp(1e-3 × (平滑充填 − 目標)/目標, ±1e-3))。比例制御のみで定常偏差が残る（100ppmで目標の約10%）ことを`ponytail:`コメントに書く。
- 状態は`Refilling` / `Running`。再充填の完了条件は「充填量 ≥ 目標 + needed」で、2msのフェードインで再開。アンダーラン（充填量 < needed）時は手元のサンプルで出力し末尾に最大2msのフェードアウト → 無音、`underruns++`、ジッタ余裕+2ms（上限20ms）、`interp.reset()`、`Refilling`へ。充填量が目標の3倍超なら、2msフェードアウト → 目標まで破棄 → 2msフェードイン、`discards++`。
- 入力側のモノラル化はAudioIOの入力コールバックで行う（有効チャンネルの平均。nullポインタは飛ばす）。

**PitchShifter**

```cpp
enum class State { Resting, Priming, FadingIn, Active, FadingOut };
void prepare (double fs, int maxBlock); // presetDefault（120ms・30ms、D-014）、クロスフェード用バッファ、ウォームアップ
void setTarget (bool run, float semitones, float formantFactor) noexcept; // setTransposeSemitones / setFormantFactor(f, true)
void process (const float* in, float* out, int n) noexcept;  // n ≤ maxBlock
void reset() noexcept;                                       // → Resting
int  getLatencySamples() const noexcept;  // FadingIn / Active / FadingOut のとき inputLatency()+outputLatency()、それ以外0
State getState() const noexcept;
// ブロック長は120ms・インターバル30msに固定（D-014。decideStretchBlockは廃止）
```

状態遷移（20msのクロスフェード。各ランプは現在のゲイン値から続けて動かし、途中で逆向きに戻しても不連続にしない）:

| 現在 | 条件 | 遷移先 | 補足 |
|---|---|---|---|
| Resting | 稼働要求 | Priming | Stretchを`reset()` |
| Priming | 送り込み量 ≥ inputLatency+outputLatency | FadingIn | Priming中はdryを出す |
| Priming | 休止要求 | Resting | |
| FadingIn | ランプ完了 | Active | |
| FadingIn | 休止要求 | FadingOut | 現在のゲインから反転 |
| Active | 休止要求 | FadingOut | |
| FadingOut | ランプ完了 | Resting | Stretchへの入力を止める |
| FadingOut | 稼働要求 | FadingIn | resetしない |

稼働判定はプリセットとpitchで行う（`shifterShouldRun`）。ケロケロは補正量が0付近でも常に稼働させ、休止と稼働の往復を防ぐ。

**PitchDetector（YIN）**

```cpp
void prepare (double fs, int maxBlock);  // 間引き係数 D = round(fs/12000)。AAFは dsp::FilterDesign の高次Butterworth
void reset() noexcept;
void process (const float* in, int n) noexcept;  // ホップ約5msごとに推定を更新
float getFrequencyHz() const noexcept;  // 無声のときは直前の値を保持
float getVoicing() const noexcept;      // 0..1。RMSが閾値未満なら0
```

12kHzで窓W≈256、τは12〜171、直接計算（約9M MAC/s）。

**Effects**

```cpp
class Echo          { void prepare(double fs, int maxBlock); void reset() noexcept; void process(const float* in, float* out, int n) noexcept; };
class RingModulator { /* 同じ形。40Hzの位相累積 */ };
class Talkbox       { void prepare(double fs, int maxBlock); void reset() noexcept;
                      void process(const float* modulator, float* out, int n, float carrierHz, float voicing) noexcept; };
```

- Echo: `juce::dsp::DelayLine<float, None>`（最大0.35秒）、帰還路に3.5kHzのローパス。dry 1.0 + wet 0.6。`reset()`はバッファを消去し、その後20msはディレイへの書き込みにフェードインをかける（消去直後の最初のサンプルが300ms後に段差として出るのを防ぐ）。
- Talkbox: 20バンド × 2段のバンドパス（係数はprepareで生成）を変調側とキャリア側に持つ。包絡は全波整流 + 1次ローパス（アタック5ms、リリース20ms）。キャリアはPolyBLEP鋸波と`juce::Random`の白色雑音を有声度で混ぜる。キャリア周波数は`SmoothedValue`（約5ms）で補間。固定の補正ゲインはテストで決める。

**Engine**

```cpp
struct EngineConfig { double sampleRate; int maxBlock; };
void prepare (const EngineConfig&);            // メッセージスレッド。デバイス停止中のみ
AtomicParams& params() noexcept;
void process (float* monoInOut, int n) noexcept; // n > maxBlock なら maxBlock ごとに分割
float takeInputPeak() noexcept;                  // exchange(0)
int   getShifterLatencySamples() const noexcept;
uint32_t getErrorFlags() const noexcept;         // bit0: 非有限値, bit1: 例外（AudioIOが立てる）
void  clearErrorFlags() noexcept;
```

ブロックごとの処理:

1. atomicを読む。
2. 入力ピークを記録する。
3. 必要ならピッチ検出を回す（切替中は旧プリセットの分も含める）。
4. シフターの目標を設定する（移調 = pitch + preset + ケロケロ補正、フォルマント = presetの係数）。
5. シフターを通す。
6. 層2の効果を通す（切替中は旧と新を別バッファで処理し20msでクロスフェード。切替中に来た次の変更は切替完了まで保留）。
7. リバーブ（自前の`SmoothedValue`でrを補間。0かつ補間完了なら処理を止めて`reset()`。`Parameters`には wetLevel = r/3、dryLevel = (1−r)/2、width = 1 を渡す。D-008）。
8. ゲイン（`SmoothedValue` Multiplicative、50ms）。
9. 非有限値の検査 → リミッター（D-007: `dsp::Compressor` 閾値-1dB・比100・アタック0.1ms・リリース100ms + `FloatVectorOperations::clip`で±1.0）→ 非有限値の検査。非有限値があればブロックを無音にし、全DSPを`reset()`し、フラグを立てる。
10. バイパス切替は20msのクロスフェード。完全バイパス中はチェーンを処理しない。解除時はチェーンを`reset()`（シフターはPriming経由で復帰）。**最終出力の非有限値検査はバイパス中も常に行う**（D-010）。

**AudioIO（app、メッセージスレッドで操作）**

```cpp
StringArray getInputNames(), getOutputNames();
bool open (const String& inName, const String& outName);  // 両方停止 → 作成 → open → prepare → start
void close();
struct DeviceInfo { String name; bool lowLatency; double rate; int bufferSize; int latencySamples; };
DeviceInfo getInputInfo() const, getOutputInfo() const;
LatencyBreakdown getLatency() const;   // デバイス入出力 / リングバッファ / シフター
float getCpuLoad() const noexcept;     // 出力コールバック内の処理時間 ÷ 担当音声長（平滑化）
String getErrorText() const;
```

- WASAPIの型インスタンスを2つ（`sharedLowLatency`と`shared`）持ち、両方で`scanForDevices()`。
- 入力と出力それぞれ、まず低遅延モードで`createDevice` + `open(全ch, rate 0, getAvailableBufferSizes()の最小)`、失敗したら共有モードで開き直す。
- 開いた後: デバイス遅延 = `getInput/OutputLatencyInSamples()/rate`、リング目標 = max(各バッファ長) + 2ms、`Engine::prepare`（ブロック長は120ms固定、D-014）と`ResamplingFifo::prepare`。その後、入力、出力の順で`start`。
- コールバックは入力用と出力用の内部クラスに分ける。どちらも`ScopedNoDenormals`とtry/catch、呼び出し回数のatomicカウンタ。出力は`getHighResolutionTicks`で計時し、pull → Engine → 全チャンネルへ複製。
- `audioDeviceError`ではatomicフラグだけ立てる。`audioDeviceAboutToStart`の2回目以降でレートやバッファ長が変わっていたら、出力を無音にするフラグと再オープン要求フラグを立てる。
- Linuxでは`AudioDeviceManager::createAudioDeviceTypes`で得た先頭の型を1つだけ使う。
- 500msの`juce::Timer`で`ConnectionMonitor`を評価する。`audioDeviceListChanged`も同じ評価関数に流す。

**ConnectionMonitor（core、純粋ロジック）**

```cpp
enum class Action { None, CloseAndFail, TryReopen };
Action update (int64 nowMs, uint64 inCount, uint64 outCount, bool errorFlag, bool reopenRequested, bool devicesPresent, bool listChanged) noexcept;
bool isFailed() const noexcept;
```

入力・出力それぞれのコールバックカウンタがどちらか一方でも2秒進まない・エラーフラグ・再オープン要求・「一覧変更通知あり かつ 使用中デバイスが一覧に無い」のいずれかで`CloseAndFail`。異常状態の間は、一覧変更通知が来てデバイスがあれば即`TryReopen`、それ以外は2秒ごとに`TryReopen`。片方だけの異常でも両方を閉じ、両方揃ってから両方開き直す（D-009）。

**StatsLog（core）**: `juce::String formatStatsLine (const StatsSnapshot&)`と`void resetIfLarger (const juce::File&, juce::int64 limitBytes)`。60秒タイマーと追記はMain.cpp。

**Main.cpp**

- 設定: `PropertiesFile`（applicationName "VoiceChange"、folderName "VoiceChange"、filenameSuffix ".settings"、millisecondsBeforeSaving 1000）、読み込み時に`sanitize`。終了時は`saveIfNeeded()`。初回格納通知の済みフラグも保存する。
- トレイ: `SystemTrayIconComponent`の派生クラス、アイコンはコード内で描画。左クリックでウィンドウ表示。右クリックで`Process::makeForegroundProcess()`の後に`PopupMenu::showMenuAsync`。
- ウィンドウ: `closeButtonPressed`で非表示。
- VB-CABLE未検出ダイアログ: `DialogWindow::LaunchOptions::create()`で非モーダル。「公式サイトを開く」は`URL::launchInDefaultBrowser`。
- `--screenshot <path>`: 起動後に`createComponentSnapshot`をPNGで保存して終了（Xvfb確認用）。
- `moreThanOneInstanceAllowed()`は false（D-009）。
- 起動時に保存済みデバイスが見つからない場合は既定デバイスへ切り替えず、エラー表示と再試行を続ける（D-009）。

**MainComponent**: docs/design.mdに従う。30fpsの`Timer`は`visibilityChanged`で開始・停止。

## 3. 作業手順

依存関係: T-002 → T-003 → T-004 → T-005 → T-008。T-006はT-004とdocs/design.mdが必要。T-007はT-003とT-006の後。

ローカル検証（Linux）:

```
cmake -S /home/user/VoiceChange -B /home/user/VoiceChange/build -G Ninja -DCMAKE_BUILD_TYPE=Release [FETCHCONTENT_SOURCE_DIR_* の指定]
cmake --build /home/user/VoiceChange/build --parallel
ctest --test-dir /home/user/VoiceChange/build --output-on-failure            # 全部
ctest --test-dir /home/user/VoiceChange/build --output-on-failure -L quick   # 開発中
```

### T-002: CMake構成・依存の固定・CI・README

- 変更対象: `CMakeLists.txt`、`.gitignore`、`.github/workflows/build.yml`、`README.md`、`src/app/Main.cpp`（空ウィンドウ）、`src/core/Params.h`（プリセット表のみ）、`tests/TestMain.cpp`、`tests/TestSignals.h`（土台）、`tests/ShifterTests.cpp`（Smokeのみ）。
- README: アプリ用に全面的に書き換える（テンプレート説明は置き換え、開発フローはAGENTS.mdを参照させる）。目的、ビルド手順（Windows/Linux）、依存と選定理由（ユーザー指示の選定理由4項目を転記）、Rubber Band・排他モードを使わない理由、ライセンス（JUCEのAGPLv3/商用、Signalsmith Stretch/linearのMIT。exeを配布する場合の注意）。
- テスト Smoke: 48kHzで`configure(1, 960, 240)`のとき`inputLatency()+outputLatency()`が960×[0.95, 1.05]。1秒分の正弦を処理して出力が有限値。
- 完了条件: ローカル検証で構成とビルド（アプリとテスト）が成功しSmokeが通る。CIのwindowsジョブ（ビルド、ctest、dumpbin検査、artifact）とlinuxジョブが成功する。dumpbinの出力にVCランタイム/UCRTのDLLがない。

### T-003: 入出力の分離・リングバッファ・クロックずれ補正とオフライン模擬

- 変更対象: `src/core/ResamplingFifo.*`、`src/app/AudioIO.*`（2デバイスのopen/フォールバック/コールバック。出力はpullした音をそのまま流す暫定処理。ウォッチドッグはT-007）、`src/app/Main.cpp`、`tests/RingBufferTests.cpp`、`tests/TestMain.cpp`（アロケーション検出フック）、`CMakeLists.txt`。
- 模擬ドライバ（テスト内、離散イベント方式）: 入力イベント時刻 = k·Bi/(fs_in·(1+ppm_in)) + ジッタ、出力も同様。ジッタは固定シードの一様乱数で、各ストリーム内で時刻が逆転しないようにする。早い方のイベントから処理し、入力は入力クロック同期の三角波をpush、出力はpullして検査する。

| ID | 内容 | 合格条件 |
|---|---|---|
| R1 | 起動直後の再充填 | 充填量 ≥ 目標+needed まで出力は全て0。再開後の最初の2msは包絡が単調増加 |
| R2 | 入力停止によるアンダーラン | underruns+1、ジッタ余裕+2ms。フェードアウト区間の隣接差最大値 ≤ 1.5×(三角波の傾き + A/フェード長)。以後0。入力再開で再充填から復帰 |
| R3 | アンダーラン20回 | ジッタ余裕 = 20ms |
| R4 | 出力を2秒停止 | overruns > 0。再開後の最初のpullでdiscards = 1、充填量 ≤ 目標+1ブロック。30秒以内に平滑充填が目標±25% |
| R5 | 44.1kHz→48kHz、1kHz正弦 | FFTピークが1000Hz±0.1% |
| R6 | アロケーション | push/pull中のアロケーション0回 |

1時間模擬（RingBufferLong。各シナリオ音声時間3600秒）:

| ID | 入力→出力 | ずれ | Bi / Bo | ジッタ |
|---|---|---|---|---|
| S1 | 48k→48k | 入力+100ppm | 480/480 | なし |
| S2 | 48k→48k | 入力−100ppm | 480/480 | なし |
| S3 | 44.1k→48k | +100ppm | 441/480 | なし |
| S4 | 48k→44.1k | −100ppm | 480/441 | なし |
| S5 | 48k→48k | +50ppm | 128/1024 | なし |
| S6 | 44.1k→48k | −100ppm | 1024/144 | なし |
| S7 | 48k→48k | +100ppm | 480/480 | 各コールバック0〜3ms |
| S8 | 48k→48k | +100ppm | 480/480 | 約30秒ごとに入力を1回15ms遅延 |

S1〜S7の合格条件（すべて）:

- a) 初期充填後のunderrunsが0
- b) overruns 0、discards 0
- c) t ≥ 5分で |平滑充填 − 目標| ≤ 0.25×目標
- d) 平滑充填の平均（最後の10分）− 平均（5〜15分）≤ 0.5ms相当
- e) t ≥ 10分の速度比補正の平均が期待値 (1+ppm_in)/(1+ppm_out)−1 の±10ppm以内
- f) 入力は周期2秒・振幅0.5の三角波。出力の2階差分 |x[n]−2x[n−1]+x[n−2]| が 0.25×傾き を超えるサンプルが初期充填後に0個（折り返し点付近 |x| > 0.5 − 4×入力傾き は除外）。線形区間ではLagrange補間が厳密になるため2階差分はfloatの丸め程度に収まり、1サンプルの欠落・重複は傾きと同程度の段差になる。

S8: underruns合計9以下、10分以降0、ジッタ余裕 ≤ 20ms、b〜fはt ≥ 10分で成立。

完了条件: ローカル検証でRingBufferとRingBufferLongが通り、CIの両ジョブが成功する。実デバイスでの挙動は未検証として報告する。

### T-004: 層1・ピッチシフターの休止・ブロック長の決定

- 変更対象: `src/core/PitchShifter.*`、`src/core/Engine.*`、`src/core/Params.h`、`src/app/AudioIO.cpp`、`tests/ShifterTests.cpp`、`tests/EngineTests.cpp`。
- クリック判定器（TestSignals.h、全テスト共通）: 切替区間の隣接差最大値 ≤ 1.5 × max(切替前の定常区間の隣接差最大値, 切替後の同値)。定常区間は各1秒で、シフターの遅延が落ち着いた後から取る。自己テスト: 位相が跳ぶ2本の正弦の単純連結は必ず検出、20msの二乗余弦クロスフェード連結は合格。入力の先頭には-80dBFSの雑音を置く（Stretchのデジタル無音モードを避ける）。

| ID | 内容 | 合格条件 |
|---|---|---|
| P1 | 状態遷移 | Priming → FadingIn が送り込み量 = inputLatency+outputLatency（誤差1ブロック以内）。FadingInの長さ = 20ms±1サンプル。報告レイテンシはResting/Primingで0、それ以外でL |
| P2 | 全遷移のクリック | Resting→Active、Active→Resting、Priming中の中止、FadingIn中の反転、FadingOut中の反転を、200Hz・A=0.3の正弦で判定器にかけて合格 |
| P3 | 移調精度 | 220Hz正弦、±12、±5、+8、−6半音。FFTピークが 220·2^(s/12) の±1% |
| P4a | 層1ピッチでフォルマント保持（合格基準） | 合成母音（f0=150Hz、/a/: F1=730, F2=1090, F3=2440）、±5半音。f0比±1%、包絡スケール s = 1.0±10% |
| P4b | 層1ピッチでフォルマント保持（低い声の特性確認、D-015） | 合成母音（f0=100Hz、同上）、±5半音。f0比±2%、包絡スケール s = 1.0±25%。Signalsmith Stretchの既知の特性（D-015）で、実測基準値はf0誤差最大+1.4%・s=0.795(-5半音)/1.165(+5半音)。これより悪化したら回帰 |
| P5 | ブロック長 | 48kHz・44.1kHz・96kHzで inputLatency+outputLatency が120ms×[0.95, 1.05]（D-014） |
| E1 | ゲイン | 1kHz・-20dBFSで+6dB → 補間完了後のRMSが+6dB±0.1dB。変更時のクリックなし |
| E2 | リミッター | ゲイン+20dBで0dBFS級の正弦 → 全サンプル |x| ≤ 1.0。-20dBFS・ゲイン0dBでRMS変化 ≤ 0.1dB（透過性） |
| E3 | リバーブ | r: 0→0.01でRMS変化 ≤ 0.5dB。r=0.5で入力停止後200msのRMSが入力比-40dB超。r→0の補間完了後はリバーブ停止 |
| E4 | バイパス | 完全バイパス中の出力が入力とビット一致。ON/OFFでクリックなし |
| E5 | NaN/Inf注入 | 1ブロックにNaN（別ケースでInf、ミニオン稼働中）→ そのブロックの出力が全て0、エラーフラグ。以後の正常入力でL + 2ブロック以内に非ゼロへ復帰し、以後全て有限。バイパス中にNaNを入力した場合も出力0・フラグ |
| E6 | 分割処理 | n = 3×maxBlock+17 の一括処理とmaxBlockごとの処理の出力がビット一致 |
| E7 | アロケーション | 全プリセット・全遷移の`process`中のアロケーション0回 |

完了条件: ローカル検証でShifterとEngineが通り、CIの両ジョブが成功する。

### T-005: 層2プリセット

- 順序: エコー → ロボット → ヘリウム → ミニオン → ジャイアント → ピッチ検出 → ケロケロ → トークボックス。
- 変更対象: `src/core/Effects.*`、`src/core/PitchDetector.*`、`src/core/Engine.cpp`、`tests/EffectsTests.cpp`、`tests/EngineTests.cpp`、`README.md`（測定結果）、`docs/spec.md`（数値を調整した場合）。
- フォルマント移動の測定（TestSignals.h）:
  1. 合成母音は加算合成。k次倍音の振幅 = 3つの2極共振器（F1/F2/F3、帯域幅80/90/120Hz）の k·f0 での振幅応答の積。
  2. 定常区間1秒以上をHann窓の16384点FFTで平均し、f0の各倍音位置±3%でピークを取り、対数振幅を対数周波数軸（200〜4000Hz）上で線形補間して包絡 E(f) を作る。
  3. 包絡スケール s は、E_out(s·f) と E_in(f) の相関が最大になる s を [0.5, 2] で0.5%刻みに探す。
  4. 補助としてF1/F2のピーク（倍音3点の放物線補間）も出力する。f0比は基本波ピークの放物線補間。入力も同じ解析器で測り比を取る。

| ID | 内容 | 合格条件 |
|---|---|---|
| D1 | ピッチ検出の精度 | 正弦80/110/220/440/880Hz、帯域制限鋸波100/200/400Hz、合成母音f0=100/150/250Hz。ウォームアップ50ms後の有声フレームの95%以上が真値±1%、中央値誤差 ≤ 0.3% |
| D2 | 無声の判定 | 無音 → 全フレーム無声。白色雑音-20dBFS → 90%以上のフレームで有声度 < 0.5 |
| D3 | 別レート | 44.1k/96kでもD1と同じ |
| D4 | アロケーション | 0回 |
| F1 | ヘリウム（f0=150Hz、合格基準） | f0比1±1%、s = 1.6±10% |
| F2 | ミニオン（f0=150Hz、合格基準） | f0比 2^(8/12)±1%、s = 1.4±10% |
| F3 | ジャイアント（f0=150Hz、合格基準） | f0比 2^(−6/12)±1%、s = 0.75±10% |
| K1 | ケロケロ | 450Hz正弦 → 440Hz±1%。425Hz → 415.3Hz±1%。母音f0=150Hz → 146.8Hz±1%。100msの無音を挟んでも補正量が不変 |
| X1 | エコー | 200Hzのトーンバースト → 第1エコーが300ms±1ms・振幅比0.6±0.05、第2エコーが600ms・第1エコー比0.45±0.05。再選択で以前の残響が0 |
| X2 | ロボット | 1kHz正弦 → 960/1040Hzにピーク、1000Hz成分が30dB以上低下 |
| X3 | トークボックス | 母音3種×f0 3種・-20dBFSで出力RMSが入力比±3dB（固定補正ゲインは中央値から決め、READMEとspec.mdに記録）。倍音間隔が 検出f0×2^(p/12) の±1%。雑音入力で出力RMSが入力比-40dB超 |
| E8 | 全プリセットの異常値 | 母音・雑音バースト・無音を含む10秒（-60〜0dBFS）で、NaN/Infなし、ピーク ≤ 1.0 |
| E9 | 切替のクリック | 8×7=56通りの順序付き切替で判定器に合格。判定窓は切替から20ms + 300ms + 20msまで。デジタル無音から発声し始める瞬間も含める |
| E10 | CPU（参考値） | 48kHz・480ブロック・10秒で、プリセットごとの処理時間÷音声時間を表で出力（失敗判定なし）。Linuxでノーマル ≥ 1% またはトークボックス等 > 5% なら問題として報告 |

F1〜F3が通らなかった場合は閾値を緩めず、測定値を添えて報告する（PSOLA系への差し替え判断の材料にする）。P4と同じ方針（D-015）で、F1〜F3はf0=150Hzを合格基準とし、f0=100Hzでの特性確認（低い声でのフォルマント追従・f0ずれ）も測定してD-015の実測基準値と比較する（失敗判定なし。悪化していれば報告する）。

完了条件: ローカル検証でEffectsとEngineが通り、CIの両ジョブが成功する。READMEにフォルマント測定値（s、F1/F2、f0比）とCPUの表、PSOLA系への差し替え要否の判断が記載されている。

### T-006: UIの結合と設定の保存・復元

- 変更対象: `src/app/MainComponent.*`、`src/app/Main.cpp`（設定、VB-CABLEダイアログ、`--screenshot`）、`src/core/Params.h`（sanitize）、`tests/AppLogicTests.cpp`。
- テスト AppLogic（設定）: 範囲外の値（gain ±100dB、pitch ±50、reverb 2.0 / −1）が範囲の端へ丸められ、pitchは整数に丸める。不明なプリセット名 → Normal。キーがない場合 → 初期値。
- 完了条件: AppLogicが通る。`xvfb-run -a -s "-screen 0 1280x1024x24" build/VoiceChange_artefacts/Release/VoiceChange --screenshot <scratchpad>/ui.png`が終了コード0で終わり、PNGのサイズがdesign.mdの固定サイズと一致する。目視でdesign.mdと比較し、日本語が表示され要素の順序が仕様どおりであること。非表示中にタイマーが止まることはコードレビューで確認する。

### T-007: トレイ常駐・デバイス切断時の処理・エラー表示・統計ログ

- 変更対象: `src/core/ConnectionMonitor.h`、`src/core/StatsLog.*`、`src/app/AudioIO.*`、`src/app/Main.cpp`、`src/app/MainComponent.*`、`tests/AppLogicTests.cpp`。
- ConnectionMonitor: カウンタが2.0秒止まる → `CloseAndFail`、1.9秒 → `None`。異常状態で`TryReopen`が t=+2, +4, +6秒。一覧変更通知あり・デバイスあり → 即`TryReopen`。エラーフラグ → 即`CloseAndFail`。再オープン要求 → 即`CloseAndFail`の後`TryReopen`。
- StatsLog: 整形した行が全項目（経過時間、レイテンシ、充填量、アンダーラン/オーバーラン、補正ppm、CPU、メモリ）を含む。1MB超の一時ファイル → 作り直し後のサイズ0。1MB以下 → 変化なし。
- 完了条件: AppLogicが通り、CIの両ジョブが成功し、Linuxでアプリがビルドできる。トレイ、抜去、再接続、`GetProcessMemoryInfo`はREADMEの手動確認手順に入れ、未検証として報告する。

### T-008: 連続動作検証（オフライン模擬1時間）と実機テスト手順

- 変更対象: `tests/LongRunTests.cpp`、`README.md`。
- 模擬: ResamplingFifo（44.1k→48k、+100ppm、ジッタ0〜3ms）とEngineを組み合わせ、音声時間3600秒。10秒ごとにプリセットを巡回、7秒ごとにpitchを変更、60秒ごとにバイパスを切り替える。入力はピッチが滑る合成母音、雑音、無音の混合。
- 合格条件: NaN/Infなし、ピーク ≤ 1.0。初期充填後のunderruns 0、エラーフラグ0。全区間の`process`/`pull`/`push`中のアロケーション0回。T-003のc〜eを満たす。CPUの参考値を出力する。
- README: 実機1時間テストの手順と、ログでの判定基準（開始1分以降アンダーランが増えない、レイテンシ変動2ms以内、メモリ増加1時間で1MB以内、CPUがノーマル1%未満・重い効果5%以下）。
- 完了条件: LongRunが通り、CIの両ジョブが成功し、READMEに手順が記載されている。

各ステップ共通: Developerは`docs/progress.md`に記録する。設計判断は`docs/decisions.md`にD-011以降として追記する。ファイルが肥大化したら`// ===== SECTION: 名称 =====`のアンカーを置く。

## 4. 完了条件（計画全体）

- T-002〜T-008の各完了条件を満たす。
- 最終コミットでローカル検証の`ctest --output-on-failure`（quickとlong）が全件成功する。
- CIのwindowsジョブとlinuxジョブが成功している。
- 証拠（ctestの要約、CIのrun URL、dumpbin出力、UIのPNG）を示す。
- WASAPI固有の挙動、トレイ、抜去と再接続、聴感、1時間の実機動作は未検証であることを明記し、READMEの手順でユーザーに委ねる。

## 5. リアルタイム制約の守り方とテストでの担保

- 確保しない: バッファ（`AudioBuffer::setSize`、DelayLine、スクラッチ）は全て`prepare()`で確保する。`dsp::IIR::Coefficients::make*`と`FilterDesign`はprepareでだけ呼ぶ。音声パスで`juce::String`、`DBG`、`Logger`、`std::function`の生成、`push_back`/`resize`を使わない。Stretchの`configure`はprepareでだけ呼び、構築（`std::random_device`）はメッセージスレッドで行う。`prepare()`の最後に白色雑音と母音でシフターを数ブロック空回しする（vector容量の不足を先に出し切る）。
- ロックしない: UIとのやりとりはatomicだけ。`CriticalSection`/`std::mutex`/`SpinLock`を使わない。`AsyncUpdater::triggerAsyncUpdate`も音声スレッドから呼ばない（atomicフラグ + タイマーでのポーリング）。
- I/Oしない: ログはメッセージスレッドのタイマーから書く。
- `ScopedNoDenormals`を両方のコールバックに置き、`try/catch(...)`で処理全体を囲む。
- テストでの担保: `tests/TestMain.cpp`でグローバルの`operator new`/`new[]`/aligned版/nothrow版と対応するdeleteを置き換え、thread_localのフラグが立っている間の呼び出し回数を数える。`ScopedAllocationGuard`で`push`/`pull`/`Engine::process`/`PitchDetector::process`の呼び出しだけを囲み、回数0をexpectする（expectはガードの外で呼ぶ）。ロックとI/Oは`grep -rnE "mutex|CriticalSection|ScopedLock|SpinLock|DBG\(|Logger::|triggerAsyncUpdate" src/core`をReviewerのチェック項目にする。

## 6. 既知のリスク

- 休止⇔稼働の切替では、ブロック長分（20〜40ms）の音が重複または欠落する（D-002の帰結）。クロスフェードでクリックは防げるが、わずかな引っかかりとして聞こえ得る。
- Stretchの`process()`での稀なアロケーション。検出テストで見つかった場合はウォームアップで解消できるか確認し、だめなら報告する。
- ジッタ余裕が増えた後もブロック長は再計算しない（開いたときだけ計算する）。合計が50msを超えたらUIに表示される。
- 同名デバイスに付く番号（例「(2)」）は接続順で変わり得るため、保存したデバイス名と一致しなくなることがある（低）。
- プリセット名「ミニオン」は既存キャラクター名。一般配布する場合は名称の検討が必要（低）。

## 8. 追加計画: マイク処理（ノイズ除去＋EQ、T-009〜T-015）

要件はdocs/spec.md「マイク処理: ノイズ除去」「マイク処理: EQ」、判断はD-019〜D-025、UIはdocs/design.md 10章を正とする。

### 8.1 依存ソースから確認した事実

1. RNNoiseのビルド手段はautotoolsだけで、CMakeファイルはない。学習済みモデルはGitに含まれず、`src/rnnoise_data.c/.h`を含むtar.gz（v0.2は`rnnoise_data-0b50c45.tar.gz`、21MB）をmedia.xiph.orgから取得する。v0.2のスクリプトにチェックサム検査はない。
2. ライブラリのソースはdenoise.c、rnn.c、pitch.c、kiss_fft.c、celt_lpc.c、nnet.c、nnet_default.c、parse_lpcnet_weights.c、rnnoise_tables.c、および（モデル側の）rnnoise_data.c。x86/以下はCPU別最適化を有効にした場合のみ。
3. `rnnoise_create`は`malloc`を使う。`rnnoise_process_frame`（denoise.c・rnn.c・nnet.c）はスタック配列だけを使う。parse_lpcnet_weights.cの確保はファイル/バッファからモデルを読むときだけ。
4. 入出力はint16の値域のfloat。FRAME_SIZE 480、WINDOW_SIZE 960。v0.2には1フレームの先読み遅延を足すコミット（84fe83b）が入っている。
5. JUCEの`IIR::ArrayCoefficients`は`std::array`を返し、`Coefficients::operator=(std::array)`は`clearQuick()`＋`ensureStorageAllocated(max(8, Num))`で代入するため、容量があれば再確保しない（JUCE masterで確認。9.0.2での一致はT-012で確認）。
6. 既存のアロケーション検出（tests/TestMain.cpp）はC++の`operator new`だけを数え、Cの`malloc`は数えない。

### 8.2 実装方針

#### ファイル構成（追加分）

```
src/core/MicProcessing.h/.cpp   NoiseReducer（フレーミング・48kHz変換・RNNoise・混合・ゲート・インパクト抑制・状態遷移）、Equalizer
src/app/MicPanel.h/.cpp         マイク処理ウィンドウの内容（design.md 10章）
tests/MicTests.cpp              カテゴリ Mic（N0〜N12、EQ1〜EQ9、M1）
```

#### CMake

- FetchContent（既存と同じく、どのターゲットよりも前）:
  - `rnnoise`: GIT_REPOSITORY https://github.com/xiph/rnnoise.git、GIT_TAG v0.2、GIT_SHALLOW ON。CMakeLists.txtがないため、FetchContent_MakeAvailableは取得だけを行う。
  - `rnnoise_model`: URL https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz、URL_HASH SHA256=（T-009で取得したファイルから算出して固定）、DOWNLOAD_EXTRACT_TIMESTAMP ON。
- `add_library(vc_rnnoise STATIC …)`: 8.1-2のソース（x86/以下を除く）と`${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c`。includeは`${rnnoise_SOURCE_DIR}/include`（PUBLIC、SYSTEM）と、`${rnnoise_SOURCE_DIR}/src`・`${rnnoise_model_SOURCE_DIR}/src`（PRIVATE）。UNIXでは`m`をリンク。必要なコンパイル定義（`_USE_MATH_DEFINES`等）はT-009で決める。rnnoiseのソースは改変しない（必要になったら報告する）。
- JUCEを含まないCのライブラリなので、STATICでもD-005の懸念（JUCEソースの二重コンパイル）はない。`vc_core`のINTERFACEに`vc_rnnoise`を加える。`CMAKE_MSVC_RUNTIME_LIBRARY`はadd_libraryより前に設定済みのため、`/MT`が適用される。
- ローカル検証: `-DFETCHCONTENT_SOURCE_DIR_RNNOISE=<scratchpad>/rnnoise -DFETCHCONTENT_SOURCE_DIR_RNNOISE_MODEL=<scratchpad>/rnnoise_model`（モデルはscratchpadにtar.gzを展開したディレクトリ）。
- `VoiceChangeTests`にだけ`juce::juce_audio_formats`をリンクする（T-014のWAV入出力用）。新規パッケージではない。
- ctest: `vc_add_test(mic Mic)`（quick）。
- CI: モデルのtar.gzを`actions/cache`（キーはファイル名）で保存し、CMakeのURLに「ローカルのキャッシュ → media.xiph.org」の順で渡す（ハッシュ検査は両方に効く）。方式の細部はT-009で決める。

#### 公開インターフェース

```cpp
// Params.h（追加）
enum class EqType { Peak, LowShelf, HighShelf, LowCut, HighCut };
constexpr int kEqBands = 5;
struct EqBandSettings { bool on; EqType type; float hz, gainDb, q; };
constexpr std::array<EqBandSettings, kEqBands> kEqDefaults;  // spec.mdの初期値（仮）。T-015でSonarの再現値に置き換える
struct EqBandAtomic { std::atomic<bool> on; std::atomic<int> type; std::atomic<float> hz, gainDb, q; };
// AtomicParams に追加
std::atomic<bool>  nrEnabled { false };
std::atomic<float> nrBackground { 0.7f }, nrImpact { 0.0f };   // 0〜1
std::atomic<bool>  eqEnabled { false };
std::array<EqBandAtomic, kEqBands> eqBands;                    // 初期値はkEqDefaults
// SavedSettings に同じ項目を追加し、sanitize()で非有限値→初期値、範囲の端へ丸める
const char* eqTypeId (EqType) noexcept;  EqType eqTypeFromId (const juce::String&) noexcept;  // 不明ならPeak

// MicProcessing.h
class NoiseReducer {
public:
    void prepare (double fs, int maxBlock);            // メッセージスレッド。初回はrnnoise_create、以後はrnnoise_init。FIFO・遅延線・補間器を確保。rnnoise_destroyは、unique_ptrのデリータ（デストラクタ）
    void setTarget (bool run, float background, float impact) noexcept;
    bool process (float* buf, int n) noexcept;         // その場で処理、n ≤ maxBlock。Restingなら何もしない。入力に非有限値があればfalse（bufとパイプラインは触れない。呼び出し側がreset()）
    void reset() noexcept;                             // → Resting。rnnoise_init、FIFO・遅延線・包絡を消去（確保なし）
    int  getLatencySamples() const noexcept;           // FadingIn/Active/FadingOutでD、それ以外は0（PitchShifterと同じ規則）
};
class Equalizer {
public:
    void prepare (double fs, int maxBlock);            // Coefficientsをmake*で作成（ここだけで確保）
    void setTarget (bool run, const std::array<EqBandSettings, kEqBands>&) noexcept;
    void process (float* buf, int n) noexcept;         // OFFのフェード完了後は何もしない
    void reset() noexcept;
};
```

- NoiseReducerの状態遷移は2.5節PitchShifterの表と同じ（Priming完了条件は「送り込み量 ≥ D」）。共通化はしない（同じ表の2か所目。3か所目が出たら検討する）。
- D（ノイズ除去の遅延）は、48kHzでは480 + RNNoise固有の遅延（T-009の実測960。D = 1440）。48kHz以外では出力レートへ換算し、補間器の遅延を加えて整数に丸める（実装: 出力FIFOの初期充填P = ceil(480×fs/48000) + 4 + 2×ceil(fs/48000)、D = P + round(959.7×fs/48000 + 2 + 2×fs/48000)。余裕はレート比に比例させる（下げ側の端数の持ち越しがレート比に比例するため。192kのblock 1/2/7で枯渇した）。959.7はRNNoise固有遅延の実測値［放物線補間］で、960に丸めると192kで1サンプル超ずれる。44.1kで1333、96kで2893、192kで5781）。原音側の遅延線も同じDを使う。
- 48kHz以外: 入力FIFO（出力レート）→ 上げ用のLagrange（速度比 = fs/48000）で480サンプルを作る → RNNoise → 下げ用のLagrange（速度比 = 48000/fs）→ 出力FIFO。出力FIFOは初期充填（ceil(480×fs/48000) + 余裕）を持ち、どんなブロック列でも枯れないこと（テストN11）。
- RNNoiseの計算はフレームがそろったコールバックに集中する。小さいバッファでは、そのコールバックだけ処理時間が伸びる（CPU表示は平滑化されるため影響は小さい）。

#### Engineへの組み込み

- `processChunk`: `updateInputPeak`の直後、バイパス判定の前に`noiseReducer.setTarget/process` → `equalizer.setTarget/process`を置く（D-020）。以降の`dryScratch`はマイク処理後の信号になる。
- `resetChain()`は層1・層2だけにする（現状の内容のまま）。`resetMic()`（noiseReducer/equalizerのreset）を新設し、`handleNonFinite`は両方を呼ぶ。バイパス解除時は`resetChain()`だけを呼ぶ。
- `int getNoiseReducerLatencySamples() const noexcept`を追加する。`AudioIO::getLatency()`は`LatencyBreakdown::noiseMs`を加えてtotalに含める。W2（device + ring > 48ms）の式は変えない。
- atomicは各フィールドを独立に読む（1バンドの周波数とゲインが1ブロックだけ新旧混在しうるが、どちらも有効値で補間されるため許容する）。

#### RT制約のテストでの担保（追加）

- tests/TestMain.cppに、glibc環境（`#if defined(__GLIBC__)`）でのみ`malloc/calloc/realloc/free/aligned_alloc/posix_memalign`を置き換えて`__libc_*`へ転送し、ガード中の呼び出しを数える処理を足す。Cライブラリ（RNNoise）の確保も検出できるようにするため。Windowsではソースレビューで代える。

### 8.3 作業手順

依存関係: T-009 → T-010 → {T-011, T-012} → T-013（design.md 10章をDesignerが確定してから）→ T-014 → T-015（ユーザーの素材を受け取ってから）。

#### T-009: RNNoiseの取り込み・ビルド確認（MSVC /MT・Linux）と遅延・CPUの実測

- 変更対象: `CMakeLists.txt`、`.github/workflows/build.yml`（モデルのキャッシュ）、`tests/MicTests.cpp`（N0のみ）、`tests/TestMain.cpp`（mallocの計数）、`README.md`（依存とライセンス: RNNoise BSD-3-Clause、モデルの出典）。

| ID | 内容 | 合格条件 |
|---|---|---|
| N0a | フレーム長 | `rnnoise_get_frame_size() == 480` |
| N0b | 基本動作 | 48kHzの正弦・白色雑音・無音を各1秒処理して、出力がすべて有限値 |
| N0c | 固有遅延 | 合成母音（ビブラート付き・不規則な音節長。インパルス列・1kHz正弦のバースト・雑音バーストはRNNoiseが雑音として抑えるため使わない）の相互相関で遅延を測り、480または960サンプル（±2）であること。値を出力して記録する |
| N0d | 確保 | `rnnoise_process_frame`と`rnnoise_init`（既定モデル）の呼び出し中の確保0回（new・malloc） |
| N0e | CPU（参考値） | 10秒分の処理時間÷音声時間を出力する（失敗判定なし） |

- 完了条件: CIの両ジョブが成功し、dumpbinにVCランタイム/UCRTのDLLがない。N0が通る。N0c・N0eの値をprogress.mdに記録し、spec.mdの「約20〜30ms」「3%」を実測値で更新する案をManagerへ出す。MSVCでビルドできない場合は、原因と回避案（コンパイル定義、CPU別最適化の無効化、v0.1.1の検討）を添えて報告し、先へ進まない。

#### T-010: NoiseReducer本体とEngineへの組み込み（混合まで）

- 変更対象: `src/core/MicProcessing.*`、`src/core/Params.h`、`src/core/Engine.*`、`src/app/AudioIO.*`（LatencyBreakdown）、`tests/MicTests.cpp`、`tests/TestSignals.h`（話し声に近い合成母音［ビブラート±2%・5Hz、4Hzの音節抑揚］、ピンク/ブラウン雑音、クリック、相互相関による遅延測定）。
- 範囲: フレーミング、48kHz変換、RNNoise、原音との混合（背景ノイズ0〜50%）、状態遷移、遅延の報告、異常値。ゲートとインパクト抑制はT-011。

| ID | 内容 | 合格条件 |
|---|---|---|
| N6 | 遅延 | 48k/44.1k/96kで、報告値 = 実測値±1サンプル。ブロック長を変えても一定 |
| N7 | アロケーション | ON/OFFの全遷移・パラメータ掃引・44.1k経路で`Engine::process`中の確保0回（new・malloc） |
| N8 | NaN/Inf | ノイズ除去ON（Active）で1ブロックにNaN（別ケースでInf）→ そのブロックの出力0、フラグ。以後D + 2ブロック以内に非ゼロへ復帰し、以後すべて有限。バイパス中でもノイズ除去ONなら同じ |
| N9a | 切り替えのクリック | ノイズ除去のOFF→ON、ON→OFF、Priming中の中止、FadingIn中の反転、FadingOut中の反転、背景ノイズ0↔100%の急変を、200Hz・A=0.3の正弦（-80dBFSの雑音付き）でクリック判定器に合格 |
| N10 | OFF時のビット一致 | ノイズ除去OFFのとき`NoiseReducer::process`の前後でmemcmp一致（\|x\|>1を含む乱数入力）。ON→OFFの遷移完了後も一致。既存のE4aが通る |
| N11 | 分割処理 | ブロック長{1, 7, 128, 441, 480, 4096}の混在列と一括処理（3×maxBlock+17）の出力がビット一致。44.1kで出力FIFOのアンダーフロー0回 |
| N3a | 清音の歪み（RNNoiseのみ） | 雑音のない合成母音（f0 = 120/200Hz）を背景ノイズ50%で処理し、レベル変化 ≤ 1.5dB、100〜4000Hzの1/3オクターブ長時間スペクトルの差 ≤ 3dB（暫定閾値） |
| N4a | 低い声（RNNoiseのみ） | f0 = 85/100/120Hzの清音の母音で、基本波成分の減衰 ≤ 3dB、全体 ≤ 1.5dB（暫定閾値） |

- 完了条件: MicとEngine（E1〜E10を変更なしで）、LongRunが通り、CIの両ジョブが成功する。暫定閾値から大きく外れた場合は閾値を緩めずに実測値を添えて報告する（D-013と同じ運用）。

#### T-011: VAD連動ゲートとインパクト抑制

- 変更対象: `src/core/MicProcessing.*`、`tests/MicTests.cpp`。

| ID | 内容 | 合格条件（暫定。実測基準値を記録する） |
|---|---|---|
| N1 | SNR改善量 | 合成母音（抑揚付き、発話1秒・無音1秒の繰り返し）にピンク雑音をSNR 10dBで重ね、背景ノイズ70%で、遅延を揃えた清音との区間SNRが入力比+6dB以上 |
| N2 | 無声区間の残留雑音 | 雑音だけの区間（開始1秒以降）の出力RMSが入力雑音比で、50%で−12dB以下、100%で−25dB以下。0/25/50/75/100%で残留量が単調に減る |
| N3b | 発話区間の歪み | N3aの条件で背景ノイズ100%でもレベル変化 ≤ 1.5dB。有声区間（冒頭50msを除く）でゲートが閉じたサンプルが0 |
| N4b | 低い声・語尾 | f0 = 85/100/120Hzで150msの指数減衰の語尾を持つ母音について、減衰開始から100msのエネルギーが背景ノイズ0%比で−3dB以内 |
| N5a | クリック除去量 | -50dBFSのピンク雑音上に、3kHzの1ms・5ms減衰のクリック（ピーク-20dBFS）。インパクト100%でクリック区間のピークがインパクト0%比で−12dB以下 |
| N5b | 誤検出 | 10msアタックの母音の立ち上がりと、合成の破裂音（5msの雑音バースト、30ms後に母音）で、インパクト100%でも後続母音の減衰 ≤ 1dB。インパクト0%では抑制段を通らない（0%と未使用でビット一致） |
| N7/N9b | 確保・クリック | ゲート・インパクトを含めてN7・N9aを再実行する。N9b: ノイズ除去ON（ゲートが働く背景ノイズ100%を含む）でのプリセット切り替え56通りを、E9と同じ判定器で確認する（ゲートで無音に近づいた信号でSignalsmith Stretchの無音モードに入らないか） |
| N12 | CPU（参考値） | 48kHz・480ブロック・10秒で、ノイズ除去ON（背景70%・インパクト50%、EQはOFF）のとき、ノーマル・トークボックス・ミニオンの処理時間÷音声時間を出力（失敗判定なし）。増分が3%を超えたら報告する。EQ込みの測定はT-012のEQ9 |

- 完了条件: MicとEngineが通り、CIの両ジョブが成功する。N1〜N5の実測値をREADMEに記載する。

#### T-012: EQ（Equalizer）とEngineへの組み込み、LongRunの拡張

- 変更対象: `src/core/MicProcessing.*`、`src/core/Params.h`、`src/core/Engine.*`、`tests/MicTests.cpp`、`tests/LongRunTests.cpp`。

| ID | 内容 | 合格条件 |
|---|---|---|
| EQ1 | 周波数特性 | 各タイプ（ピーキング1kHz +6dB Q1、ローシェルフ200Hz +6dB Q0.71、ハイシェルフ5kHz −4dB Q0.71、ローカット80Hz、ハイカット12kHz）で、インパルス応答（65536点FFT）の振幅が`getMagnitudeForFrequency`の解析値と20Hz〜0.45fsで0.1dB以内。48k/44.1k |
| EQ2 | 直列 | 5バンド同時の特性が各バンドのdB和と0.1dB以内 |
| EQ3 | クリック | ゲイン−12→+12dB、周波数100→5000Hz、Q 0.5→8、タイプ変更、バンドの有効/無効、EQ全体のON/OFFで、クリック判定器に合格 |
| EQ4 | 極端な値 | f = 20kHz（44.1kでは0.45fsへ丸め）、Q = 0.1/10、ゲイン±18dBで白色雑音を10秒処理し、有限・発散なし |
| EQ5 | アロケーション | パラメータ・タイプ変更中を含めて確保0回 |
| EQ6 | OFF時のビット一致 | EQ OFF（フェード完了後）でmemcmp一致 |
| EQ7 | 低い声 | 全バンドのゲインが0dBのフラット設定（kEqDefaultsの値には依存しない。T-015で置き換わるため）で85Hz正弦の変化 ≤ 0.1dB |
| EQ9 | CPU（参考値） | 48kHz・480ブロック・10秒で、EQのみON、およびノイズ除去（N12と同条件）＋EQ ONのとき、ノーマル・トークボックス・ミニオンの処理時間÷音声時間を出力（失敗判定なし）。ON時の増分が3%を超えたら報告する |

- LongRun: 既存のシナリオに「90秒ごとにノイズ除去のON/OFF、45秒ごとに背景ノイズとEQバンド1のゲインを変更」を加える。合格条件は既存と同じ（確保0回を含む）。
- 完了条件: Mic・Engine・LongRunが通り、CIの両ジョブが成功する。

#### T-013: 設定の保存・UI（マイク処理ボタン・マイク処理ウィンドウ・内訳表示）・統計ログ

- 前提: Designerがdesign.md 10章（Plannerの案）を確定していること。
- 変更対象: `src/core/Params.h`（SavedSettings・sanitize・タイプ名）、`src/app/Main.cpp`（設定キー、マイク処理ウィンドウの所有と表示/非表示、`--screenshot-mic <path>`）、`src/app/MainComponent.*`（ボタン、内訳、バイパス時の文言、Tab順）、`src/app/MicPanel.*`、`src/core/StatsLog.*`（`nr=` `eq=`）、`tests/AppLogicTests.cpp`。
- 設定キー: `nrEnabled`、`nrBackground`、`nrImpact`、`eqEnabled`、`eqBand{1..5}On`、`eqBand{1..5}Type`（`peak/lowshelf/highshelf/lowcut/highcut`）、`eqBand{1..5}Hz`、`eqBand{1..5}GainDb`、`eqBand{1..5}Q`。
- テスト AppLogic: 範囲外（背景2.0/−1、周波数5/50000、ゲイン±100、Q 0/100）が端へ丸まる。NaN/Inf → 初期値。不明なタイプ名 → peak。キーがない → 初期値。StatsLogの行に`nr=`・`eq=`が含まれる。
- 完了条件: AppLogicが通る。`--screenshot`と`--screenshot-mic`のPNGが460×600で、design.mdと目視で一致する（日本語の表示、要素の順序、内訳行がはみ出さない）。非表示中にタイマーが止まること、トレイ格納時にマイク処理ウィンドウも閉じることはコードレビューで確認する。

#### T-014: 実録音比較のオフライン処理ツールと手順

- 変更対象: `tests/TestMain.cpp`（`--process-wav`・`--compare`モード）、`tests/MicTests.cpp`（M1）、`CMakeLists.txt`（juce_audio_formats）、`README.md`（手順）。
- `VoiceChangeTests --process-wav <in.wav> <out.wav> [--settings <VoiceChange.settingsのパス>] [--bg 0.7 --impact 0.3 --eq on]`: WAVを読み、出力レート＝WAVのレートでEngine（プリセットはノーマル、ゲイン0）に通して書き出し、遅延を表示する。
- `VoiceChangeTests --compare <raw.wav> <sonar.wav> <ours.wav> [--segments <区間ファイル>]`: 区間（無音・発話・打鍵のみ・発話＋打鍵）ごとに次を表で出す。区間は台本の時刻ファイルで指定し、省略時は生音声のエネルギーで自動判定する。遅延は相互相関で揃える。
  - 無声区間の残留雑音RMS [dBFS] と1/3オクターブスペクトル
  - 発話区間のレベル
  - 発話と残留雑音の比（レベルに依存しない比較量）
  - 打鍵区間のピークとRMS
  - 発話区間の長時間平均スペクトルの比（sonar/raw＝Sonarの実効的なEQ、ours/raw）の差（63Hz〜8kHz、1/3オクターブ）
- M1: 既知の合成WAV（雑音床と発話レベルが既知）で、ツールの指標が期待値と0.5dB以内。
- READMEの録音手順（要点。詳細はT-014で書く）: OBSで物理マイクとSonarの仮想マイクを別トラックで同時に録音する。台本は約90秒（無音10秒 → 普通に話す30秒 → 話さずに打鍵10秒 → 話しながら打鍵20秒 → 無音10秒 → 小声と語尾を伸ばす発話10秒。空調・PCファンは普段どおり）。48kHzのWAVでraw.wavとsonar.wavを書き出し、区間の時刻を書いたメモを添える。Developerが`--process-wav`と`--compare`で比較表を作り、発話レベルを揃えたraw・sonar・oursの3本をユーザーへ渡す。ユーザーが聴き比べて採否と調整値を返す。
- 完了条件: M1が通り、READMEに録音と比較の手順がある。

#### T-015: Sonarの設定を初期値に反映し、実録音で評価する（ユーザーの素材待ち）

- 前提: ユーザーからSonarのスクリーンショット（EQと、Noise Reductionのスライダー値）と、同時録音のWAV 2種を受け取っていること。スクリーンショットには、使っているバンド数（5を超えるなら実装のバンド数を増やす）、各バンドのフィルタタイプと周波数・ゲイン・Q、ローカット等の傾きが写っていること。
- 変更対象: `src/core/Params.h`（kEqDefaults、背景ノイズ・インパクトの初期値）、`docs/spec.md`（初期値の表）、`tests/MicTests.cpp`（EQ8）、`README.md`（評価結果）。
- EQ8: kEqDefaultsの周波数特性が、スクリーンショットから読み取った曲線上の点と±1dB以内で一致する。Sonarとフィルタの定義が違う場合は換算の根拠をdecisions.mdに記録する。
- 評価: T-014のツールで比較表を作り、目標（本アプリの「発話と残留雑音の比」がSonar比で−3dB以内、打鍵区間でSonar比+3dB以内、80〜300Hzの長時間スペクトル差±2dB以内）と照らして報告する。目標は判断材料で、合否はユーザーの聴感で決める。
- 完了条件: 比較表と3ファイル（raw・sonar・ours、発話レベルを揃えたもの）をユーザーへ渡し、ユーザーの聴感の結果（採否・調整値）をprogress.mdとspec.mdに反映した。

### 8.4 完了条件（追加計画全体）

- T-009〜T-015の各完了条件を満たす。
- 最終コミットで、ローカル検証の`ctest --output-on-failure`（quickとlong）が全件成功し、CIのwindowsジョブとlinuxジョブが成功している。
- 証拠（ctestの要約、CIのrun URL、dumpbinの出力、2枚のUIのPNG、N0c/N12/EQ9の実測値、T-015の比較表）を示す。
- 実機でのCPU・遅延（統計ログ）と聴感は未検証として明記し、READMEの手順でユーザーに委ねる。

### 8.5 既知のリスク

- 遅延の増加（30ms、1440サンプル@48kHz）で、シフター休止時の合計が50msを超えうる。出力ブロックが480の倍数なら、フレーミングの遅延は480 − gcd(ブロック長, 480)まで縮められる。ただし、ブロック列の揺れに弱くなるため初期実装では採らない。必要になったらManager判断で検討する。
- 合成信号の評価はRNNoiseの実音声での挙動を代表しない。合成母音を雑音と誤認する、または逆がありうる。最終判断は実録音と聴感で行う。
- ゲートとインパクト抑制による、低い声の語尾・子音・破裂音の欠け。初期値はインパクト0%、ゲートは背景ノイズ50%超でのみ働く。
- ゲートで無音に近づいた信号でSignalsmith Stretchの無音モード（エネルギー<1e-15が2ブロック）に入る可能性。ノイズ除去ONでのプリセット切り替えを、E9と同じ判定器で確認する（T-011のN9b）。
- RNNoiseのMSVCビルドとモデル取得（media.xiph.orgへの依存）。
- EQで持ち上げた信号は、バイパス中はリミッターを通らず±1.0のクリップだけになる（D-018、D-020）。

### 8.6 レビューで確かめる点（Reviewer向け）

1. 音声スレッドでの確保: `rnnoise_create`/`destroy`がprepareとデストラクタ以外から呼ばれていないか。`Coefficients`への代入で再確保が起きないか（EQ5とglibcのmalloc計数が有効か）。`std::function`や`juce::String`の生成、ロックが音声パスに入っていないか（`grep -rnE "malloc|mutex|CriticalSection|DBG\(|Logger::" src/core`）。
2. ブロックの区切りへの非依存: 係数の再計算周期・ゲートのホールド・FIFOがサンプル数基準か（N11とE6で確認）。
3. 遅延の整合: 報告値と実測値が一致するか。原音側の遅延がRNNoise側と揃っているか（背景25%で清音の特性が平坦か。ずれると櫛形フィルタになる）。44.1kでの端数の丸め。
4. リセットの意味: バイパス解除でマイク処理をリセットしていないか。NaNではリセットしているか。`prepare`の再入（デバイスの開き直し）で二重生成やリークがないか。
5. 状態遷移: 全遷移でクリックがないか。遷移途中の報告遅延がPitchShifterと同じ規則か。
6. 低い声での誤動作: ゲートとインパクトの誤作動（N3b・N4b・N5b）。閾値が黙って緩められていないか。
7. ビット一致: OFF時のビット一致と、既存E4a（ノイズ除去・EQがOFFのときに限る）。バイパス時の文言がD-020の挙動と一致しているか。
8. 設定の検証: 非有限値・範囲外・不明なタイプ名の扱い。UIスレッドだけがatomicを書いているか。
9. EQの境界値: 44.1k/96kでの周波数の丸め、Q=10やナイキスト付近での安定性、補間中のタイプ変更。
10. CMake: Cターゲットに`/MT`がかかっているか（dumpbin）。モデルのハッシュを固定したか。FETCHCONTENT_SOURCE_DIR指定時にネットワークへ出ないか。READMEのライセンス記載。
11. UI: ミントの規則（バイパス中もマイク処理はミントのまま）。Tab順とスクリーンリーダーの題名。内訳行のはみ出し（スクリーンショットで確認）。マイク処理ウィンドウの初期フォーカスがスイッチではないこと。
