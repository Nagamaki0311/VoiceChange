# Code Review Graph 対応

project001はCode Review Graph（[tirth8205/code-review-graph](https://github.com/tirth8205/code-review-graph)、MIT License）をOptional Dependencyとして扱う。project001自体はCode Review Graphへの依存を持たず、実行環境にインストールされている場合のみDeveloper/Reviewerが活用する。

**混同注意**: Claude Codeのプラグイン一覧に表示されうる「Codegraph」（Colby McHenry作のClaude Codeプラグイン）は、本ドキュメントが対象とするpip配布CLI（tirth8205作）とは無関係の別プロダクトである。検出は`command -v code-review-graph`のみで判定するため、当該プラグインの導入有無はCapability Layerの検出結果に影響しない。

**現在Tier2（検出のみ）**: 動作未検証のまま複数セッションで`unavailable`が続いているため、`.claude/agents/developer.md`・`.claude/agents/reviewer.md`・REVIEW.mdへの振る舞い統合は見送っている（docs/capability-layer.md、D-021参照）。以下は検出・動作確認が取れた際にTier1へ昇格する場合の利用方針として残す。

## Code Review Graphとは

- `pip install code-review-graph`（または`pipx install`）で導入するCLIツール（実行ファイル: `code-review-graph`）。コードベースをグラフ化し、変更の影響範囲（呼び出し元・依存関係・関連テスト）を解析する。
- Python/JavaScript/TypeScript/Go/Rust/Java/C/C++/C#など30言語以上に対応。Git/SVNリポジトリ内での動作が前提。
- `code-review-graph serve`でMCPサーバーとしても動作するが、project001ではCLI直接呼び出しのみを統合する（下記「疎結合設計」参照）。

project001はCode Review Graph本体を組み込まない。バージョン更新の影響を受けないよう、検出とフォールバックのみを実装する。

## 検出方法

Developer/Reviewerは、影響範囲の確認が必要な場面で以下により利用可否を確認する。

```bash
command -v code-review-graph >/dev/null 2>&1
```

- コマンドが存在する場合: 利用可能。以下のコマンドを優先利用する。
- 存在しない場合: 利用不可と判断し、即座にRead/Grep/Bashによる通常の調査（呼び出し元の`grep`検索等）へフォールバックする。インストールを促したり、エラーで停止したりしない。

## 対応コマンドとマッピング

要求される機能（Graph Build / Incremental Update / Review Delta / Blast Radius Analysis / Impact Analysis）は、確認済みのCLIサブコマンドで以下のように実現する。`detect-changes --brief`は変更検出（変更された関数・リスクスコア・テストギャップの要約）を返すのに対し、`impact --files <変更ファイル>`は影響範囲そのもの（呼び出し元・影響ファイルを含む具体的なノード列挙）を返す、役割の異なるコマンドである（`--help`上もそれぞれ"Analyze change impact against the existing graph (read-only)"、"Analyze the blast radius of changes"と説明が分かれている）。

| 要求された機能 | 対応コマンド | 用途 |
|---|---|---|
| Graph Build | `code-review-graph build` | コードベース全体を解析しグラフを構築する（初回、または未構築時） |
| Incremental Update | `code-review-graph update` | 変更ファイルのみをグラフへ反映する（2回目以降、差分のみ） |
| Review Delta | `code-review-graph detect-changes --brief` | 変更ファイルの変更関数・リスクスコア・テストギャップを検出する（呼び出し元・影響ファイルは含まない） |
| Blast Radius Analysis / Impact Analysis | `code-review-graph impact --files <変更ファイル>` | 変更ファイルの呼び出し元・影響ファイル（`impacted_nodes`/`impacted_files`を含むJSON）を具体的に列挙する |

**PR Reviewについて**: `/code-review-graph:review-pr`はエディタ統合用のスラッシュコマンドであり、CLIから直接呼び出せる確証が得られなかった（人間向けUIコマンドである可能性が高く、`.claude/settings.json`の`/hooks`と同種の制約が疑われる）。project001では統合せず、代わりに`impact --files <変更ファイル>`で得られる影響範囲情報をReviewerが手動で解釈する運用とする。GitHub Actions連携（`tirth8205/code-review-graph`公式アクション）によるPRへの自動コメントは、project001自体にCIがないため対象外とし、個別アプリのリポジトリ側で必要になった場合の選択肢として記録するに留める。

## 使用フロー

1. **Developer**: 実装前後に、変更対象ファイルについて`code-review-graph impact --files <変更ファイル>`（未構築なら先に`build`）を実行し、想定外の呼び出し元・依存箇所がないか確認する。変更検出のサマリのみでよい場合は`detect-changes --brief`も併用してよい。利用不可ならgrepで呼び出し元を検索する通常のフローに切り替える。
2. **Reviewer**: レビュー時に同様のコマンドで影響範囲を確認し、REVIEW.mdの「回帰」「依存関係」観点の検証に用いる。Developerが既に取得した結果を再利用してよい（同一クエリの重複実行を避ける）。

## 疎結合設計

- project001のリポジトリにCode Review Graphへの依存記述（pyproject.toml等）を追加しない。
- MCPサーバーモード（`code-review-graph serve`、30種のMCPツール）は統合しない。デーモンプロセスの起動・停止管理や`.mcp.json`への登録という新しい運用が必要になり、CLI直接呼び出しに比べて疎結合性が下がるため。将来的にMCPモードの明確な必要性が生じた場合に改めて検討する。
- Code Review Graph固有のコマンド・仕様の詳細はこのファイルにのみ記載する（developer.md/reviewer.md等には利用方針のみを書き、複製しない）。
- 検出・利用の規約自体はdocs/capability-layer.mdに従う。

## 関連

- 検出規約（Capability Layer）: docs/capability-layer.md
- レビュー観点への反映: REVIEW.md
- Agent構成: docs/agents.md
