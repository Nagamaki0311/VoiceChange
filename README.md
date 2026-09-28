# VoiceChange

Windows向けリアルタイムボイスチェンジャー（単一の.exe）。物理マイク・SteelSeries Sonar等の仮想入力デバイスの音声を加工し、VB-CABLE経由でDiscordやOBSへ渡す。

想定するチェーン:

```
物理マイク → (Sonar等の前段処理) → VoiceChange → VB-CABLE (CABLE Input) → Discord / OBS
```

詳細な仕様はdocs/spec.md、UIはdocs/design.md、実装計画はdocs/plan.md、設計判断の経緯はdocs/decisions.mdを参照する。開発フロー（Manager/Planner/Developer/Reviewer等の役割分担）はAGENTS.mdを参照する。

## 動作要件

- Windows 10 / 11（64bit）。
- [VB-CABLE](https://vb-audio.com/Cable/)を別途インストールしておくこと（本アプリには同梱しない）。未インストールの場合、起動時に検出できなかった旨のダイアログを表示する。
- 入力元は物理マイクに限らず、SteelSeries Sonar等の仮想入力デバイスも区別なく選べる。

## 開発中の機能

現時点（T-002完了時点）では、CMake構成・依存関係の固定・CI・空のウィンドウのみが実装済みで、音声処理チェーンは未実装である。今後の実装状況はdocs/tasks.md、作業履歴はdocs/progress.mdを参照する。

## ビルド手順

### Windows（配布用）

Visual Studio 2022（Desktop development with C++）とCMake 3.25以上が必要。

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成物は`build/VoiceChange_artefacts/Release/VoiceChange.exe`。MSVCランタイムは静的リンク（`/MT`）するため、VC++ 再頒布可能パッケージのインストールなしで動作する（CIの`dumpbin /dependents`検査で確認している）。

### Linux（開発用、配布対象外）

開発はLinux上で行い、DSPコア（`vc_core`、GUI・デバイス非依存のINTERFACEライブラリ）とテストの検証、UIのXvfb上でのスクリーンショット確認に使う。ALSA・X11系・freetype・fontconfigの開発パッケージが必要（`.github/workflows/build.yml`のlinuxジョブ参照）。

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

依存ソース（JUCE / signalsmith-stretch / signalsmith-linear）はCMakeの`FetchContent`で取得する。ネットワークなしでローカルの取得済みソースを使う場合は次のように指定する。

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_JUCE=<JUCEのパス> \
  -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH-STRETCH=<stretchのパス> \
  -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH-LINEAR=<linearのパス>
```

## 技術スタック

| 項目 | 採用 |
|---|---|
| 言語 | C++20 |
| フレームワーク | JUCE 9.0.2（CMake、FetchContentでタグ固定） |
| ピッチ/フォルマント | Signalsmith Stretch 1.4.0（依存: signalsmith-linear 0.6.4） |
| ビルド | MSVC、`/MT`（GitHub Actions windows-latest） |

### 選定理由

- JUCEは`IAudioClient3`による「WASAPI共有低遅延モード」に対応しており、他アプリと干渉しない共有モードのまま低遅延を実現できる。
- GC（ガベージコレクション）のない言語（C++）で音声スレッドを完全に制御できる。
- DSP部品・GUI・デバイス管理が単一フレームワーク（JUCE）で揃い、依存が少ない。
- Signalsmith Stretchは高品質なピッチシフトとフォルマント操作をMITライセンスで提供する。

### 使わないもの

- **Rubber Band**: GPLライセンスのため、AGPLv3/商用デュアルのJUCEと組み合わせても配布条件が複雑になる。MITのSignalsmith Stretchで要件（移調・フォルマント操作）を満たせるため採用しない。
- **WASAPI排他モード**: 排他モードで開いたデバイスは他アプリが同時に使えなくなる。本アプリはSonar等の前段処理やDiscord/OBSと並行して動く前提のため、共有モード（低遅延モード優先、フォールバックで通常モード）のみを使う。

## ライセンス

- JUCE 9はAGPLv3と商用ライセンスのデュアルライセンス。本アプリの.exeを配布する場合、ソースコード全体をAGPLv3で公開するか、JUCEの商用ライセンスを取得する必要がある。
- Signalsmith Stretch / signalsmith-linearはMITライセンス。

## ドキュメント

- docs/spec.md: 仕様書
- docs/design.md: UIデザイン仕様
- docs/plan.md: 実装計画
- docs/decisions.md: 設計判断の記録（ADR）

開発フロー・設計原則はAGENTS.mdを参照する。
