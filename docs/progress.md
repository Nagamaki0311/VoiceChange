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

## 2026-09-28 T-002 レビュー指摘修正（CI dumpbin検査・デッドストア）

### 実施内容
- `.github/workflows/build.yml`: dumpbin検査ループで`Test-Path`によるexe存在確認、`$LASTEXITCODE`確認、出力に"Image has the following dependencies"が含まれるかの確認を追加し、解析失敗で禁止DLL検査が素通りしないようにした。
- `tests/ShifterTests.cpp`: `bool allFinite = true;`の宣言直後の上書き（デッドストア）を解消し、`const bool allFinite = ...`の1行にまとめた。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`成功（1件のsmokeテストがPassed）。
- pwshがLinux環境に無いため、`.github/workflows/build.yml`の変更は目視レビューのみ（既存の`-match`配列判定パターンと整合させた）。

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
