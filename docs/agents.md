# Agent構成とモデル・Hook詳細

Agent構成、モデル構成（Model Routing）、オーケストレーションの技術的根拠、Hook構成を記録する。開発方針・設計原則はAGENTS.mdを、レビュー方針はREVIEW.mdを、Agent-Reach対応の詳細はdocs/agent-reach.md・docs/research-workflow.mdを、デザインワークフローの詳細はdocs/design-workflow.mdを、Capability Layer/Code Review Graphの詳細はdocs/capability-layer.md・docs/code-review-graph.mdを、進捗の可視化の詳細はdocs/status-line.mdを参照（本ファイルでは扱わない）。AGENTS.md/CLAUDE.mdから参照される。

## Agent構成

| Agent | 定義ファイル | 役割 | 起動元 |
|-------|-------------|------|--------|
| Manager | なし（このセッション自身の役割。CLAUDE.md参照） | タスク管理・Agent割当・優先順位判断・レビュー依頼・完了判定 | User |
| Planner | `.claude/agents/planner.md` | 現状分析・実装計画の作成 | Manager |
| Designer | `.claude/agents/designer.md` | UI/UXデザイン設計（デザインワークフロー対応） | Manager（Plannerが必要と判断した場合のみ） |
| Developer | `.claude/agents/developer.md` | 実装・バグ修正 | Manager |
| Reviewer | `.claude/agents/reviewer.md` | コードレビュー・敵対的検証（REVIEW.md参照） | Manager |
| Researcher | `.claude/agents/researcher.md` | 外部情報収集・重複除去・信頼性評価（Agent-Reach対応） | Manager（Plannerが必要と判断した場合のみ） |

## モデル構成（Model Routing）

各Agentの`model`は`inherit`（Managerのセッションモデルに追従）を使わず、役割ごとに固定している。`inherit`のままだと、Managerが使っているチャットモデル次第でPlanner/Developer/Reviewerの品質・コストが意図せず変動してしまい、Model Routingの前提（役割に応じた予測可能な品質・コスト）が崩れるため。`effort`（推論にかける労力。未指定ならセッション設定を継承する）にも同じ理由が当てはまるため、役割ごとの推論の深さが一定なAgentでは固定している。

| Agent | モデル | effort | 選定理由 |
|-------|--------|--------|----------|
| Manager | セッションの既定モデル（上書きしない） | 未指定 | ルートセッション＝Userが選んだ対話モデルそのもの。判断・Agent割当・状態管理という業務内容自体は既存の推論力で十分であり、`.claude/settings.json`でproject001が対話モデルを強制すると、このリポジトリでの作業全体（Manager業務に限らない）に影響してしまうため上書きしない |
| Planner | `opus` | `high` | 設計判断の質は下流（Developer/Reviewer）の手戻り量に直結し、起動頻度は低い（タスクごとに1回程度）。品質を安易に落とさない方針（Definition of Done参照）に従い、最も推論力の高いモデルを固定する |
| Designer | `opus` | `high` | デザイン判断の質が実装・レビューの手戻り量に直結する点でPlannerと同じ理由。「AIっぽいUI」の回避は表面的なルール適用ではなく文脈判断が要るため、最も推論力の高いモデルを固定する |
| Developer | `sonnet` | 未指定 | Anthropicのコーディング用途の標準モデルであり、既存コード規約に沿った実装・根本原因修正に十分な理解力を持つ。Opus常用ほどの必要性はなく、Haikuでは複雑な既存コードの理解漏れリスクがあるため中間の`sonnet`を固定する。effortを固定しないのは、担当タスクが軽微な修正から複雑なデバッグまで幅広く、単一の値が適さないため。必要に応じてManagerが起動時に指定する |
| Reviewer | `sonnet`（既定） | `high` | project001は不特定多数の個別アプリのテンプレートであり、そこにコピーされた先ではセキュリティ・正確性に関わるコードレビューが行われる。テンプレートの既定は下げすぎない。REVIEW.mdの敵対的検証は仮説の列挙と反証の積み重ねで成立するため、effortは高く固定する |
| Researcher | `sonnet` | `medium` | 複数チャネルの検索結果の重複除去・信頼性評価には一定の推論力が要るが、Plannerほどの開放的な設計判断は不要。検索・要約が中心の作業内容とコストのバランスを取る。成果の質は推論の深さより情報源の広さに依存するため、effortは中程度に留める |

**環境変数による上書きに注意**: `CLAUDE_CODE_SUBAGENT_MODEL`環境変数が設定されている実行環境では、この表のAgent別`model`フロントマターより優先され、全Agentが同一モデルで起動する。project001自体はこの変数を設定しないが、ユーザー環境側の設定はproject001から検知・警告できないため、Model Routingが意図通り機能しない場合はまずこの変数の有無を確認する。

### 軽量レビューの扱い（新規Agentを追加しない理由）

Markdown/README/docsの体裁確認など、コード品質やセキュリティに関わらない軽量なレビューについては、専用の軽量Agentを新設せず、Managerがreviewerを起動する際にAgent呼び出しの`model`パラメータで`haiku`等へ一時的に上書きする運用とする。Claude Code側に「呼び出し時のモデル上書き」という既存機能があるため、これに専用Agentファイルを追加するのはPonytailの判定ラダー（3: 標準ライブラリ/既存機能で足りるか）に反する。reviewer.md自体は変更せず、既定(`sonnet`)のまま維持する。

## オーケストレーションルール

Manager起動の制約・開発フロー・修正ループはAGENTS.mdの「ワークフロー」を参照（重複記載しない）。ここでは技術的な補足のみを記す。

- 各Agentはタスク終了後、変更点と結果を要約してManagerに返す。詳細な作業ログはdocs/progress.mdに記録し、会話コンテキストには要約のみ残す。

### Hookとの接続（なぜManagerを独立subagentにしないか、その2つ目の根拠）

SessionStart/PreCompactのHook出力は、常にManager（ルートセッション）のコンテキストにのみ注入される。Planner/Developer/Reviewerはsubagentとして隔離されたコンテキストで動くため、これらのHook出力を直接受け取ることはない。したがって

- SessionStart Hook → Manager（タスク状況を把握してから Planner を起動）
- PreCompact Hook → Manager（サイクルの任意の時点で発火。docsへの記録を判断・実行してから圧縮を許容する）

という流れは、Managerをsubagent化しない限りにおいて技術的に自然に成立する。これはManagerを独立subagentにしなかった判断（公式推奨のオーケストレーション構成。project001テンプレートでの決定）に加え、Hookとの統合という観点からも同じ結論を補強する。
なお、PreCompact Hookは「Reviewerの後」等の固定ステップではなく、コンテキストサイズに応じて任意のタイミングで発火するイベントである。開発フローの図はあくまで論理的な作業順序を示すものであり、PreCompactは並行して発火し得る点に注意する。

## 採用しなかったAgent

- **research（過去に不採用、現在はResearcherとして採用）**: project001テンプレートのD-004時点では「Plannerが調査を兼ねており役割が重複する」として不採用としていた。Agent-Reach対応（project001テンプレートのD-009）により、GitHub/Web/RSS/YouTube/Reddit/X等の複数チャネルを横断する調査量・重複除去・信頼性評価という、Plannerの設計判断とは異なる作業が明確になったため判断を見直し、Researcherとして採用した。Plannerは「何をどのチャネルで調べるか」を決める（軽微な確認のためのWebFetch/WebSearchは残す）、Researcherは「実際に集めて評価する」に役割を分離しており、重複しない。
- **UI（過去に不採用、現在はDesignerとして採用）**: project001自体はアプリケーションコードを持たないテンプレートでありUIレビューの対象が存在しないため、当初は個別アプリのリポジトリ側で追加する方針としていた。Design基盤統合（project001テンプレートのD-025）でユーザーが方針の見直しを選択し、project001自体にDesigner Agentを常設する構成へ変更した。project001自身の開発でDesignerが起動することは通常ないが、個別アプリ側でテンプレートをそのまま使えるよう、常設のAgent定義として保持する。

## コード品質ルール

AGENTS.mdの「設計原則」を参照（判定ラダー、後方互換性を維持しない方針、根本原因修正、`ponytail:`コメント運用など）。本ファイルでは重複記載しない。

## Hook構成（`.claude/settings.json`）

セッション継続性・ドキュメント品質を技術的に担保するため、5種のHookを導入している。いずれも新規の外部依存関係は追加していない。Capability Layerの検出手順（`command -v`）は当初SessionStart Hookのコマンド文字列に直接列挙していたが、検出対象が増えて1行コマンドが長大化したため`.claude/bootstrap.sh`へ切り出した（判定ラダー6段目の再評価。経緯はdocs/capability-layer.md参照）。

### SessionStart

セッション開始時に3つを自動表示し、「docs/progress.md・docs/tasks.mdを確認する」運用ルール（CLAUDE.md参照）を手動確認任せにせず技術的に補助する。

- `.claude/bootstrap.sh --check`によるCapability Layerの検出結果
- `docs/tasks.md`の未完了タスク（状態列が`完了`の行を除外）
- `docs/progress.md`の最新エントリ

検出対象の追加・変更は`.claude/bootstrap.sh`の1箇所で完結し、`.claude/settings.json`側の変更は不要（`bootstrap.sh`が存在しなくても`2>/dev/null`によりHook全体は失敗しない）。完了タスクの除外は、積み重なるほど表示コスト（トークン消費）が線形に増えるのを防ぐため。`awk -F'|'`で行を列分割し状態列（5列目）を判定しており、備考欄に含まれる「完了」という文字列には影響されない。`docs/tasks.md`のタスク一覧表の列構成（`| ID | タスク | 優先度 | 状態 | 担当エージェント | 備考 |`の6列）を前提とする。Capability検出の規約自体はdocs/capability-layer.mdを参照（本ファイルでは重複記載しない）。

### PreCompact

コンテキスト圧縮の直前に、docs/progress.md・docs/tasks.mdへの記録を促すリマインダーを表示する。長時間セッションで未記録の作業がコンテキスト圧縮により失われることを防ぐ。

SessionStart/PreCompactは、コード（アプリケーションロジック）を持たないproject001に合わせ、状態の要約表示・記録の呼びかけに留めている。

### PostToolUse

`Write`/`Edit`で`.md`ファイルへの書き込み直後に`.claude/hooks/ja-style-check.py`を実行する。

- 検査対象: `.claude/ja-style-rules.json`のNGパターン（「効く」の一語で済ませる曖昧な効果表現、依頼されていない対比構文）、文末表現（ます・です等）の3文以上連続
- 該当時: `exit code 2`でエージェント自身へ差し戻す。単語だけの置き換えは禁止し、該当文全体の書き直しを求める（グッドパターンの例も併記する）
- ルールは`.claude/ja-style-rules.json`への追記のみで拡張できる（project001テンプレートのD-017参照）

project001自体にアプリケーションコードは無いが、AIが生成するdocs/配下のドキュメントという明確な対象がある点でlint/ビルド連携用のPostToolUse Hookとは事情が異なる。lintやビルド連携等、対象コードが存在しないPostToolUse Hookは引き続き導入していない（個別アプリのリポジトリ側で必要になった場合はそちらで追加する）。

### SubagentStop

サブエージェント完了時に`.claude/hooks/subagent-doc-check.py`を実行する。

- `agent_type`が`developer`の場合のみ、`git status --porcelain -- docs/progress.md`でdocs/progress.mdに未コミットの変更があるかを確認し、無ければ`exit code 2`で「完了報告の前にdocs/progress.mdへ記録すること」を差し戻す
- 他のAgentタイプ（Planner/Reviewer/Researcher）はmatcherを絞らずスクリプト内で`agent_type`を判定し、該当しなければ無反応（`exit code 0`）

「docs/progress.md: 作業履歴、次回開始位置（Developerが記録）」という運用ルール（本ファイル冒頭の参照ドキュメント表）が、指示文だけでなく機械的にも守られるようにする（project001テンプレートのD-018参照）。

### SessionEnd

セッション終了時に、リポジトリ全体で未コミットの変更（`git status --porcelain`）があるかを確認し、あれば「docs/progress.md・docs/tasks.mdへの記録漏れがないか確認しcommit/pushしてください」とリマインドする。未コミットの変更が無ければ無反応。PreCompactは圧縮が発生した場合のみ発火するため、圧縮を経ずにセッションが終了するケース（`/clear`等）を補完する（project001テンプレートのD-018参照）。

### Stop Hookを不採用とした理由

docs更新のタイミングとしては頻度が高すぎ、毎ターンのリマインダーはトークンの無駄になるため不採用とした。公式が示す「確定的なレビューゲート」としてのStop Hookも検討したが、pass/failを返すスクリプトが前提であり、アプリケーションコードを持たないproject001には機械判定可能なチェックが存在せず、REVIEW.mdの敵対的検証はLLMの判断でありスクリプト化できず、8回連続ブロックでClaude Codeが自動オーバーライドするため「確定的」な保証にもならない。ただし個別アプリのリポジトリではテスト・ビルド等の機械判定可能なチェックが存在するため、そちら側でStop Hookによる確定的ゲートを検討してよい。

### 運用上の注意（環境依存）

- `.claude/settings.json`が存在しない状態で開始したセッションでは、設定の再読み込みに `/hooks` を開くかセッション再起動が必要になる場合がある（Claude Code側の既知の挙動）。
- Claude Code on the web／リモート実行環境では `/hooks` コマンド自体が提供されていないことがある。これはUIコマンドの制約であり、Hook実行機構（`.claude/settings.json`の`hooks`設定）自体はリモート環境でも動作する（`$CLAUDE_CODE_REMOTE`環境変数がHook内で参照可能であることからも裏付けられる）。新規セッション（コンテナの新規起動）では自動的に有効化される。
- Hookが発火しているか不明な場合は、SessionStart Hookのコマンドをターミナルで直接実行し、出力が想定通りか確認する。

## Status Line構成（`.claude/settings.json`）

`subagentStatusLine`により、サブエージェント実行時のみエージェントパネルに進捗を日本語で表示する（`.claude/statusline-subagent.sh`、新規依存なし）。Hookとは異なり会話コンテキストへは注入されないため、LLMトークンを消費しない。stdinスキーマ・出力形式・実装時に発見した問題（マルチバイト文字のtruncationバグ等）の詳細はdocs/status-line.mdを参照（本ファイルでは重複記載しない）。

## Auto Memoryとの役割分担

Claude Codeには、Claude自身が訂正・学習をリポジトリごとの`MEMORY.md`へ自動で記録するAuto Memory機能があり、毎セッション自動でロードされる（project001が明示的に組み込む設定ではなく、Claude Code本体の標準機能）。project001のdocs/tasks.md・docs/progress.md・docs/decisions.mdはこれとは別の、project001テンプレートのD-001（Auto Memory登場以前）由来の独自運用である。両者は以下のように役割が異なり、置き換え関係ではない。

| | Auto Memory | docs/tasks.md・progress.md・decisions.md |
|---|---|---|
| 記録主体 | Claude自身が自動判断 | Manager/Developerが都度追記（AGENTS.mdワークフロー） |
| 記録内容 | 個人的な気づき・訂正・好みの学習 | タスク状態・作業履歴・設計判断（ADR形式） |
| 正式な記録か | 非公式（PRレビューを経ない） | 公式（PR経由でリポジトリにコミットされる） |
| 一貫性の担保 | なし（Claudeの裁量） | REVIEW.mdの敵対的検証、SubagentStop/SessionEnd Hookで機械的に補強 |

docs/配下の運用（設計判断の正式な記録、タスクの状態管理）を縮小したりAuto Memoryへ委譲したりする変更は行わない。Auto Memoryは横で自動的に動く別レイヤーとして扱い、docs/配下と重複する内容を意図的にAuto Memoryへ書き込む運用も行わない。
