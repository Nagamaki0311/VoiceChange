# VoiceChange 実装計画（T-002〜T-008）

Plannerが作成し、Managerが7章の判断を確定した計画。要件はdocs/spec.md、UIはdocs/design.md、判断の経緯はdocs/decisions.mdを正とし、本書と食い違う場合はそちらを優先する。各タスクの完了時に、実際の構成と本書がずれた箇所は本書を更新する。

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
