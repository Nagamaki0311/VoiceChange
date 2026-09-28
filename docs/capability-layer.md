# Capability Layer

project001における「利用可能な外部ツールを自動検出し、あれば優先利用・なければ既存フローへフォールバックする」という共通規約を定義する。

## 位置づけ

Capability Layerの本体は、以下の**Agentへの指示規約**である。`.claude/bootstrap.sh`はこの規約自体を置き換えるものではなく、規約が定める検出手順（手順1）を1箇所にまとめて事前実行するキャッシュ最適化の実装に過ぎない。project001はアプリケーションランタイムを持たないテンプレートであり、検出ロジックをコードとして実装すること自体が正当化できない新規依存にならないよう、`bootstrap.sh`は`command -v`のみに依存し新規の外部依存を一切持たない（AGENTS.mdの判定ラダー3〜5段目を満たす）。

1. 該当ツールの検出コマンドを`command -v <ツール名>`（または同等の軽量チェック）で実行する
2. 利用可能なら、そのツールが提供する機能を優先利用する
3. 利用不可、またはコマンドが失敗した場合は、即座に既存フロー（Read/Grep/Bash等の標準ツール）へフォールバックする。インストールを促したり、タスクを止めたりしない
4. ツール固有の仕様・コマンドは、そのツール専用のdocsファイル（例: docs/agent-reach.md）にのみ記載し、複製しない

当初はこの検出手順（手順1）を`.claude/settings.json`のSessionStart Hookコマンド文字列に直接列挙していたが、検出対象が増えるにつれ1行コマンドが線形に長大化し可読性が落ちる問題があった。判定ラダー6段目（1行で書けるか）を再評価した結果、「1行では収まらない」という結論に至ったため、`.claude/bootstrap.sh`という専用スクリプトへ切り出した（project001テンプレートのD-013で`subagentStatusLine`用のスクリプトファイルを新設した際と同じ判断基準）。検出対象リストはスクリプト内1箇所にまとめており、追加は1行で完結する。

## キャッシュ効率: bootstrap.sh経由の事前検出

利用可否はセッション中に変化しないため、`.claude/settings.json`のSessionStart Hookが`bash "$CLAUDE_PROJECT_DIR/.claude/bootstrap.sh" --check`を一度だけ実行し、結果をManagerのコンテキストに表示する（`.claude/settings.json`参照）。Managerは、Planner/Researcher/Developer/Reviewerを起動する際にこの結果をタスク説明に含めてよい（各Agentが同じ検出コマンドを繰り返し実行する無駄を避ける）。Plannerは自身でBashを実行できないため、この共有が実質的な唯一の入手手段になる。

ただし、Hookが発火しない実行環境が存在すること（docs/agents.md「運用上の注意」参照）から、**各Agent自身も`command -v`による自己検出を維持する**。Hookのキャッシュはあくまで最適化であり、それに依存しないと機能しない設計にはしない。

## Capability一覧（3階層）

### Tier1: 統合済み（Agentの振る舞いに組込み）

現在Tier1に該当するCapabilityはない。Agent-Reach/Code Review Graph/Context7/GitHub CLIの4件は、2026年公式仕様監査（T-020〜）で、動作未検証のまま複数セッションにわたり一度も検出されなかった実績が確認されたため、Tier2へ格下げした（project001テンプレートのD-021参照）。実際に検出・動作確認が取れた時点で、下記「新しいCapabilityを追加する手順」に沿って再度Tier1へ昇格させる。

### Tier2: 検出のみ（Agentの振る舞いには未組込み）

`.claude/bootstrap.sh`の検出対象には含めるが、Agent定義ファイル（`.claude/agents/*.md`）・REVIEW.mdへの振る舞い統合は行わない。

| Capability | 検出コマンド | 検出のみに留める理由 |
|---|---|---|
| Agent-Reach | `command -v agent-reach`（詳細はdocs/agent-reach.md参照） | 動作未検証のまま複数セッションで`unavailable`が続いており、Tier1（Researcherの振る舞いへの統合）は実際の検出実績が出るまで見送る（project001テンプレートのD-021） |
| Code Review Graph | `command -v code-review-graph`（詳細はdocs/code-review-graph.md参照） | 同上（Developer/Reviewerへの統合を見送る） |
| Context7 | `command -v ctx7`（詳細はdocs/context7.md参照） | 同上（Planner/Developer/Reviewer/Researcherへの統合を見送る）。サブコマンド仕様も本環境で未検証のまま |
| GitHub CLI (`gh`) | `command -v gh` | 同上（Researcherへの統合を見送る） |
| Node.js | `command -v node` | Context7 CLI（`ctx7`）の実行にNode.js 18+が前提となるため、関連ツールとして検出対象に含めている |
| Python | `command -v python3` | Agent-Reach・Code Review GraphがいずれもpipでCLIを配布しているため、関連ツールとして検出対象に含めている |
| Playwright | `command -v playwright` | project001自体にブラウザ操作対象のUIコードが存在しない。個別アプリのリポジトリでUIが実装された時点で、そちらでTier1へ昇格させる出発点として検出のみ用意している |

### 対象外（検出対象にしない）

| 項目 | 対象外とする理由 |
|---|---|
| MCP Server | 個別ツールではなくClaude Code本体の接続機構であり、`command -v`で検出する対象と同列ではない。加えてproject001のsubagent（`.claude/agents/*.md`）は`tools`フロントマターがallowlist方式であり、現状Planner/Developer/Reviewer/ResearcherからMCPサーバーのツールを呼び出せない。追加する場合は、対象subagentの`tools`にMCPツール名を追記した上で、当該Agent定義ファイルとdocs/capability-layer.mdの両方を更新する手順が必要になる |
| Claude Design `/design-sync` | Claude Code本体のスラッシュコマンドであり`command -v`で検出する対象ではない。project001はテンプレート元リポジトリのため、特定アプリの永続的なデザインシステムをdesign-syncで同期する対象を持たない（詳細はdocs/design-workflow.md参照） |
| Ponytail | AGENTS.mdの設計原則1〜8として本文へ統合済み（project001テンプレートのD-003、project001テンプレートのD-014）であり、`command -v`で検出する外部コマンドではないため検出対象にしない |
| Claude Codeプラグイン全般 | project scopeでは既定で有効化しない。プラグインはユーザー個人のインストール状況に依存し、project001をコピーした派生プロジェクトすべてに強制されるべきものではないため。派生プロジェクトで有効化する場合は、`.claude/settings.json`にプラグインmarketplace/インストール設定を追加し（`enabledPlugins`等、Claude Code公式ドキュメントのキー構成に従う）、user scope（個人環境全体）ではなくproject scope（このリポジトリ配下のみ）に限定した上で、docs/capability-layer.mdに追加したプラグイン名と用途を記録する。**例外**: Frontend Design（`frontend-design@claude-plugins-official`）はproject001テンプレートのD-025によりproject scopeで有効化済み（`.claude/settings.json`の`enabledPlugins`）。詳細はdocs/design-workflow.md参照 |

### 派生プロジェクトでの推奨プラグイン（project001テンプレートのD-030）

project001自体はコードを持たないため有効化しない。派生プロジェクトで言語・要件が決まった時点で、上記の手順（project scopeに限定し本ファイルに記録）に従って判断する。

| プラグイン | 推奨条件 | 理由・注意点 |
|---|---|---|
| Code intelligence（`typescript-lsp@claude-plugins-official`、`pyright-lsp@claude-plugins-official`等） | 型付き言語を使う場合は有効化を推奨 | 公式のトークン削減策の1つ。記号単位の定義参照がgrep+複数ファイル読込を置き換え、編集直後に型エラーを検出する。言語サーバー本体（例: `npm install -g typescript-language-server typescript`）の別途導入が必要。クラウドセッション（claude.ai/code）では言語サーバーが起動せず効果がない |
| security-guidance（`security-guidance@claude-plugins-official`） | 既定では有効化しない。認証・決済・個人情報を扱うアプリで、コスト増を許容できる場合のみ検討する | ターンの終了時（Stop Hook）とcommit時に別途LLMレビューを実行するため、ターンごとに追加のAPI消費が発生する。Reviewerの敵対的検証（REVIEW.md「セキュリティ」観点）と役割が重なる |

Tier2からTier1への昇格条件は2種類ある。Node.js/Python/Playwrightは、個別アプリのリポジトリ（project001から作成された別リポジトリ）で実際に使うタスクが発生した時点。Agent-Reach/Code Review Graph/Context7/GitHub CLIは、実行環境での検出・動作確認が取れた時点（project001テンプレートのD-021）。いずれも下記「新しいCapabilityを追加する手順」に沿ってTier1へ昇格させる。

## 新しいCapabilityを追加する手順

1. `.claude/bootstrap.sh`の`CAPABILITIES`に1行追加する
2. `docs/<tool>.md`を新設し、ツールの正体・検出方法・フォールバック方針・疎結合設計を記載する
3. 利用するAgent（`.claude/agents/*.md`）に、優先利用する旨の参照1行を追加する
4. 本ファイルのTier表に1行追加する
5. `docs/decisions.md`に記録する

## アーキテクチャ

現在Tier1が空のため、以下の図はTier1昇格後に適用される検出・フォールバックの流れを示す（Tier2のままではAgent定義ファイルに振る舞いとして組み込まれない）。

```
Agent起動時（Researcher/Developer/Reviewer）
        │
        ▼
   command -v <tool>?
   ├─ Yes → <tool> doctor/version等で健全性を確認
   │         ├─ 成功 → 対応機能を優先利用
   │         └─ 失敗 → フォールバック
   └─ No  → フォールバック（Read/Grep/Bash/WebFetch等の標準ツール）
        │
        ▼
   結果を要約してManagerへ返す（生の出力はコンテキストに残さない）
```

`.claude/bootstrap.sh --check`での事前検出も同じ`command -v`判定を使うため、Agent起動時の自己検出と矛盾しない（同じ規約の二重適用であり、結果は常に一致する）。

## 関連

- Agent-Reach: docs/agent-reach.md
- Code Review Graph: docs/code-review-graph.md
- Context7: docs/context7.md
- 調査ワークフロー: docs/research-workflow.md
- レビュー方針: REVIEW.md
