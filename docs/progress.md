# 作業履歴

作業内容、実施結果、次回開始位置を記録する。新しいエントリは先頭に追加する（新しい順）。

## 記録フォーマット

```
## YYYY-MM-DD タスクID/概要

### 実施内容
- 何を行ったか

### 結果
- 動作確認結果、テスト結果など

### 次回開始位置
- 次に着手すべき場所（ファイル/関数/タスクID）
```

---

## 2026-09-28 T-003 入出力の分離・リングバッファ・クロックずれ補正とオフライン模擬

### 実施内容
- `src/core/ResamplingFifo.h/.cpp`: docs/plan.md 2.5節どおりに実装。`juce::AbstractFifo`（容量=入力レートで1秒）+ `juce::LagrangeInterpolator` + スクラッチ。充填量は`pull()`内で読み出し直後に測る（D-006）。状態機械は`Refilling`/`FadeIn`/`Idle`/`OverrunFadeOut`/`OverrunFadeIn`の5つ（`UnderrunFadeOut`は即時完結するため`handleUnderrun()`内で処理し予約のみ）。フェード区間は複数回の`pull()`呼び出しにまたがってよいよう`phaseCount`/`gain`をメンバに持たせて継続する。速度比の比例制御が定常偏差を残すこと（100ppmで目標の約10%）を`updateSpeedRatio()`の`ponytail:`コメントに明記した。
- `tests/AllocationGuard.h`（新規）: `ScopedAllocationGuard`と、TestMain.cppが定義するthread_localフラグ/カウンタのextern宣言。複数テストファイルから使うため独立ヘッダに切り出した。
- `tests/TestMain.cpp`: グローバルの`operator new`/`new[]`/aligned版/nothrow版と対応する`delete`（計16個）を置き換え、thread_localガード有効時の呼び出し回数を数えるフックを追加。実確保/解放は`std::malloc`/`std::free`（アラインメント版は前後に管理領域を足す手動アライン方式）。
- `tests/TestSignals.h`: `findFftPeakHz()`（Hann窓+FFT+放物線補間）を追加。本来T-004/T-005予定だが、R5（44.1k→48kHzのリサンプリング精度検証）に先行して必要だったため追加した。
- `tests/RingBufferTests.cpp`（新規）: カテゴリ`RingBuffer`にR1〜R6、`RingBufferLong`にS1〜S8を実装。離散イベント方式の模擬ドライバ（`EventClock`）をテスト内に実装し、S1〜S8はパラメータ化した`runScenario()`で共通処理する。
- `src/app/AudioIO.h/.cpp`（新規）: 入力・出力を別々の`AudioIODevice`として開く（D-001）。Windowsは`sharedLowLatency`→`shared`の順でフォールバック（`#if JUCE_WINDOWS`）、Linuxは`AudioDeviceManager::createAudioDeviceTypes`の先頭の型を1つ使用。入力コールバックは全入力chを平均してモノラル化しpush、出力コールバックはpullしてモノラルバッファを全出力chへ複製（Engineは未実装のためpullした音をそのまま出す暫定処理）。コールバック回数のatomicカウンタとエラーフラグを実装（ウォッチドッグ・再接続はT-007）。`getLatency()`でデバイス入出力/リングバッファの内訳を返す。
- `src/app/Main.cpp`: 起動時に`getInputNames()`/`getOutputNames()`の先頭（一覧の既定デバイス）で`AudioIO::open()`を試みる。一覧が空/openが失敗してもクラッシュしない（Linux/Xvfbでデバイス無しを想定）。
- `CMakeLists.txt`: `ring_buffer`（quick）と`ring_buffer_long`（long, LABELS long, TIMEOUT 1800）をctestに登録。

### 結果
- `cmake --build build --parallel`成功（アプリ・テスト両方）。
- `ctest --test-dir build --output-on-failure -L quick`: smoke・ring_buffer(R1〜R6)とも成功。
- `ctest --test-dir build --output-on-failure`（quick+long）: **ring_buffer_longがFAILED**。内訳は次のとおり（1回のフル実行、実時間約30秒）。
  - 合格: R1〜R6（全件）、S5、S6、S8。
  - 不合格: S1、S2、S3、S4、S7 — いずれも条件c)（t≥5分で|平滑充填−目標|≤0.25×目標）のみ不合格。他の条件（a,b,d,e,f）はS1〜S7全件で合格。
  - 実測値（cViolations=条件c)の違反サンプル数、位置No.は「7件中4件目のテスト＝条件c」のindex）:
    - S1 (48k→48k +100ppm, Bi/Bo=480/480): avgFillD=633.6, avgFillLast10=633.6, avgPpm=100.00(期待100.00), cViolations=52140, fViolations=0
    - S2 (48k→48k -100ppm): avgFillD=518.4, avgPpm=-100.00(期待-100.00), cViolations=52140
    - S3 (44.1k→48k +100ppm, 441/480): avgFillD=582.1, avgPpm=100.00, cViolations=52140
    - S4 (48k→44.1k -100ppm, 480/441): avgFillD=518.4, avgPpm=-100.00, cViolations=52140
    - S5 (48k→48k +50ppm, 128/1024): avgFillD=1175.3, avgPpm=50.05(期待50.00), cViolations=0 → 合格
    - S6 (44.1k→48k -100ppm, 1024/144): avgFillD=1001.0, avgPpm=-100.00, cViolations=0 → 合格
    - S7 (48k→48k +100ppm, ジッタ0-3ms): avgFillD=633.4, avgPpm=99.99, cViolations=25974
    - S8 (48k→48k +100ppm, 30秒ごと15ms遅延): underruns合計=4(≤9)、underrunsAfter600=0、jitterMarginMs=10.00(≤20)、b〜f全件0 → 合格
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（タイムアウトによる強制終了のみ、クラッシュなし）。ALSAの`seq_hw.c`警告（MIDIシーケンサ、無関係）のみ標準エラーに出力。

### 条件c)不合格の原因分析（S1〜S4, S7）
デバッグ用に`ResamplingFifo`へ一時アクセサ（`debugRawFilled()`。原因特定後に削除済み）を追加し、`fillSmoothed`と生の`fifo.getNumReady()`を10秒ごとにトレースして原因を特定した。

- S1(Bi=Bo=480, target=576)では、`fillSmoothed`が定常時576付近まで収束した後、**約100秒周期で生の充填量が480（Bi丸々1ブロック分）だけ瞬時にジャンプ**し、その後約80〜90秒かけて`fillSmoothed`が576へ戻る、という鋸歯状の挙動を繰り返す。このジャンプが「平滑充填−目標」を最大約63%（576→940超）まで押し上げ、0.25×目標(144)を大きく超える区間が生じる。
- 100秒という周期は、入力イベント周期のずれの累積が入力1ブロック分（Bi/fs_in秒）に達するまでの時間 T = Bi / (fs_in × |ppm_in/1e6|) と厳密に一致する（S1: 480/(48000×1e-4)=100.0秒）。これは離散イベント方式の模擬ドライバ（固定ブロックサイズ・イベント時刻のみドリフト、docs/plan.md 3章の式どおり）では、周波数がわずかに異なる2つの周期系列の間に必然的に生じる「ビート」現象であり、実装のバグではなく、周期系列でクロックがずれた2デバイスを固定ブロックでやり取りする限り原理的に不可避な現象と判断した（S7でジッタを0〜3ms加えても件数が半減するのみで解消しない）。
- 一方S5/S6が合格するのは、(a) S5はBi=128が目標1120の11%しかなく、ブロック1個分の瞬時ジャンプ自体が0.25×目標を超えない、(b) S6はBo=144が小さく出力（消費）が高頻度なため、ビートで生じた余剰がLagrange補間の2秒平滑化フィルタの帯域より十分速く消費され平滑値に現れない、という理由による。S1〜S4・S7はBi≈Bo≈targetの近傍（block/target比≈83%）で、上記いずれの保護機構も働かない。
- 比例制御のゲイン（1e-3、docs/spec.mdの式どおり）を強めても、ブロック到着直後の瞬時値そのもの（576→1056等）はその後の速度補正では遡って直せないため、Bi(またはBo) > 0.25×目標である限り条件c)を無違反で満たすことは原理的に不可能である（目標を十分大きくする＝ジッタ余裕を初期値から大きく取る、という選択はレイテンシ50ms予算と背反する。例えばS1で条件c)を満たすにはtarget≥4×Bi=1920サンプル=40msが必要）。

**閾値・実装のいずれも変更していない**（docs/plan.md 2.5節の式、spec.mdの目標充填量の式のとおりに実装した結果としてこの挙動になることを確認した）。この発見はManagerへの報告事項とし、次回の判断（目標充填量の式の見直し、条件c)の許容の見直し、またはBi=Bo近傍の構成を許容外とする、等）を仰ぐ。

### 計画からの変更点
- R4のテストで「破棄後の充填量が目標+1ブロック以下」を、`ResamplingFifo`の公開APIに瞬時充填量のgetterが無いため、直後の数回の`pull()`で追加の破棄が起きないことを確認する間接的な方法に置き換えた（`ResamplingFifoStats`は平滑値のみを公開する設計のため）。
- それ以外はdocs/plan.md 2.5節・3章T-003のとおりに実装した。

### 未解決事項
- 上記「条件c)不合格の原因分析」のとおり、S1・S2・S3・S4・S7がRingBufferLongで不合格のまま。Manager判断待ち。
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり）。

### 次回開始位置
- Managerの判断（条件c)またはターゲット充填量の式の扱い）を受けてから T-004（`src/core/PitchShifter.*`、`src/core/Engine.*`）に着手する。

### 実施内容
- `.github/workflows/build.yml`: dumpbin検査ループで`Test-Path`によるexe存在確認、`$LASTEXITCODE`確認、出力に"Image has the following dependencies"が含まれるかの確認を追加し、解析失敗で禁止DLL検査が素通りしないようにした。
- `tests/ShifterTests.cpp`: `bool allFinite = true;`の宣言直後の上書き（デッドストア）を解消し、`const bool allFinite = ...`の1行にまとめた。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`成功（1件のsmokeテストがPassed）。
- pwshがLinux環境に無いため、`.github/workflows/build.yml`の変更は目視レビューのみ（既存の`-match`配列判定パターンと整合させた）。

### 次回開始位置
- T-003: `src/core/ResamplingFifo.*`、`src/app/AudioIO.*`（入出力分離・リングバッファ・クロックずれ補正とオフライン模擬テスト）。Windows CIでの今回のdumpbin検査ロジックの実動作確認はMasterへのpush後にManagerが行う。

---

## 2026-09-28 T-002 CMake構成・依存固定・CI・README

### 実施内容
- `CMakeLists.txt`: `vc_core`をINTERFACEライブラリ化（D-005）。JUCE 9.0.2とsignalsmith-stretch 1.4.0をFetchContentでタグ固定（`GIT_SHALLOW`、stretchは`SYSTEM`）。`CMAKE_MSVC_RUNTIME_LIBRARY`をどのターゲットよりも先に設定し`/MT`相当にする。`VoiceChange`（GUIアプリ）と`VoiceChangeTests`（コンソールアプリ）を追加し、ctestにSmokeカテゴリを登録。
- `.gitignore`: build/等を除外。
- `.github/workflows/build.yml`: windows-latest（`cmake -A x64` → Release ビルド → ctest → vswhereでdumpbinを探しVCランタイム/UCRT DLL非依存を検査 → exeをartifact化）とubuntu-24.04（Ninja Releaseビルド → ctest）の2ジョブ。
- `src/app/Main.cpp`: 空の固定サイズウィンドウ（460×600、タイトル"VoiceChange"、背景`0xFF17181A`）。`moreThanOneInstanceAllowed() = false`（D-009）。UI本体はT-006で追加。
- `src/core/Params.h`: `Preset`/`Effect` enumと`kPresets`（docs/spec.md層2表どおり）のみ。`AtomicParams`等はT-004で追加する旨をコメントで明記。
- `tests/TestMain.cpp`: `juce::UnitTestRunner`ベースのランナー。`--category`必須、対象0件または失敗ありで非0終了。
- `tests/TestSignals.h`: 正弦生成・有限値検査の土台のみ。
- `tests/ShifterTests.cpp`: Smokeカテゴリ。48kHzで`configure(1, 960, 240)`のとき`inputLatency()+outputLatency()`が960×[0.95,1.05]であること、1秒の220Hz正弦を処理して出力が全て有限であることを確認。
- `README.md`: アプリ用に全面的に書き換え（目的・チェーン・動作要件・ビルド手順・技術選定理由・使わないもの・ライセンス・ドキュメント一覧）。開発フローはAGENTS.md参照の一文のみ残した。

### 結果
- ローカル（Linux/Ninja/Release）でクリーンビルドが成功（アプリ・テスト両方）。`ctest --test-dir build --output-on-failure`は1件（smoke）成功。
- `xvfb-run -a ./build/VoiceChange_artefacts/Release/VoiceChange`をtimeout 8秒で起動し、プロセスが生存したまま推移することを確認（クラッシュなし。タイムアウトによる強制終了のみ）。
- Linux開発環境に`libxi-dev`が未導入だったため追加インストールした（`juce_gui_basics`が`X11/extensions/XInput2.h`を要求する）。CIのlinuxジョブのapt installリストに`libxi-dev`を追加済み。
- Windows CIは未確認（Managerがpush後に確認）。

### 計画からの変更点
- なし。docs/plan.md 2.2〜2.4節のとおりに実装した。FetchContentのローカル上書き変数名（`FETCHCONTENT_SOURCE_DIR_JUCE`/`_SIGNALSMITH-STRETCH`/`_SIGNALSMITH-LINEAR`）はプロンプト指定のとおりで問題なく機能したため、plan.mdの更新は不要だった。

### 未解決事項
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり、実機はユーザーが確認）。
- Windows CIの初回実行結果はMasterへのpush後にManagerが確認する。

### 次回開始位置
- T-003: `src/core/ResamplingFifo.*`、`src/app/AudioIO.*`（入出力分離・リングバッファ・クロックずれ補正とオフライン模擬テスト）。

### コミット
- `2aa16a3` T-002: CMake構成・依存固定・Windows/Linux CI・README
- `df6b870` README: D-011（個人利用のみ、配布なし）の記載を追加
