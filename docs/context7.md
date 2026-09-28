# Context7 対応

project001はContext7（[upstash/context7](https://github.com/upstash/context7)、MIT License）をOptional Dependencyとして扱う。project001自体はContext7への依存を持たず、実行環境にインストールされている場合のみPlanner/Developer/Reviewer/Researcherが活用する。

**現在Tier2（検出のみ）**: 動作未検証（サブコマンド仕様も含む）のまま複数セッションで`unavailable`が続いているため、`.claude/agents/*.md`への振る舞い統合は見送っている（docs/capability-layer.md、D-021参照）。以下は検出・動作確認が取れた際にTier1へ昇格する場合の利用方針として残す。

## Context7とは

- ライブラリの最新の公式ドキュメント・型定義をLLMのコンテキストへ取得するツール。学習データの古さやAPIのバージョン違いによる誤った実装・レビューを防ぐことを目的とする。
- 本来はMCPサーバー（`https://mcp.context7.com/mcp`）として配布されているが、project001が検出するのはCLI版（コマンド名: `ctx7`。前提: Node.js 18+。upstash/context7公式READMEに基づく。本環境では未検証）である。

project001はContext7本体を組み込まない。バージョン更新の影響を受けないよう、検出とフォールバックのみを実装する。

## 検出方法

Developer/Reviewer/Researcherは、外部ライブラリのAPI仕様を確認する必要がある場面で以下により利用可否を確認する。

```bash
command -v ctx7 >/dev/null 2>&1
```

- コマンドが存在する場合: 利用可能。ライブラリ名・確認したい内容を渡して問い合わせる（サブコマンド仕様の例: `ctx7 library <name> <query>`。upstash/context7公式READMEに基づく。本環境では未検証のため、断定的な仕様として他ドキュメントへ複製しない）。
- 存在しない場合: 利用不可と判断し、即座にWebFetch/WebSearchへフォールバックする。インストールを促したり、エラーで停止したりしない。

Plannerは`tools`フロントマターにBashを持たず自身で`command -v`を実行できないため、上記の自己検出は行わない。代わりにManagerが共有するSessionStart Hook（`.claude/bootstrap.sh --check`）の検出結果を使う（docs/capability-layer.md参照）。Managerからタスク説明でContext7が利用可能と共有されていれば優先利用し、共有がない・利用不可と共有された場合はWebFetch/WebSearchへフォールバックする。

## 利用方針

- Planner: 実装計画に外部ライブラリのAPI仕様の確認が必要な場合に優先利用する。
- Developer: 実装対象のライブラリの最新API・型定義を確認する際に優先利用する。
- Reviewer: 指摘の裏付け（実際のAPI仕様と実装の食い違いの確認等）に優先利用する。
- Researcher: 外部調査でライブラリ固有の技術情報を確認する際に優先利用する。

## フォールバック方針

`ctx7`の呼び出しが失敗した場合も、その場でWebFetch/WebSearchに切り替え、タスク全体を止めない（一般原則はdocs/capability-layer.md参照）。

## MCP版を統合しない理由

Context7はMCPサーバー（`https://mcp.context7.com/mcp`）としての利用が公式に案内されているが、project001ではMCP版を統合しない。理由は2点。

1. project001のsubagent（`.claude/agents/*.md`）はいずれも`tools`フロントマターがallowlist方式であり、MCPサーバーのツールをフロントマターに列挙できない（現状の構成では呼び出せない）。
2. MCP版を使うにはAPIキーの発行・管理という新しい秘密情報の運用が発生し、project001が重視する疎結合設計（依存を追加しない、いつでも取り除ける）に反する。

CLI版（`ctx7`）はローカルの実行可否を`command -v`のみで判定でき、上記いずれの制約も受けないため、こちらのみを検出対象とする。

## 疎結合設計（アーキテクチャ）

```
Planner/Developer/Reviewer/Researcher
        │
   command -v ctx7?
   ├─ Yes → ctx7でライブラリドキュメント・型定義を確認
   │         └─ 失敗 → WebFetch/WebSearchへフォールバック
   └─ No  → WebFetch/WebSearchへフォールバック
```

- project001のリポジトリにContext7への依存記述（package.json等）を追加しない。
- Context7固有のコマンド例・仕様の詳細はこのファイルにのみ記載する（各Agent定義ファイルには検出コマンドの呼び出し方針のみを書き、複製しない）。

## 関連

- 検出規約（Capability Layer）: docs/capability-layer.md
- Agent構成: docs/agents.md
