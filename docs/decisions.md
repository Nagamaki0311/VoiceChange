# 設計判断記録 (ADR)

設計判断、採用理由、変更履歴を記録する。新しいエントリは末尾に追加する（古い順）。

## 記録フォーマット

```
## D-XXX: タイトル

- 日付: YYYY-MM-DD
- 状態: 採用 / 却下 / 廃止（廃止の場合は後継のDを記載）

### 背景
- なぜこの判断が必要になったか

### 決定
- 何を決定したか

### 理由
- なぜその選択をしたか（検討した代替案があれば併記）

### 影響
- この決定が及ぼす影響、制約
```

---

## D-001: タスク管理をdocs配下のMarkdownファイルで一元管理する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- 長期開発を前提としたAI開発環境として、タスクの状態・作業履歴・設計判断をセッションを跨いで引き継げる仕組みが必要だった。

### 決定
- `docs/tasks.md`（タスク管理）、`docs/progress.md`（作業履歴）、`docs/decisions.md`（設計判断）の3ファイルに役割を分離して記録する。
- `CLAUDE.md` からこれらを参照するルールを明記し、開発フロー（計画・実装・レビュー）の各段階で参照/更新することを必須とする。

### 理由
- 単一ファイルに全情報を詰め込むと肥大化し、目的別の検索性が落ちるため、関心事ごとに分離した。
- 外部のIssue管理ツールを使わず、リポジトリ内のMarkdownで完結させることで、AIエージェントが直接読み書きできるようにした。

### 影響
- 今後のタスク着手時は `tasks.md` の確認・更新が前提となる。
- 作業完了時は `progress.md` への追記が必須となる。
- 設計判断が発生した場合は、都度 `decisions.md` に追記する運用とする。

---

## D-002: project001を個別アプリ開発から共通AI開発エージェント用テンプレートへ転換する

- 日付: 2026-08-02
- 状態: 採用

### 背景
- project001を特定アプリの開発リポジトリとして運用していたが、今後は新規プロジェクトを立ち上げるたびに同じ開発ルール・ドキュメント運用の仕組みを再構築する必要が出てくることが見込まれた。
- 個別アプリの仕様やコードとテンプレートとしての基盤ルールが混在すると、テンプレートとしての再利用性が下がる。

### 決定
- project001の役割を「共通AI開発エージェント用テンプレートリポジトリ」と定義する。
- 個別アプリケーションの仕様・実装コードはこのリポジトリに保持しない。
- 新規プロジェクト作成時は、本リポジトリ（CLAUDE.md + docs/配下の3ファイル）を雛形としてコピーして初期状態とする。
- planner/developer/reviewerによる開発ループ、および `docs/tasks.md` / `docs/progress.md` / `docs/decisions.md` の運用ルールはそのまま維持する。
- あわせて、CLAUDE.mdに「トークン効率化ルール」を新設し、サブエージェントへの調査委任、実装前の方針確認、タスク切替時のコンテキストリセットなど、コンテキストを小さく保つための運用ルールを明記する。

### 理由
- 開発ルール・ドキュメント運用の仕組みは汎用性が高く、個別アプリの実装から切り離すことで、新規プロジェクトの立ち上げコストを下げられる。
- 既存の開発フロー（planner/developer/reviewer）とドキュメント運用（tasks/progress/decisions）は、アプリの種類に依存しない仕組みであるため、そのまま基盤として流用できる。
- トークン効率化ルールは、外部記事（Claude Codeのトークン節約に関する運用術）の知見を踏まえ、テンプレートとして今後利用されるすべてのプロジェクトに恩恵があるため、この段階で組み込むことにした。

### 影響
- 今後、本リポジトリに個別アプリの仕様やコードを追加する作業は行わない。
- 新規プロジェクトを開始する際は、本リポジトリをコピーして雛形として利用する運用に変わる。
- 既存の開発フロー・完了条件・ドキュメント運用ルールには変更がないため、運用面での移行コストは小さい。

---

## D-003: ponytail（DietrichGebert/ponytail）のコード品質ルールをCLAUDE.mdへ統合する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- [DietrichGebert/ponytail](https://github.com/DietrichGebert/ponytail)（MIT License）は、AIコーディングエージェント向けに「不要なコードを書かせない」ための判断ラダーとルールセットを提供するプロジェクトである。
- project001は共通AI開発エージェント用テンプレート（D-002）であり、developer/reviewerが実装・レビュー時に従うコード品質の基準がこれまで明文化されていなかった。

### 決定
- ponytailが提供する `AGENTS.md` のルール（実装前の7段階判断ラダー、削除優先・抽象化最小化などの原則、入力検証やセキュリティ等で手を抜かない対象、`ponytail:` コメントによる意図的な簡略化の明示）を日本語化し、CLAUDE.mdに「コード品質ルール（Ponytail）」として統合する。
- Claude Codeのプラグインマーケットプレイス経由（`/plugin marketplace add` 等）でのインストールは行わない。対話型CLIコマンドでありエージェントから実行できないため、ルール本文をCLAUDE.mdに直接組み込む方式を採用した。
- 導入範囲はproject001リポジトリ自体とする。テンプレートの一部となるため、今後本リポジトリを雛形としてコピーする新規プロジェクトにも自動的に引き継がれる。
- ponytailの `skills/` `commands/` `hooks/` 等のディレクトリ一式はコピーしない。ルール文書の統合のみを行う。

### 理由
- project001は「開発ルールの提供」がリポジトリの目的（Project Role参照）であり、実装フェーズの品質基準としてponytailの思想は既存の開発フロー（planner/developer/reviewer）や完了条件と矛盾しない。
- プラグイン形式でのインストールは対話操作が必須でありエージェントセッションから完結できないため、CLAUDE.mdへのルール統合という確実な方法を選んだ。
- skills/commands等のインフラ一式を持ち込むと、テンプレートの複雑性が増し「トークン効率化ルール」（CLAUDE.mdを簡潔に保つ）と矛盾するため、ルール本文の統合のみに留めた。

### 影響
- developer/reviewerは今後、実装前に判断ラダーを適用し、レビュー時にもこの基準を確認する。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このコード品質ルールが引き継がれる。
- `/ponytail-review` 等のponytail提供コマンドは未導入のため、必要になった場合は別途検討する。

---

## D-004: Manager導入とドキュメント/Agent構成をAI開発OSとして整理する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- project001はテンプレートとして長期・複数Agent運用を前提とするが、これまで「誰がタスク状態を管理し、いつ完了と判断するか」が曖昧だった（User→Planner→Developer→Reviewerの間に調整役が不在）。
- CLAUDE.mdは毎セッション自動でロードされるにもかかわらず、D-003で追加したPonytail全文（約80行）を含め273行まで肥大化しており、見出しの`\#`エスケープや行ごとの余分な空行で更に水増しされていた。トークン効率化ルール自体が形骸化していた。
- docsをagents.md等へ拡張する提案があったが、ファイル数を増やすこと自体が目的ではない。

### 決定
- **Manager導入**: `.claude/agents/manager.md`という独立ファイルは作らず、Managerを「このセッション（ルート会話）自身の役割」としてCLAUDE.mdに定義する。責務はタスク管理・Agent割当・優先順位判断・レビュー依頼・完了判定のみとし、コードは直接書かない。
- **開発フロー変更**: User → Manager → Planner → Developer → Reviewer → Manager → Complete。修正ループ（Reviewer検出時はManager経由でDeveloperへ差し戻し）もこのフローに統合。
- **CLAUDE.mdの縮小**: `\#`エスケープと冗長な空行を除去し、Ponytail原則の全文（D-003でCLAUDE.mdに置いていたもの）をdocs/agents.mdへ移設。CLAUDE.mdにはAgent一覧・フロー・参照先のみを残す（273行→約45行）。
- **docs/agents.mdを新設**: Agent構成表、オーケストレーションルール（Manager以外は互いを起動しない等）、Ponytail原則、不採用としたAgent（research/UI）の理由を1ファイルに集約する。architecture.md/project-map.md/known-issues.md/changelog.mdは作成しない。
- **developer.md/reviewer.mdを微修正**: Ponytail原則の参照先をdocs/agents.mdに統一し、reviewerには過剰実装チェックの観点を1行追加。planner.mdは役割に変更がないため無変更。
- **Hookは未実装**: セッション開始時にtasks.md/progress.mdを自動表示するSessionStart Hookを将来の検討事項としてdocs/agents.mdに記録するに留め、今回は追加しない。

### 理由（検討した代替案）
- **Managerを独立subagentにする案は不採用**: Claude Code公式の推奨構成は「ルートセッションがオーケストレーターとして専門subagentに委任する」形であり、subagentが他のsubagentを起動する構成は非公式かつ、tools設定・コンテキスト分離の面で不安定になりやすい。またManagerをsubagent化すると毎回コールドスタートし、会話の文脈（タスク履歴）を失うため、長期運用・トークン効率の両方に反する。ルート会話をManagerとすることで、「Managerだけが起動する」制約が構造的に（技術的に）保証される。
- **research Agentは不採用**: planner.mdのtools（Read/Glob/Grep/WebFetch/WebSearch）が既に現状分析・外部調査を兼ねており、分離すると役割が重複する。
- **UI Agentは不採用**: project001自体にアプリケーションコードがなく、UIレビュー対象が存在しない。必要になるのは個別アプリ側のリポジトリであり、テンプレート側で先取りして追加するのはYAGNIに反する。
- **architecture.md/project-map.md/known-issues.md/changelog.mdは不採用**: architecture.mdやproject-map.mdはアプリ固有の構造を記述するものでproject001自体には対象がなく、空のボイラープレートになる。known-issues.mdはtasks.mdの「レビュー中」状態と、changelog.mdはprogress.mdの作業履歴と役割が重複するため、既存ファイルに統合し新設しない。
- **Hookは提案のみに留めた**: SessionStart Hookは有用だが、設定ファイル(.claude/settings.json)への変更は実行環境に影響するため、必要性が具体的に確認できるまでは追加しない。

### 影響
- 今後、CLAUDE.mdを読むだけでAgent一覧とフローが把握でき、Ponytail原則の詳細やAgent構成の背景はdocs/agents.mdを開いた時だけコストが発生する（毎セッションのトークン消費を削減）。
- develop/reviewerはPonytail原則を単一ファイル（docs/agents.md）からのみ参照するため、今後ponytail側の更新を反映する際も更新箇所が1箇所で済む。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この構成（Manager運用・docs/agents.md）が引き継がれる。
- D-003で「CLAUDE.mdに統合」と決定したPonytail原則の格納場所は、本決定によりdocs/agents.mdへ変更された（D-003の決定自体は履歴として維持し、上書きしない）。

---

## D-005: SessionStart/PreCompact HookをPonytail方式で実装する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- D-004で「SessionStart Hookは有用だが確定的な必要性がないため未実装」としていたが、大規模なClaude Code構成リポジトリ（affaan-m/ECC）の実運用調査により、同種のHook（セッション開始時の状態表示、コンテキスト圧縮前の状態保存）が長期運用で有効であることが裏付けられた。
- project001はCLAUDE.mdの運用ルールで「セッション開始時にdocs/progress.md・docs/tasks.mdを確認する」を求めているが、これまで手動確認に依存しており、確認漏れのリスクがあった。

### 決定
- `.claude/settings.json`を新設し、以下2つのHookを追加する。
  - **SessionStart**: `docs/tasks.md`のタスク一覧テーブルと`docs/progress.md`の最新エントリを表示する。
  - **PreCompact**: コンテキスト圧縮前にdocs/progress.md・docs/tasks.mdへの記録を促すリマインダーを表示する。
- 実装はPonytailの判定ラダーに従い、以下を確認した上で採用した。
  1. 必要性: ECCでの実運用による裏付けあり（YAGNIを満たす）。
  2. 既存コードでの代替: なし（初導入）。
  3. 標準ライブラリ: grep/awk/echoという標準shellコマンドのみで実現可能と判断。
  4. ネイティブ機能: Claude Code純正のHook機構（SessionStart/PreCompact）をそのまま利用。
  5. 既存依存関係: 新規依存関係なし。
  6. 1行で書けるか: 両Hookとも`command`フィールド1行（複数コマンドを`;`で連結）で完結させ、専用スクリプトファイルは追加しなかった。
- update-configスキルの手順（既存設定の確認→スキーマ確認→pipe-testによる動作確認→jqによるJSON検証）に従って追加した。

### 理由（検討した代替案）
- **専用スクリプトファイル（`.claude/hooks/*.sh`）は不採用**: 各Hookの処理はgrep/awk 1個ずつの単純な抽出であり、JSON文字列内に直接書いても可読性を大きく損なわない（ダブルクォートを使わずシングルクォートと`[|]`のブラケット表現でエスケープ地獄を回避した）。ファイルを増やすとD-004の「ファイル数を最小限にする」方針と矛盾する。
- **PreCompactでの自動記録（progress.mdへの自動追記）は不採用**: 圧縮前に何を記録すべきかはLLMの判断が必要であり、shellだけでは意味のある要約を生成できない。無理に自動化すると不正確な記録が残るリスクの方が大きいため、人間（Manager）へのリマインダー表示に留めた。
- **PreToolUse/PostToolUse（lint・ビルド連携等）は不採用**: project001自体にアプリケーションコードがなく対象が存在しない。個別アプリのリポジトリ側で必要になった場合はそちらで追加する（docs/agents.md参照）。

### 影響
- セッション開始時の状態確認がHookにより自動化され、「確認し忘れ」による手戻りリスクが下がる。
- `.claude/settings.json`は新規ファイルで、`.claude/`の監視対象が変わるため、初回のみ`/hooks`を開くかセッション再起動でHookが有効化される（Claude Code側の既知の挙動）。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このHook構成が引き継がれる。
- docs/agents.mdの「将来の検討事項（未実装）」は本決定により解消され、「Hook構成」として実装済みの内容に置き換えた。

---

## D-006: 全体レビュー（重複排除・Hook環境検証・Manager-Hook接続の明文化）

- 日付: 2026-08-03
- 状態: 採用

### 背景
- project001をAI開発OSとして完成度を高めるため、CLAUDE.md/Agent設計/Hook設計/docs構成/Ponytail適用/トークン効率をユーザー指示に沿って一つずつ再レビューした。
- レビュー中に、CLAUDE.mdのPlanner/Developer/Reviewer各1行説明が、(a) docs/agents.mdのAgent構成表、(b) 各`.claude/agents/*.md`自身のfrontmatter descriptionと三重に重複していることが判明した。これはCLAUDE.md自身に書いた「同じ情報を複数箇所に保存しない」というトークン効率化ルールに反する具体的な違反であり、根本原因（docs/agents.md新設時にCLAUDE.md側の対応する記述を削除し忘れたこと）に対処する必要があった。
- ユーザーから、このセッション（Claude Code on the web）で`/hooks`コマンドが利用できないという報告があり、D-005で導入したHookが実際に機能する環境かどうかの確認が必要になった。

### 決定
- **CLAUDE.mdの重複除去**: Planner/Developer/Reviewerの個別1行説明を削除し、「Agent構成の詳細はdocs/agents.mdを参照」という参照のみに一本化した。Manager自身の責務（コードを書かない等）は、CLAUDE.md＝ルートセッション自身の指示という性質上、参照ではなくこのファイルに残す（自己参照的な行動制約のため）。
- **Manager-Hook接続の明文化**: docs/agents.mdに、SessionStart/PreCompact Hookの出力は常にManager（ルートセッション）のコンテキストにのみ注入され、subagent化されたPlanner/Developer/Reviewerには届かないことを明記した。これはD-004でManagerを独立subagentにしなかった判断を補強する2つ目の技術的根拠である。
- **PreCompactの発火タイミングの訂正**: PreCompact Hookは「Reviewerの後」等の固定ステップではなく、コンテキストサイズに応じて任意のタイミングで発火するイベントであることを明記した（ユーザー提案の直列フロー図をそのまま採用せず、より正確な表現に修正した）。
- **Hook環境依存性の文書化**: `session-start-hook`スキルの情報をもとに、`/hooks`コマンドの不在はUIコマンドの制約であり、Hook実行機構自体（`.claude/settings.json`）はリモート環境（`$CLAUDE_CODE_REMOTE`）でも動作することをdocs/agents.mdに明記した。新規セッションでは自動的に有効化され、既存settings.jsonがない状態で開始した実行中セッションのみ再読み込みが必要という区別を明確化した。
- **CLAUDE.mdへ/compact運用の1文を追加**: 「長時間セッションでは能動的に/compactを使い、PreCompact Hookの案内に従って圧縮前にdocsへ記録する」という運用ルールを追記し、既存のHookと既存のトークン効率化ルールを実際の運用としてつなげた。
- **Agent構成・Hook構成・docs構成は変更なし**: Planner/Developer/Reviewerの3Agent構成、SessionStart/PreCompact 2Hook構成、tasks/progress/decisions/agentsの4docs構成は、いずれも再検証の結果、現状維持が最適と判断した（詳細は理由を参照）。

### 理由（検討した代替案）
- **Manager Agentの独立subagent化は再度不採用**: 今回新たに「Hook出力がsubagentには届かない」という技術的根拠が加わり、D-004の判断がより強く裏付けられた。
- **Stop Hookの追加は不採用**: 応答終了ごとに発火するため、docs更新を促すリマインダーとしては頻度が高すぎ、毎ターンのトークン消費の方が確認漏れ防止のメリットを上回ると判断した。
- **PostToolUse（lint/ビルド連携）は不採用**: project001自体にアプリケーションコードがなく対象が存在しない（D-005から変更なし）。
- **architecture.md/project-map.md/known-issues.md/changelog.mdの追加は再度不採用**: D-004の判断（対象の不在、既存ファイルとの役割重複）から状況の変化がないため据え置いた。
- **progress.mdの分割（アーカイブ化）は今回は不採用**: 現時点でファイルサイズは小さく、必要性が確認できないため実施を見送った。将来ファイルが肥大化した場合の選択肢として認識はしている（docs/tasks.mdのメモ参照）。

### 影響
- CLAUDE.mdはさらに簡潔になり、Agent情報の更新はdocs/agents.mdの1箇所で完結するようになった（今後同種の重複が再発しにくい）。
- Hookの環境依存性が文書化されたことで、他の実行環境（CLI/IDE拡張等）でproject001を使う場合の期待値のズレを防げる。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、今回の修正が引き継がれる。

---

## D-007: Agent別モデル最適化（Model Routing）を導入する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- Planner/Developer/Reviewerは全て`model: inherit`のままであり、Managerのセッションモデルにそのまま追従していた。Managerが何のモデルで対話しているかによってPlanner等の品質・コストが意図せず変動しうる状態だった。
- 常に最高性能モデルを使うのではなく、役割ごとに必要十分なモデルを割り当てることで、品質を維持しながらコスト・速度を最適化したいという要望があった。

### 決定
- `inherit`をやめ、Agentごとに`model`を固定する。
  - Planner: `opus`（設計品質を最優先。安易な軽量化はしない）
  - Developer: `sonnet`（コーディング用途の標準モデル。品質優先だが常時opusは不要）
  - Reviewer: `sonnet`（既定はコード品質・セキュリティレビューを想定し下げすぎない）
  - Manager: 変更なし（ルートセッション自身のモデルをそのまま使用。`.claude/settings.json`での上書きは行わない）
- Markdown/README/docsの体裁確認など軽量なレビューについては、専用の軽量Agentを新設せず、ManagerがReviewerを起動する際にAgent呼び出しの`model`パラメータで`haiku`等へ一時的に上書きする運用とする。reviewer.md自体（既定モデル）は変更しない。

### 理由（検討した代替案とPonytail判定ラダーの適用）
- **Managerのモデルを`.claude/settings.json`で強制する案は不採用**: ルートセッションはUser自身が選んだ対話モデルであり、project001が上書きすると、このリポジトリでの作業全体（Manager業務に限らない、Userとの雑談的なやり取りも含む）に影響する。Managerの業務内容（判断・割当・状態管理）自体は既存の推論力で十分であり、上書きの必要性が確認できないため見送った。
- **軽量レビュー専用Agent（例: docs-reviewer）の新設は不採用**: Ponytailの判定ラダー（3〜4段目: 標準ライブラリ／ネイティブ機能で足りるか）を適用すると、Claude Code側に既に「Agent呼び出し時のmodelパラメータ上書き」という機能があり、これで要件を満たせる。新規Agentファイルを追加すると、Reviewerとの役割重複（レビューという同じ機能を担うAgentが2つになる）を招くため、既存機能の活用を優先した。
- **Reviewerの既定モデルをhaikuに下げる案は不採用**: project001は不特定多数の個別アプリのテンプレートであり、コピー先では実際のアプリケーションコードに対するセキュリティ・正確性レビューが行われる。テンプレートの既定を下げすぎると、それを継承した個別プロジェクトの品質が下がるリスクがある（「品質が低下すると判断したAgentは現状維持を選択する」という方針に従った）。
- **Developerをopusに引き上げる案は不採用**: Developerの作業は基本的にPlannerが決めた方針に沿った実装であり、Planner ほどの開放的な設計判断は求められない。Sonnetはコーディング用途の標準モデルとして十分な理解力を持つと判断した。
- **Developerの一部処理だけ軽量モデルに分離する案は不採用**: 単一のAgent定義内でタスクの種類ごとにモデルを動的に切り替える仕組みはなく、無理に分離すると新規Agent追加と同じ複雑化を招く。トークン最適化は「軽量モデルへの分割」ではなく「不要なAgent起動を避ける」（Managerの招集判断）で対応する方針とした。

### 影響
- Planner/Developer/Reviewerの品質・コストは、Managerが使用する対話モデルに左右されず安定する。
- 軽量レビューが必要な場面では、Managerが呼び出し時に`model`パラメータを指定するだけで対応でき、Agent定義ファイルやsettings.jsonの追加変更は不要。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このモデル構成が引き継がれる。個別プロジェクトでコーディング言語やドメインの複雑さが分かった時点で、Developer/Reviewerのモデルを調整することは妨げない。

---

## D-008: ルートにAGENTS.mdを新設し、CLAUDE.mdとの責務を分離する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- ユーザーから、汎用的な開発方針7項目（後方互換性を維持しない、最小実装、段階的成長、モジュール化、成熟したライブラリの優先、既存依存関係の再利用、長期保守性の重視）を最優先ルールとして統合してほしいという要望があった。
- これを機に、「AIエージェントが常に守る開発方針・設計原則・ワークフロー・レビュー基準」と「Claude Code固有の設定・運用・参照導線」の責務を分離する（AGENTS.md／CLAUDE.md）よう指定された。
- 注記: ここで新設するproject001直下の`AGENTS.md`は、T-003（D-003）で参照した「ponytail（DietrichGebert/ponytail）リポジトリ内のAGENTS.md」とは別物である。ponytail側は判定ラダー等の原文の出典に過ぎず、project001の`AGENTS.md`はそれを統合した本リポジトリ自身の方針文書である。

### 決定
- ルートに`AGENTS.md`を新設（48行、目標150行以内）。内容は「設計原則」（判定ラダー＋新7項目を統合・重複排除・矛盾解消した8項目）、「ワークフロー」（User→Manager→Planner→Developer→Reviewer→Manager→Complete、修正ループ、Agent起動制約）、「レビュー基準」（完了条件）、docsへの参照のみ。
- `CLAUDE.md`は先頭に`@AGENTS.md`のimportを置き、内容をClaude Code固有の運用（セッション/Hook運用、トークン効率化、Skills/Agent Reachの導線、参照ドキュメント一覧）のみに絞った。開発方針・設計原則・ワークフロー・レビュー基準は書かない。
- `docs/agents.md`から、AGENTS.mdと重複していたPonytail全文（判定ラダー・原則・手を抜かない対象、約37行）とオーケストレーションルールの重複5行を削除し、AGENTS.mdへの参照に置き換えた。Agent構成表・モデル構成（Model Routing）・Hookとの技術的接続・Hook構成・採用しなかったAgentの理由など、AGENTS.mdに書くには詳細すぎる情報はdocs/agents.mdに残した。
- `.claude/agents/developer.md`・`reviewer.md`のPonytail参照先を「docs/agents.md」から「AGENTS.md」へ更新した。
- `README.md`にAGENTS.mdを構成一覧の先頭に追加し、開発フロー詳細の参照先をAGENTS.mdへ変更した。

### 重複ルール一覧と統合内容（新7項目 × 既存Ponytail原則）
| 新ルール（ユーザー提示） | 重複していた既存ルール | 統合後の扱い |
|---|---|---|
| 後方互換性を維持しない、廃止パスは削除 | なし（新規） | AGENTS.md 設計原則3としてそのまま追加 |
| 最小実装・投機的抽象化/設定/間接層を避ける | 「明示的に要求されていない抽象化を追加しない」「求められていないボイラープレートを書かない」 | 1つの原則（5）に統合し、新ルールの「投機的設定・間接層」という語を追加 |
| 段階的成長・動くものの上に積む | なし（新規） | AGENTS.md 設計原則4としてそのまま追加 |
| モジュール化・責務分離 | 「ファイル数は最小限に」 | 一見矛盾するため、「責務は分離するが、それは新しいファイル・抽象化を作ることとは別問題」として1原則（5）に統合し矛盾を解消 |
| 成熟したライブラリを優先、無断で再実装しない | 判定ラダー3〜5段目（標準ライブラリ／ネイティブ機能／既存依存関係） | ラダー3段目に「成熟した外部ライブラリ」「ドキュメント確認」の文言を追加して統合 |
| 既存依存関係を再利用、ドキュメント確認 | 「避けられる場合は新しい依存関係を追加しない」 | ラダー3・5段目に統合 |
| 長期保守性重視・一時しのぎを避ける | 「最短の差分が勝つ」「意図的な簡略化は`ponytail:`コメントで明記」 | 矛盾するため、「最小 ≠ 一時しのぎ」という原則（6）を新設し、正しく理解した最小実装と黙って残す一時しのぎを区別する形で整理 |

### 削除内容
- `docs/agents.md`の「コード品質ルール（Ponytail）」節（約37行、判定ラダー全文・原則・手を抜かない対象・`ponytail:`コメント運用の説明）を削除し、AGENTS.mdへの参照1行に置き換えた。
- `docs/agents.md`の「オーケストレーションルール」からManager起動制約・開発フロー・修正ループの重複5行を削除し、AGENTS.mdへの参照に置き換えた（Hookとの接続などdocs固有の技術的補足は残した）。

### 検討したが不採用の案
- **SKILL.mdへの分離**: 現時点でproject001にSkillsやAgent Reachは導入されておらず、AGENTS.md/CLAUDE.mdの記述にもSkill化すべき具体的な再利用ワークフローは存在しない。空のSKILL.mdや未使用の仕組みを先取りして作ると、YAGNIおよび「不要な抽象化や将来使うかもしれない設計は追加しない」という方針に反する。CLAUDE.mdに「導入する場合の指針」だけを1節残し、実体は作らなかった。
- **CLAUDE.mdでのAGENTS.md内容の再掲**: `@AGENTS.md`のimportにより、CLAUDE.mdを開けば実質的にAGENTS.mdの内容も読み込まれるため、要約であっても再掲すると三重管理（AGENTS.md本体／import／CLAUDE.mdの要約）になり、過去のD-006と同じ重複問題を再発させる。importのみとし、要約の再掲はしなかった。

### 影響
- 開発方針の更新は今後AGENTS.md 1箇所で完結する。Ponytail側のルールが更新された場合も、更新箇所はAGENTS.mdのみになった。
- CLAUDE.mdはClaude Code固有の内容だけになり、他のAIエージェント（AGENTS.md規約に対応する他ツール）からもAGENTS.mdを直接読める構成になった。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、AGENTS.md／CLAUDE.mdの責務分離が引き継がれる。

---

## D-009: Agent-Reach対応をOptional Dependencyとして追加する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- ユーザーから、[Panniantong/Agent-Reach](https://github.com/Panniantong/Agent-Reach)（MIT License、`agent-reach`というpipパッケージのCLIツール。GitHub/Web/RSS/YouTube/Reddit/X等を横断する情報収集の呼び出し方をSKILL.md経由でAgentに教える仕組み）への対応要望があった。
- 要件は「本体を組み込まず、利用可能な環境では自動活用、未導入環境では従来どおり動作する」疎結合設計。project001自体をコードとして持たないテンプレートであるため、統合はドキュメントとAgent定義への指示のみで実現する。
- ユーザー提示の前提には「REVIEW.md」「Issue運用」など、project001の実際の構成と一致しない記述があった。project001では完了条件はAGENTS.mdに集約済み（D-008）、状態管理はGitHub Issuesではなくdocs/tasks.mdで行っている。これらは新規に作らず、既存の構成（AGENTS.md・reviewer.md・docs/tasks.md）を拡張する形で要件を満たした。

### 決定
- **Researcher Agentを新設**（`.claude/agents/researcher.md`、`model: sonnet`）。外部情報収集・重複除去・信頼性評価を担当し、Plannerが必要と判断した場合のみManagerが起動する（常時起動しない）。
- **検出方法**: `command -v agent-reach >/dev/null 2>&1 && agent-reach doctor` を調査開始前に実行し、成功時のみAgent-Reachの提供チャネルを優先利用する。失敗時（未導入・doctorエラーいずれも）は即座にWebFetch/WebSearchへフォールバックする。未導入環境で`command -v agent-reach`が`exit 1`で安全に失敗することを本セッションで実機確認済み。
- **docs/agent-reach.md新設**: Agent-Reachの正体、検出方法、フォールバック方針、疎結合設計（依存を持たない、詳細をこの1ファイルに集約）を記載。
- **docs/research-workflow.md新設**: Planner→Researcher→Developer→Reviewerの調査フロー、Researcherを起動する条件、Reviewerが確認する調査品質基準（情報源の妥当性・最新性・重複・Agent-Reach利用可否による品質差）を記載。
- **AGENTS.mdのワークフローにResearcherを追加**（外部調査が必要な場合のみの分岐として、主フローは変更しない）。Agent-Reachという固有名詞はAGENTS.mdには書かず、「利用可能な検索ツールがあれば優先利用」という汎用的な表現にとどめた（AGENTS.mdは全AIエージェント共通の方針であり、特定ツールの銘柄をここに書くとCLAUDE.mdとの責務分離（D-008）が崩れるため）。
- **CLAUDE.mdの「Skills / Agent Reach」節を更新**: Agent-Reachの基本方針（Optional Dependency、検出・フォールバックの一文）のみ記載し、詳細はdocs/agent-reach.mdへ委譲した。
- **docs/agents.mdを更新**: Agent構成表・Model Routing表にResearcherを追加。「採用しなかったAgent」の`research`項目を、D-004時点の判断を覆した経緯（Agent-Reach対応により役割分離が明確になったため）として書き換えた（削除ではなく、判断が変わった理由を残す）。
- **docs/tasks.mdの状態定義に`調査中`を追加**。あわせて「完了条件（CLAUDE.md参照）」という古い参照（D-008でAGENTS.mdへ移管済みだったが未修正だった）を「AGENTS.md参照」に修正した。
- **planner.md/developer.md/reviewer.mdを更新**: Plannerは調査対象とチャネルを整理してResearcherに委ねる（自身の大量調査は行わない）、Developerは調査結果を実装に反映する、Reviewerは調査品質も確認する、という役割分担を明記した。

### 重複ルール一覧・削除内容
- 新規追加であり、既存ルールとの重複はない。ただしAgent-Reach固有の検出コマンド・チャネル一覧はdocs/agent-reach.mdのみに記載し、researcher.md/CLAUDE.md/AGENTS.mdには要点のみを書き、複製しなかった（同じ情報を複数箇所に保存しないというAGENTS.md/CLAUDE.mdの既存ルールを踏襲）。

### 検討したが不採用の案
- **REVIEW.mdの新規作成**: ユーザー提示の前提に基づけば作成候補だったが、project001には現在「レビュー基準」がAGENTS.mdに一元化されている（D-008）。追加する調査品質基準（4項目程度）は既存のAGENTS.md「レビュー基準」とreviewer.mdの拡張で十分収まる分量であり、新規ファイルにすると再びレビュー基準が分散する（D-006/D-008の教訓に反する）。docs/research-workflow.mdにReviewer観点の詳細を書き、AGENTS.md/reviewer.mdからはそちらを参照する形にした。
- **docs/architecture.mdの新規作成**: D-006で「project001自体に汎用アーキテクチャの記述対象がない」として不採用にした判断は今回も維持した。ただしAgent-Reach統合固有のデータフロー（検出→フォールバック→Agent間の受け渡し）は、docs/agent-reach.mdの「疎結合設計（アーキテクチャ）」節に図として記載し、要望の実質（統合の設計を可視化する）には応えた。
- **GitHub Issues運用への切り替え**: ユーザー前提文の「Issue運用を維持」は、project001の実態と異なる（現状はdocs/tasks.mdによる状態管理）。より大きな運用変更になり要望の範囲を超えるため、既存のdocs/tasks.md運用を「Issue運用に相当するもの」として維持する解釈を採った。
- **`agent-reach`本体の依存追加やvendoring**: project001はアプリケーションコードを持たないテンプレートであり、依存を追加する対象（package.json等）がそもそも存在しない。検出はシェルコマンドのみで行い、コード上の依存は一切追加しなかった。
- **SessionStart Hookでの起動時Agent-Reach検出**: Manager（root）はResearchを行わないため、セッション開始時に毎回検出する必要性がない。検出はResearcher起動時にBashで都度行う設計とし、新規Hookは追加しなかった（既存のHook最小主義を維持）。

### 影響
- Agent-Reachが導入された環境では、ResearcherがGitHub/Web/RSS/YouTube/Reddit/X等を横断的に調査できる。未導入環境でも、WebFetch/WebSearchへのフォールバックにより同じワークフローで動作し、機能停止しない。
- project001のリポジトリにAgent-Reachへのコード依存は一切追加されていないため、Agent-Reach側のインターフェース変更はdocs/agent-reach.mdの更新のみで追従できる。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、Researcher Agentと疎結合なAgent-Reach対応が引き継がれる。

---

## D-010: レビュー方針を敵対的検証（Adversarial Review）へ変更し、REVIEW.mdへ集約する

- 日付: 2026-08-03
- 状態: 採用

### 背景
- 現状のレビューは「問題がないか確認する」姿勢に寄りがちで、実装を追認しやすい構造だった。ユーザーから、レビュー担当を品質保証・監査・ペネトレーションテストの立場に切り替え、「壊れる理由」「失敗する理由」「仕様を満たさない理由」を優先して探す敵対的検証（Adversarial Review）への転換要望があった。
- D-009では「REVIEW.mdの新規作成」を、当時想定していた追加項目（調査品質基準4項目程度）の分量では既存構成で足りるとして不採用にしていた。今回要求されたレビュー方針は、18観点・7手順・重要度5段階・指摘5項目という大幅に大きい分量であり、AGENTS.md/reviewer.mdに収めると再び責務が混在する。D-009の当該判断はこの点で前提が変わったため見直した。

### 決定
- ルートに`REVIEW.md`を新設。基本姿勢（敵対的検証）、レビュー観点（18項目）、レビュー手順（7段階）、重要度分類（Critical/High/Medium/Low/Nit）、指摘の記載形式、レビュー完了条件を集約した。
- **検証パスを追加**（ユーザー要望を超えて提案・採用した改善）: 手順4「壊れるケースを列挙する」で出た仮説を無条件に報告せず、手順5で実際にコード・挙動を確認し `CONFIRMED`（確認済み）/`PLAUSIBLE`（確認しきれないが疑いが強い）のいずれかを付与してから報告する。裏付けが取れない仮説は報告しない。これは「肯定的結論を安易に出さない」という要求と対になる、逆方向のリスク（存在しない問題の捏造・誤検出）への対策であり、Anthropicの構造化レビュー手法（file:line根拠、failure_scenario、検証済み/推測の区別を分けて報告する設計）に沿った提案として採用した。
- `.claude/agents/reviewer.md`のdescription/システムプロンプトを敵対的検証の立場に更新。旧方針にあったレビュー手法の詳細（file:line明記、指摘の3点セット等）はREVIEW.mdへ統合したため削除し、参照のみ残した（同じ情報を複数箇所に保存しない）。
- `AGENTS.md`の「レビュー基準（完了条件）」に、詳細な姿勢・観点・手順はREVIEW.mdに従う旨の一文を追加。AGENTS.md自体の記載量は増やしていない。
- `CLAUDE.md`は開発ルールのまま変更せず、「参照ドキュメント」にREVIEW.mdを追加し、冒頭の説明文を「レビュー基準はAGENTS.md」から「レビュー方針はREVIEW.md」に修正した（ユーザー前提の「開発ルールはCLAUDE.mdのまま」は、project001の実際の構成では開発ルールがAGENTS.mdにあるため、CLAUDE.md自体は変更せず参照導線のみ追加する形で解釈した）。
- Reviewerのモデル（`sonnet`）は変更しなかった。敵対的検証の厳格化は主に手順（検証パスの明文化）で担保する設計とし、モデルのグレードアップは不要と判断した。

### 重複ルール一覧・削除内容
- `.claude/agents/reviewer.md`から、REVIEW.mdと重複する記述（指摘は事実に基づくこと、問題/状況/直し方の3点セット、file:line明記）を削除し、「レビューの姿勢・観点・手順...はREVIEW.mdに従う」という参照1行に置き換えた。

### 検討したが不採用の案
- **ReportFindingsツールをreviewer.mdのtoolsに追加**: 構造化された指摘報告の仕組みとして検討したが、custom subagent（`.claude/agents/*.md`）がこのツールへアクセスできるか確証が得られなかった。誤った前提でツール参照を書くとレビューが機能しなくなるリスクがあるため採用せず、同等の規律をREVIEW.md内のプレーンテキストの指摘形式として明文化した。
- **Reviewerのモデルをopusへ引き上げ**: 敵対的検証の厳格化は手順（検証パスの導入）で対応可能と判断し、D-007の「Reviewerはsonnetで十分」という判断を維持した。
- **AGENTS.mdへのレビュー手順統合**: レビュー専用ルールを開発ルールと同じファイルに置くと、AGENTS.mdが150行の目標に対して大きく膨らむ（レビュー手順だけで約35行）。AGENTS.md/REVIEW.md/CLAUDE.mdの3ファイル体制とし、責務を分離した。

### 影響
- reviewer Agentは今後、REVIEW.mdの手順（仮説列挙→検証パス→CONFIRMED/PLAUSIBLE判定→報告）に従う。「問題ありません」等の未検証の肯定的結論は使用しない。
- レビュー基準の更新は今後REVIEW.md 1箇所で完結する。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、敵対的検証の方針が引き継がれる。個別アプリではUX/ブラウザ互換性等の観点も実際に該当するようになる。

---

## D-011: 新規プロジェクト初期化手順をREADME.mdへ集約し、SessionStart Hookを完了タスク除外に変更する

- 日付: 2026-08-05
- 状態: 採用

### 背景
- D-002でproject001をテンプレートリポジトリと定義し「新規プロジェクト開始時は本リポジトリをコピーして雛形とする」と決定していたが、コピー後に`docs/tasks.md`・`docs/progress.md`・`docs/decisions.md`をどう初期状態へ戻すかという具体的な手順がこれまで文書化されていなかった。
- リセットしないままだと、新規プロジェクトのSessionStart Hook（D-005）がproject001自身の構築履歴（T-001〜T-010等）を毎セッション表示し続けてしまう。さらにタスクが積み重なるほど表示されるT-xxx行数が増え、完了済みタスクまで含めて表示し続けるとトークン消費が件数に応じて線形に増加する構造上の問題も別途あった。

### 決定
- **初期化手順をREADME.mdに集約**: 「## 使い方」直後に「### 新規プロジェクトでの初期化」を新設し、`docs/tasks.md`（タスク一覧表のヘッダ・区切り行のみ残しT-xxx行・バックログを削除、列構成は変えない）・`docs/progress.md`（記録フォーマット直後の`---`〈この行を含む〉より下を削除）・`docs/decisions.md`（同様に`---`〈この行を含む〉より下のD-xxxを削除）・README.md自身（本節は削除可）の4項目の手順を記載した。AGENTS.mdの「## ドキュメント」節には手順本体を複製せず、README.mdを参照する1行のみを追加した。
- **SessionStart Hookを完了タスク除外に変更**: `.claude/settings.json`のSessionStartのcommandに`awk -F'|' '$0 ~ /^[|] (ID|T-[0-9]+) [|]/ && $5 !~ /^ *完了 *$/' docs/tasks.md`を追加し、`docs/tasks.md`の行を`|`区切りで列分割した上で状態列（5列目）が`完了`の行を表示対象から除外した。PreCompactは変更していない。D-005で確立した「ダブルクォート不使用・1行完結・専用スクリプトファイルを作らない」という方針はそのまま維持した。
  - 初回実装では`grep -vE '[|] *完了 *[|]'`により列位置を見ず行全体への部分文字列一致で判定していたため、備考欄など状態列以外のセルが単独で`完了`（前後空白のみ）と完全一致する行まで誤って除外される不具合があった（Reviewer指摘、CONFIRMED、Medium）。修正ループでawkによる列指定フィルタへ置き換え、状態列のみを判定対象とすることで根本原因を解消した。実データ（`docs/tasks.md`）と、状態が`未着手`で備考欄のみ`完了`という再現データの両方で修正後の挙動を確認済み（詳細はdocs/progress.md参照）。
- **docs/agents.mdのHook構成説明を更新**: 表示対象が未完了タスクである旨、完了タスクを除外する理由（トークン消費の線形増加を防ぐため）、状態列（5列目）の値を`awk -F'|'`で判定しておりtasks.mdの表書式（列構成）を前提とする旨を追記した。
- **Managerが確認した3点を本決定に反映**: (1) リセット対象に`docs/decisions.md`も含める、(2) コピー手段（git clone / GitHub Templateリポジトリ機能 / ファイルコピーのいずれか）は特定せず、いずれの方法でも`docs/`の中身がそのままコピーされる前提で手順を書く、(3) 本作業はT-011として1タスクにまとめる。

### 理由（検討した代替案）
- **初期化を自動化するスクリプト/コマンドの追加は不採用**: project001はアプリケーションコードを持たないテンプレートであり、リセット対象の削除は単純なMarkdown編集（表の行削除、区切り線以下の削除）に過ぎない。判定ラダー（1: そもそも必要か）に照らし、手順書（README.md）で十分要件を満たせるため、新規スクリプトファイルや依存関係を追加しなかった。
- **リセットしないファイルを個別列挙する書き方は不採用**: `AGENTS.md`・`CLAUDE.md`・`docs/agents.md`等を個別に列挙すると、将来docs/にファイルが増えるたびに記述が古くなる。「docs/のうちtasks.md/progress.md/decisions.mdの3つのみをリセットする」という内包的な表現にすることで、対象外ファイルの列挙を不要にした。
- **SessionStart Hookでの件数制限（例: 直近N件のみ表示）は不採用**: 完了/未完了という状態の意味に基づくフィルタの方が、Managerが実際に把握すべき情報（今何が残っているか）と一致する。件数による打ち切りは、未完了タスクがN件を超えた場合に表示漏れが生じるリスクがあり、状態ベースのフィルタより劣ると判断した。
- **本決定を新規のD番号ではなくD-005の改訂として扱う案は不採用**: D-005（Hook導入の初回決定）は履歴として維持し、本決定はその変更点（完了タスク除外）のみを記録する後継のD-011とした。同様にD-002（テンプレート化の初回決定）も維持し、本決定はD-002が言及していなかった具体的なリセット手順を補完する位置づけとした。

### 影響
- 新規プロジェクトへコピーした直後、README.mdの手順に従うことでSessionStart Hookがproject001自身の構築履歴を表示しなくなる。
- SessionStart Hookは今後、タスク件数が増えても未完了タスクのみを表示するため、トークン消費が完了タスクの累積によって線形に増加することがなくなる。
- `docs/tasks.md`の表書式（列構成、状態列の値として`完了`を使うこと）を変更する場合は、SessionStart Hookのフィルタ条件（`awk -F'|'`による状態列＝5列目の判定）も合わせて見直す必要がある制約が生じた。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この初期化手順とHookの完了タスク除外が引き継がれる。

---

## D-012: Capability Layer（Agent-Reach・Code Review Graphの検出規約）を統合する

- 日付: 2026-08-05
- 状態: 採用

### 背景
- project001はAgent-Reach（D-009）を「検出できれば優先利用・なければフォールバック」という指示規約として個別に統合していたが、同種の外部ツール（Code Review Graph等）が今後増えるたびに同じ検出パターンを都度別個に設計すると、規約自体が分散し重複する懸念があった。
- ユーザーから、この検出パターンを一般化した「Capability Layer」として整理し、あわせてCode Review Graph（コードベースをグラフ化し変更の影響範囲を解析するCLIツール）をOptional Dependencyとして統合してほしいという要望があった。要求リストには他にGitHub CLI/Playwright/Node.js/Python/MCP Serverも含まれていたが、project001自体はアプリケーションコードを持たないテンプレートであり、これらを実際に消費するタスクが現時点で存在しない。

### 決定
- **Capability Layerを「実行可能な基盤ではなくAgentへの指示規約」として実装する**。検出ロジック自体をスクリプトやフレームワークとしてコード化すると、それ自体が正当化できない新規依存になる（AGENTS.mdの判定ラダー3〜5段目に反する）ため、新規スクリプトファイルは一切追加せず、`docs/capability-layer.md`に「`command -v`で検出→利用可能なら優先利用→不可なら即座にフォールバック→ツール固有の仕様は専用docsに集約」という規約を文書化し、各Agent定義ファイル（planner/developer/reviewer/researcher.md）に1行ずつ参照を追加するに留めた。
- **要求された7ツールのうちAgent-ReachとCode Review Graphの2つのみを実際にAgentの振る舞いへ統合する**。GitHub CLI/Playwright/Node.js/Python/MCP Serverは、project001自体に消費するタスクが存在しないため（YAGNI）、検出コマンドの型のみを`docs/capability-layer.md`の「将来のCapability候補」表に記載し、Agentの振る舞い（`.claude/agents/*.md`）への統合は見送った。個別アプリのリポジトリで実際に使うタスクが発生した時点で、この表に沿って追記する運用とする。
- **Code Review GraphはCLI直接呼び出しのみを統合し、MCPサーバーモード（`code-review-graph serve`）は統合しない**。デーモンプロセスの起動・停止管理や`.mcp.json`への新規登録が必要になり、project001が重視する疎結合設計（依存を追加しない、いつでも取り除ける）に反するため、CLIの`build`/`update`/`detect-changes --brief`のみをDeveloper/Reviewerの振る舞いに組み込んだ。
  - 修正ループ（Reviewer指摘対応・1周目、High/CONFIRMED）: 初回統合時はBlast Radius Analysis / Impact Analysisも`detect-changes --brief`にマッピングしていたが、Reviewerが実機検証（`pip install code-review-graph`、2ファイル構成のgitリポジトリで`build`後に`detect-changes --brief`と`impact --files <変更ファイル>`を比較）した結果、`detect-changes --brief`は呼び出し元・影響ファイルを一切返さず、`--help`に"Analyze the blast radius of changes"と明記された別サブコマンド`impact`（`--files`/`--depth`/`--max-results`、`impacted_nodes`/`impacted_files`を含むJSONを返す）が影響範囲解析の実体であることが判明した。DeveloperがReviewerの手順を再現確認した上で、`docs/code-review-graph.md`のマッピングをReview Delta＝`detect-changes --brief`、Blast Radius Analysis / Impact Analysis＝`impact --files <変更ファイル>`に訂正した（詳細はdocs/progress.mdのT-012修正ループ参照）。
- **docs/architecture.mdは新設しない**。D-004/D-006/D-009で3回却下済みの判断（project001自体に汎用アーキテクチャの記述対象がない）を維持し、Capability Layer固有のデータフロー図は`docs/capability-layer.md`内に収めた（D-009でAgent-Reach固有の図をdocs/agent-reach.md内に収めた前例を踏襲）。
- **docs/review-workflow.mdは新設せず、REVIEW.md自体を拡張する**。レビュー観点に「依存関係（呼び出し元・呼び出し先への影響、blast radius）」を19項目目として追加し、検証パスの判定にCode Review Graphの検証手段を追記した（修正ループを経た最終版では、呼び出し元・影響ファイルの検証は`impact --files`、変更内容の要約確認は`detect-changes --brief`という役割分担で明記している。上記「修正ループ」参照）。別ファイルに分割すると、過去（D-006/D-008）に繰り返し発生した重複問題（同じ情報を複数箇所に保存してしまい更新漏れが生じる）を再発させるため、既存のREVIEW.mdへの追記に留めた。
- **「PR Review」機能（`/code-review-graph:review-pr`）は統合しない**。エディタ統合用のスラッシュコマンドであり、Agentから直接呼び出せる確証が得られなかった。ユーザーが本セッションで`/hooks`コマンドを利用できないと報告した過去の教訓（T-006）と同種の「UIコマンドはAgentから呼び出せるとは限らない」という制約を警戒し、代わりに`detect-changes --brief`で得られる情報をReviewerが解釈する運用とした。
- **SessionStart HookにCapability検出を追加する**。`.claude/settings.json`のSessionStartのcommand先頭に`agent-reach`・`code-review-graph`の利用可否を`command -v`で検出し表示する処理を追加し、Manager経由で各Agent起動時にこの結果を伝達できるようにした（同じ検出コマンドをAgentごとに繰り返し実行する無駄を避けるキャッシュ効率化）。ただし各Agent（researcher.md等）は`command -v`による自己検出も維持する。D-009の「Hookでの起動時Agent-Reach検出は不採用」という判断を今回は見直したが、これはHookが発火しない実行環境が存在する（T-005/T-006の教訓）ことへの対応として、自己検出という既存のフォールバック経路を廃止せず併存させる設計にしたためであり、Hookへの依存を新たに必須化するものではない。

### 理由（検討した代替案）
- 上記「決定」内の各項目に代替案（MCPサーバーモード統合、architecture.md新設、review-workflow.md新設、PR Review機能統合、7ツール全統合）を記載済みであり、いずれも疎結合設計の維持・過去の重複問題の再発防止・YAGNI・Agentからの呼び出し確証の欠如を理由に不採用とした。

### 影響
- Agent-Reach・Code Review Graphのいずれも未導入の環境（本セッション含む）では、SessionStart Hookが両方`unavailable`と表示し、各Agentは従来どおりRead/Grep/Bash/WebFetch/WebSearchによる標準フローで動作する。動作確認はdocs/progress.mdのT-012参照。
- 今後3つ目以降のCapabilityを統合する際は、`docs/capability-layer.md`の規約（検出→優先利用→フォールバック→専用docsへの詳細集約）にそのまま従うことができ、規約自体の再設計は不要になる。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このCapability Layerの規約とAgent-Reach・Code Review Graphの統合が引き継がれる。GitHub CLI等の残り5ツールは、個別アプリのリポジトリ側で必要になった時点で`docs/capability-layer.md`の表に沿って追加統合する。

---

## D-013: サブエージェント進捗を`subagentStatusLine`で可視化する

- 日付: 2026-08-05
- 状態: 採用

### 背景
- ユーザーから、サブエージェント実行中のみ、専門知識のないユーザーでも直感的に理解できる形で進捗を可視化してほしいという要望があった。制約として「Claude Code標準機能を活用」「不要なLLMトークンを消費しない」「状況変化時のみ更新」が明示されていた。

### 決定
- **HooksではなくsubagentStatusLineを採用する**。HooksはLLMの会話コンテキストへテキストを注入する仕組みであり、継続的な進捗表示に使うとトークンを消費し続けてしまう。`subagentStatusLine`（`.claude/settings.json`）はエージェントパネルというUI側でレンダリングされるだけで会話コンテキストに入らないため、「不要なLLMトークンを消費しない」という制約を構造的に満たす。Agent View（サブエージェント実行時のみ自動的に現れるパネル）とsubagentStatusLine（そのパネルの各行の表示内容）は競合する選択肢ではなく、後者は前者の中身をカスタマイズするものであり、「通常チャットでは表示しない／エージェント実行時のみ自動表示」はAgent View自体の既定挙動としてすでに満たされている。
- **専用スクリプトファイル（`.claude/statusline-subagent.sh`）を新設する**。既存のHookは1行コマンドで済ませてきたが、今回はサブエージェント種別→日本語文言の対応表（case分岐、将来の拡張性のため）と、複数の候補フィールドへの防御的なフォールバック抽出が必要で、1行コマンドに詰め込むと可読性・保守性が著しく落ちるため、Ponytailの「1行で書けるか」を検討した上で専用ファイルを選んだ。
- **`.claude/agents/*.md`の`name`フロントマターとの対応表を1つのスクリプト内に集約する**。新しいサブエージェントを追加する際は、対応表に1行足すだけで拡張できる構造にした。

### 実装過程で発見・修正した問題（実機検証の記録）
- 最初にstatusline-setupエージェント（Read/Editのみ、Bashを持たない）へ依頼した際、`subagentStatusLine`の公式stdinスキーマが不明なまま「1エージェント1回呼び出し、フラットなJSON、プレーンテキスト出力」という誤った前提で実装された。ManagerがWebSearch/WebFetchで公式ドキュメント（code.claude.com/docs/en/statusline）を確認した結果、実際は「リフレッシュごとに1回、表示中の全サブエージェントを`tasks`配列でまとめて渡す」「出力はタスクごとに1行のNDJSON `{"id":..., "content":...}`」という全く異なる仕様であることが判明し、スクリプトを書き直した。
- 当初`columns`（行幅）に合わせて`cut -c`で切り詰める実装にしていたが、実行環境の`LC_CTYPE=POSIX`（Cロケール）では`cut -c`がバイト単位で切り詰め、日本語が文字化けすることを実機テストで発見した。`columns`がコードポイント数・表示幅のどちらを指すかも公式ドキュメントに記載がなく確認できなかったため、行全体のtruncationを撤去し、内容自体を短く保つ（description部分のみjqのUTF-8安全なスライスで20文字に軽く制限する）方針に変更した。
- `jq -n`（`-c`なし）が複数行に整形出力するため「1行1タスク」というNDJSON仕様に違反していたことを発見し、`-c`（compact）フラグを追加した。
- 現在実行中のツール名を表すフィールドは公式スキーマに存在しないため、当初想定していた「developer内で実装中/確認中を区別する」という細分けは実現不可能と判断し、断念した（確認できない挙動を前提にした実装はしない。AGENTS.mdの「理解してから作る」原則）。

### 理由（検討した代替案）
- **Hooksでの実装は不採用**。上記の通りトークン消費の要件に反する。
- **1行コマンドでの実装は不採用**。ロジックの複雑さ（対応表・防御的フィールド抽出）を考慮し、可読性・保守性を優先して専用スクリプトファイルを選んだ。
- **developerの「実装中/確認中」の細分けは不採用**。公式スキーマに現在使用中のツール名を表すフィールドがなく、確認できない挙動を前提にした実装を避けた。
- **手動でのDeveloper→Reviewerパイプラインは今回省略**。Managerが自ら公式ドキュメントを調査し、複数のモックデータで直接動作確認（正常系・空データ・不正JSON・長い説明文の切り詰め・NDJSON形式の妥当性）を行い、2件の実バグ（スキーマ誤認識、truncationのマルチバイト破壊）を自力で発見・修正済みであるため、Reviewerによる重複した検証は費用対効果が低いと判断した。

### 影響
- サブエージェントが実行されるたびに、エージェントパネルに専門用語を使わない日本語の進捗が自動的に表示される。通常のチャット時・サブエージェントが1つも動いていない時は何も表示されない。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この進捗可視化が引き継がれる。新しいサブエージェントを追加する場合は`.claude/statusline-subagent.sh`の対応表に1行追加するだけでよい。
- `status`フィールドの正確な値の一覧、`columns`フィールドの単位（コードポイント数か表示幅か）は公式ドキュメントに明記がなく未確認のまま運用する。実際にサブエージェントを実行した際に想定と異なる表示になった場合は、docs/status-line.mdの記載に沿ってスクリプトの抽出部分を調整する。

---

## D-014: AGENTS.mdの設計原則1〜8はPonytailプラグインへの参照に置き換えず、本文を圧縮するに留める

- 日付: 2026-08-06
- 状態: 採用

### 背景
- ユーザーが個人環境（project001とは別スコープ）にPonytailの公式プラグイン（6 Skills）を含む複数のClaude Codeプラグインを導入しており、AGENTS.mdの設計原則1〜8（出典: DietrichGebert/ponytail）と内容が重複することが判明した。トークン削減のため、AGENTS.mdの該当部分をプラグインへの「参照のみ」に簡略化する案が検討された。

### 決定
- **「参照のみ」への置き換えは不採用、本文は維持し説明文のみ圧縮する**。理由は2点。
  1. project001のサブエージェント4つ（`.claude/agents/{developer,reviewer,planner,researcher}.md`）はいずれも`tools`フィールドに`Skill`を含んでおらず、プラグインのSkillを自動トリガーする手段を持たない。実装・レビューを担うDeveloper/Reviewerが原則を実際に参照できるのは、CLAUDE.mdの`@AGENTS.md`importによって本文がそのままセッションコンテキストへ注入されているためであり、これは`Skill`ツールの有無と無関係に効く経路である。本文を「参照のみ」に置き換えると、この経路が失われる。
  2. 本リポジトリの開発を実際に行っているClaude Code Remote環境そのものに、Ponytailプラグインは導入されていないことをファイルシステム調査（`find`によるponytail関連ファイル・プラグイン設定の探索）で確認した。ユーザーが提示したプラグイン一覧の画面は別のClaude Code環境（デスクトップアプリ等）のものであり、このセッションから参照できるプラグインではない。
- 上記2点により、「4エージェントに`Skill`ツールを追加し、プラグインの更新を自動的にAGENTS.mdへ反映する同期の仕組みを設ける」という代替案も検討したが、同期元のプラグインがこの環境から見えない以上、同期機構を作っても機能しないため不採用とした（判定ラダー1: そもそも必要か）。加えて、この案はCLAUDE.mdの既存方針「project001自体はいずれのツールにも依存しない」とも矛盾する。
- 採用したのは、原則の見出し・番号構成・意味を変えず、冗長な言い回しのみを削る圧縮。`判定ラダー`・`手を抜かない対象`・`ponytail:`コメント運用・`根本原因`はdocs/agents.md・decisions.md・developer.mdから見出し語として名指しで参照されているため、これらの用語・構造は残した。

### 結果
- AGENTS.mdの「設計原則」セクションは2721文字→2478文字（約9%減）。プラグインとの内容重複は解消されないが、意図的に許容する（テキストの独立性・移植性を優先）。

---

## D-015: project001をClaude Code Starter Kit化する（bootstrap.sh・Context7統合・GitHub CLI検出）

- 日付: 2026-08-06
- 状態: 採用

### 背景
- 外部Issue経由で、project001を「Claude Code Starter Kit」として拡張する要求が寄せられた。要求内容には、環境セットアップの自動化、Claude Codeプラグインの既定有効化、Context7のMCP統合、Ponytailを「Claude標準のコンパクション機能」に置き換える提案などが含まれていた。
- Managerが事前に内容を精査した結果、要求の一部はproject001の既存方針（疎結合設計・Optional Dependency・YAGNI）と矛盾する、または事実誤認に基づいていることが判明した。実装前にManagerが採用/縮小/見送りを判断した上でDeveloperへ着手を指示した。

### 決定
Managerが事前判断した4点をそのまま採用した。

1. **Bootstrapは「案内のみ」で実装する**（自動インストールしない）。`.claude/bootstrap.sh`を新設し、`command -v`のみでAgent-Reach/Code Review Graph/Context7/GitHub CLI/Node.js/Python/Playwrightの導入状況を検出する。導入済みはバージョン表示、未導入は用途とインストール例を表示するのみで、実際のインストール・書き込みは一切行わない。`--check`モード（Hook用、厳密に2行の圧縮出力）と引数なしモード（人間向け）の2モードを持ち、常に`exit 0`で終了する。
2. **Claude Codeプラグインはproject scopeで既定有効化しない**。運用手順（有効化する場合のキー構成・スコープ階層）のみをdocs/capability-layer.mdに記載し、実際の有効化はしない。
3. **Context7はCLI版（`ctx7`、`command -v`で検出）のみを統合し、MCP版（`https://mcp.context7.com/mcp`）は統合しない**。理由は、project001のsubagent（`.claude/agents/*.md`）の`tools`フロントマターがallowlist方式でありMCPツールを呼び出せないこと、およびAPIキー管理という新しい秘密情報の運用が発生し疎結合設計に反すること。
4. **Ponytailの「Claude標準コンパクション置き換え」提案は採用しない**。Ponytailは既にAGENTS.mdの設計原則1〜8としてテキストベースで統合済み（D-003、D-014）であり、ツールでも自動コンパクション機構でもない。Issueの「Ponytail→Claude標準コンパクション」という代替記述は、Ponytailの実体（判断ラダー・原則集）とClaude Codeのコンテキスト圧縮機能（PreCompact Hook、D-005）という無関係な2つの概念を混同した事実誤認であり、コード変更は行わずこの決定に事実誤認である旨を記録するに留める。

あわせて、`/init-project`コマンド（`.claude/commands/init-project.md`）を新設した。README.mdの「### 新規プロジェクトでの初期化」節の手順内容を複製せず、「README.mdの該当節を読んで手順を実行せよ」という指示のみを記載し、手順の更新箇所を1箇所（README.md）に保つ。

GitHub CLI（`gh`）は、Capability Layerの「将来のCapability候補」（D-012時点では未統合）からTier1（統合済み）へ昇格させた。Researcherが調査で優先利用する対象とし、専用docsは設けずdocs/capability-layer.mdの表とresearcher.mdの参照のみで完結させた（`gh`固有の詳細手順はGitHub公式ドキュメントに委ね、project001側では複製しない）。Node.js・Python・Playwrightは、D-012と同様に検出のみ（Tier2）に留めた。

### 理由
- **bootstrap.shを新設したことについて**: D-012時点ではCapability検出コマンドを`.claude/settings.json`のSessionStart Hookコマンド文字列に直接列挙していたが、検出対象がAgent-Reach・Code Review Graphの2つからContext7・GitHub CLI・Node.js・Python・Playwrightを加えた7つに増えたことで、1行コマンドが著しく長大化し可読性が落ちた。判定ラダー6段目（1行で書けるか）を再評価した結果「1行では収まらない」という結論になり、D-013で`subagentStatusLine`用のスクリプトファイルを新設した際と同じ判断基準で専用スクリプト化した。新規の外部依存は持たず（`command -v`のみ）、判定ラダー3〜5段目には反しない。
- **自動インストールを採用しなかった理由**: project001はアプリケーションランタイムを持たないテンプレートであり、環境ごとのパッケージマネージャ・権限・OS差異を吸収する自動インストーラを実装すると、それ自体がテンプレートの複雑性・保守負債になる。「あれば使う、なければフォールバック」という既存のOptional Dependency方針（D-009〜D-012）を維持し、案内のみに留めた。
- **プラグインを既定有効化しなかった理由**: project001を雛形としてコピーする派生プロジェクトすべてに、特定ユーザーの個人的なプラグイン選好を強制することになり、テンプレートとしての中立性を損なう。D-014で確認した「project scope」「user scope」の区別を踏まえ、有効化するかどうかは各派生プロジェクト側の判断に委ねる。
- **Context7 MCP版を統合しなかった理由**: docs/context7.mdに記載の通り、`tools`allowlistの制約とAPIキー管理という新しい秘密情報の発生が、project001の疎結合設計（依存を追加しない、いつでも取り除ける）と真っ向から矛盾するため。

### D-012からの変更点
- Capability Layerの検出手段が「`.claude/settings.json`のSessionStart Hookコマンド文字列への直接列挙」から「`.claude/bootstrap.sh`という専用スクリプトへの切り出し（Hookは`bash .claude/bootstrap.sh --check`を呼ぶのみ）」に変わった。
- 統合済みCapability（Tier1）がAgent-Reach・Code Review Graphの2つから、Context7・GitHub CLIを加えた4つに増えた。
- `docs/capability-layer.md`の表構成を「現在統合済み／将来の候補」の2表から、「Tier1（統合済み）／Tier2（検出のみ）／対象外」の3階層に再編し、対象外に分類する理由（MCP Server、Claude Design、Ponytail、Claude Codeプラグイン全般）を明記した。
- `/init-project`コマンドが新設され、README.mdの初期化手順への導線がスラッシュコマンド経由でも提供されるようになった。

### 検討したが不採用の案
- **`agent-reach doctor`等ツール固有のヘルスチェックをbootstrap.shに組み込む案**: `--check`の出力を厳密に2行（Hook用の圧縮出力）に保つ要件と衝突する。ヘルスチェックの詳細は各Agentの自己検出（`docs/agent-reach.md`等）に委ね、`bootstrap.sh`は`command -v`による存在確認のみに徹した。
- **GitHub CLI専用のdocs/gh.mdを新設する案**: `gh`はGitHub公式のCLIであり、project001固有の疎結合設計の説明（依存を持たない・フォールバックする）以外に記載すべき固有の統合仕様がAgent-Reach・Code Review Graph・Context7ほど多くない。docs/capability-layer.mdの表とresearcher.mdの参照のみで要件を満たせると判断し、新規ファイルは追加しなかった。

### 影響
- 新規に`git clone`したユーザーは、`bash .claude/bootstrap.sh`を実行するだけで、project001が想定するOptional Dependencyの導入状況を一括確認できる。未導入のままでもproject001は完全に動作する。
- 今後5つ目以降のCapabilityを追加する場合、`.claude/bootstrap.sh`のリスト・`docs/<tool>.md`・利用するAgent定義ファイル・`docs/capability-layer.md`のTier表・`docs/decisions.md`の5箇所を更新する手順が「新しいCapabilityを追加する手順」としてdocs/capability-layer.mdに明文化された。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、`.claude/bootstrap.sh`・Context7統合・GitHub CLI検出・`/init-project`コマンドが引き継がれる。

---

## D-016: Claude Code 2026運用ナレッジの適用計画を採否判断する

- 日付: 2026-08-10
- 状態: 採用

### 背景
- 外部調査（Researcher）で得た2026年のClaude Code運用ナレッジ約20件について、AGENTS.mdの判定ラダーを適用して採否を判断した。公式ドキュメント（code.claude.com/docs/en/best-practices）を一次確認済み。

### 決定（採用4件）
1. REVIEW.mdへ過剰指摘抑制ルールを追加した。検証で裏付けが取れた指摘でも、正確性・要件充足・セキュリティ・データ整合性のいずれにも影響しないものは`Low`/`Nit`に分類し必須修正としない。セキュリティ・データ整合性に関わる指摘は必須側に残す。
2. CLAUDE.mdの「トークン効率化」節に、CLAUDE.md/AGENTS.mdは`@AGENTS.md`importにより毎セッション読み込まれること、各行を「消すとAIが判断を誤るか」で判定し否なら削除するという削除テストを明文化した（対象はCLAUDE.md/AGENTS.mdのみ、REVIEW.md/docs/はオンデマンドロードのため対象外）。
3. CLAUDE.mdの「セッション運用」節の`/compact`表記を、圧縮時に残したい観点を指定できることが伝わる`/compact <残したい観点>`に修正した。
4. docs/agents.mdのStop Hook不採用理由を、公式が示す「確定的なレビューゲート」パターンとの比較で補強した。

### 決定（見送り、詳述3件）
- **Stop Hook確定的ゲート**: 公式はpass/failを返すスクリプトを前提とした確定的レビューゲートとしてのStop Hookを紹介しているが、(1)project001にはアプリケーションコードがなく機械判定可能なチェック（テスト・ビルド等）が存在しない、(2)REVIEW.mdの敵対的検証はLLMの判断でありスクリプト化できない、(3)8回連続ブロックでClaude Codeが自動オーバーライドする仕様のため「確定的」な保証にもならない、という3つの理由により見送った。
- **Dynamic Workflows / Agent Teams（大量ファンアウト）**: project001の「Managerのみが起動する」統制原則（ルート会話をManagerにする構造で技術的に担保、D-004参照）と正面衝突する。またHook出力はManager（ルートセッション）にのみ注入されファンアウト先のサブエージェントには届かないため、Capability Layer設計（SessionStart Hookの検出結果をManager経由で各Agentへ伝達する構造）が機能しない。project001自体にアプリケーションコードがなく、数十単位に分割できるタスクが存在しないことも理由に加わる。
- **Skillsへの移送**: 移す対象（時々しか関係しない知識）が存在しない。時々しか使わない初期化手順は、既に`/init-project`コマンド＋README.md（D-015参照）で公式パターンと同等の効果を達成済みである。

### 決定（見送り、圧縮7件）
- maxTurns: 公式ドキュメントでオプション名を確認できず、D-013の前例（未確認挙動を前提にした実装はしない）に従い断念した。
- 並列サブエージェント3-5件のスイートスポット: project001はファンアウトしないため適用対象がない。
- サブエージェント過剰生成への警告: AGENTS.md判定ラダー1（そもそも必要か）で既に担保済み、D-004/D-007が実例。
- 調査スコープを絞る運用: docs/research-workflow.md・planner.md・researcher.mdに既に実装済みで追記は重複になる。
- `/btw`: AIが実行できないUI操作であり、CLAUDE.mdの削除テストに落ちる。
- `/usage`統合・Remote MCP・Agent Skillsオープン標準化・Auto Mode: project001に消費するタスクがない、または既にdocs/capability-layer.mdで対象外整理済み。
- 他OSSカタログ型テンプレートとの対比: project001の疎結合最小構成は差別化ポイントであり、記録のみに留める。

### 既存判断の裏付け（変更なし）
- Writer/Reviewer分離: 公式が同じ理由付けをしており、D-004/D-010の裏付けとなることを確認した。
- `@path`import・subagent frontmatter仕様: project001の既存実装が公式仕様と一致していることを確認済み。

### D-006からの変更点
- Stop Hook不採用の理由に、機械判定可能なチェックの不在・8回連続オーバーライドの2点を追加したが、結論（Stop Hookは採用しない）は変えていない。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、レビュー過剰指摘の抑制ルールとCLAUDE.mdの削除テストが引き継がれる。

---

## D-017: PostToolUse Hookによる日本語文体チェックの導入

- 日付: 2026-08-15
- 状態: 採用

### 背景
- AIエージェントが書くdocs/配下の日本語ドキュメントに、効果の中身を書かず「効く」の一語で済ませる、依頼のない対比構文で否定から書き始める、文末表現（ます・です等）が単調に連続する、といった癖が繰り返し出ていた。
- CLAUDE.md/AGENTS.mdへ「この表現は使わない」と書き並べる対処は、会話が長くなるほど指示が薄まり効果が続かないことが外部記事（まつにぃ氏「AIの変な日本語、Hooksで撲滅しているお話」）でも報告されており、本リポジトリでも同種の問題が想定された。

### 決定
- `.claude/hooks/ja-style-check.py`を新設し、PostToolUse Hook（`Write`/`Edit`、`.md`ファイル対象）として`.claude/settings.json`に登録した。
- 検査対象は書き込み単位で完結させ、`Write`時は`tool_input.content`全体、`Edit`時は`tool_input.new_string`のみを見る。ファイルをディスクから読み直して全体を再検査する実装は採用しなかった（後述）。
- NGパターンは`.claude/ja-style-rules.json`に外出しし、パターンごとに書き直し例（グッドパターン）を対で持たせた。該当時は`exit code 2`でエージェント自身へ差し戻し、単語だけの置き換えは禁止、該当文全体の書き直しを求める。
- 初期ルールは「効く」の一語で済ませる曖昧な効果表現、依頼されていない対比構文（否定から入る言い回し）の2件（正規表現による検査）と、文末表現3文以上連続（書き込み単位のテキストを集計するスクリプト判定）の1件に絞った。

### 理由
- Hookは「AIの動作の節目にスクリプトを差し込める」という既存機構であり、判定ラダー（AGENTS.md）の3〜5段目（標準ライブラリ・既存の成熟した手法・インストール済み依存関係で足りるか）に沿って新規外部依存を追加せずPython標準ライブラリのみで実装した。
- 語だけ置換すると同じ問題が別の語形で残る、という元記事の指摘に倣い、文全体の書き直しを求める設計にした。
- 検査対象を書き込み単位に絞ったのは、当初ファイル全体を再検査する実装で試したところ、docs/agents.md・docs/decisions.mdへの追記時に、今回の編集と無関係な既存文中の正当な用法（例:「HooksではなくsubagentStatusLineを採用した」）まで大量に誤検出したため。書き込み単位に絞ることで、今回書いた内容だけが検査対象になる。
- ルールを`.claude/ja-style-rules.json`という単一ファイルへの追記のみで拡張できる構成にし、Skillとして切り出す・ルール登録を自動化する仕組みは今回は見送った（初期ルールが3件のみで、専用の登録経路を作るほどの再利用実績がまだ無いため。件数が増えた場合はSkill化を再検討する）。
- D-016で「project001には機械判定可能なチェックが存在しない」ことをStop Hook不採用の根拠にしたが、日本語文体の検査は正規表現と文末集計という機械判定が可能であり、その前提には抵触しない（対象はコードではなくAI生成ドキュメント）。

### 影響
- docs/agents.mdのHook構成に「PreToolUse/PostToolUseはlint/ビルド連携等、対象コードが存在しないため不採用」という既存の記述（D-005/D-006）と矛盾しないよう、今回追加したPostToolUse Hookの対象は「AIが生成するドキュメント」であり「アプリケーションコードのlint/ビルド」ではない点を明記した。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このHookとルールファイルが引き継がれる。個別アプリ側でルールを追加・調整する場合は`.claude/ja-style-rules.json`を編集すればよい。
- 書き込み単位に絞っても、新規に書いた文が対比構文ルールへ正当な理由で一致するケースは残る（誤検知はゼロにならない）。`exit code 2`は書き込み自体を止めるものではなく、エージェントへの差し戻しに留まるため、最終的に書き直すかどうかはエージェントの判断に委ねる設計とした。

---

## D-018: SubagentStop/SessionEnd Hookによる記録漏れの機械的検知

- 日付: 2026-08-15
- 状態: 採用

### 背景
- D-017（PostToolUse Hookによる日本語文体チェック）の実装・検証を機に、project001の他のHook候補についてユーザーと調査・検討した。docs/tasks.md・docs/progress.md・docs/decisions.mdへの記録は運用ルール（AGENTS.md/CLAUDE.md、docs/agents.mdの参照ドキュメント表）として定めているが、これまでは指示文のみに依存しており、実際に記録されたかを機械的に確認する仕組みは無かった。
- トークン削減の観点で評価した結果、記録漏れを検知しないと次回セッション開始時にManagerが状態を再構築する必要が生じ、CLAUDE.md/AGENTS.mdへのルール追記（常時ロードで確実にコストがかかる）よりも、発火時のみコストが発生するHookベースの検知の方が、再構築コストの回避という点で費用対効果が高いと判断した。

### 決定
- `.claude/hooks/subagent-doc-check.py`を新設し、SubagentStop Hookとして`.claude/settings.json`に登録した。`agent_type`が`developer`の場合のみ、`git status --porcelain -- docs/progress.md`で未コミットの変更有無を確認し、無ければ`exit code 2`で差し戻す。
- SessionEnd Hookを`.claude/settings.json`に新設した。`git status --porcelain`（リポジトリ全体）に変更があれば、docs/progress.md・docs/tasks.mdへの記録漏れがないか確認するようリマインドする。変更が無ければ無反応。
- 提案時に検討した4案（SubagentStop、SessionEnd、PreToolUseによる追記専用ルールの強制、Notificationによる承認ログ）のうち、トークン削減効果が最も大きいと評価した上位2案（SubagentStop、SessionEnd）のみを採用した。PreToolUse案・Notification案は見送り、必要性が具体化した時点で再検討する。

### 理由
- 判定ラダー（AGENTS.md）の3〜5段目に沿い、Python標準ライブラリ（subagent-doc-check.py）とシェル標準コマンド（SessionEnd）のみで実装し、新規外部依存は追加していない。
- SubagentStopの対象を`developer`のみに絞ったのは、docs/agents.mdの参照ドキュメント表が「docs/progress.md: 作業履歴、次回開始位置（Developerが記録）」と明記しており、Reviewer/Planner/Researcherには同じ記録義務が定義されていないため。義務が無いAgentに対して誤って差し戻すことを避けた。
- SessionEndは「未コミットの変更があるか」というリポジトリ全体を対象にした緩い判定に留めた。docs/progress.md・docs/tasks.md単体の差分判定にしなかったのは、SubagentStopで既にDeveloper単位の判定を行っており、SessionEndは「PreCompactを経ずに終了するケースを補完する最後の安全網」という位置づけで十分なため（判定ラダー1: そもそも厳密な判定が必要か、を再確認した結果）。
- PreCompact不採用理由（D-016、機械判定可能なチェックが存在しない）と同様、SubagentStop/SessionEndも`git status`という機械判定可能なチェックであり、Stop Hook不採用の前提（LLMの判断が必要で機械判定できない）には抵触しない。

### 影響
- docs/agents.mdのHook構成節に、SubagentStop/SessionEndの説明を追加した。「以下2つのHookを導入している」という古い記述（SessionStart/PreCompactのみを指す）を、Hook種別が増えたことに合わせて修正した。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この2つのHookが引き継がれる。
- SubagentStopはDeveloper subagentがdocs/progress.mdへ一度も書き込まずに完了しようとした場合、常に差し戻される。意図的にdocs/progress.md更新が不要な軽微な作業（例: タイポ修正のみ）でも差し戻される可能性があり、その場合はAgent側の判断で該当作業の記録を簡潔に追記して解消する運用とする。

---

## D-019: Claude Code公式仕様に基づく開発基盤監査、taste-skill 13件の撤去

- 日付: 2026-08-15
- 状態: 採用

### 背景
- project001を「Claude Codeが効率的・安定的・低コストで開発を進められる再利用可能な開発基盤」として保つため、ユーザーの依頼によりCLAUDE.md/AGENTS.md/.claude/agents/.claude/skills/hooks/docs/Capability Layer/レビューフローを、2026年時点のClaude Code公式仕様（CLAUDE.md/rules/skills/sub-agents公式ドキュメント）と2026年の実践知見に照らして監査した。
- 監査の結果、最も実害が明確な問題として、D-017で「確認プロセスを経ない例外追加」と自認したまま残っていた`Leonxlnx/taste-skill`由来のSkill13件（デザイン/UI系、計6670行）が挙がった。docs/agents.md「採用しなかったAgent」節が既に「project001自体はアプリケーションコードを持たないテンプレートであり、UIレビューの対象が存在しない」と明記しており、UI専用Skillの導入はこの既存方針と矛盾していた。1行説明は毎セッション常時コンテキストに乗っており、CLAUDE.mdの「Skillsは再利用可能な具体的ワークフローが確認された時点で追加する」という原則にも反していた。

### 決定
- `.agents/skills/`（Skill本体13件）、`.claude/skills/`（Claude Code向けシンボリックリンク13件）、`skills-lock.json`を削除した。
- CLAUDE.mdの「Skills / Capability Layer」節を「現時点で導入しているSkillはない」に戻し、今回の撤去の経緯を1文で追記した。
- 監査で洗い出した残りの論点（Auto Memoryとdocs/の役割分担、`.claude/rules/`の採用要否、README.mdの記述更新、Capability Layer未使用状態の扱い）は、この決定とは切り離し、別途ユーザーとの合意を経てから対応する。

### 理由
- 判定ラダー（AGENTS.md）1段目（そもそも必要か）に立ち返ると、project001自体にUIコードが存在しない以上、UI専用Skillは「再利用可能な具体的ワークフロー」の確認を経ておらず、そもそも必要性が無い。
- 公式ドキュメントで確認した`disable-model-invocation: true`（常時ロードされる説明文をコンテキストから外し、`/skill-name`による手動起動のみ残す設定）という選択肢も検討したが、個別アプリ側で必要になった時点で`npx skills add`により再導入すればよく、「project001自体はいずれのツールにも依存しない」という既存のCapability Layerの原則（本ファイル「Skills / Capability Layer」節冒頭）とも整合するため、無効化ではなく削除を選んだ。
- 半端に残す（無効化のみ）と、次回以降のセッションでなぜ13件のSkillディレクトリが存在するのに使われていないのかという文脈が失われやすく、削除の方がリポジトリの状態と方針の記述が一致し理解しやすい。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてから、taste-skill 13件が引き継がれなくなる。個別アプリでUI関連のSkillが必要になった場合は、そちらのリポジトリで`npx skills add`等により追加する。
- CLAUDE.mdの記述が「現時点で導入しているSkillはない」という初期状態（D-001〜D-016時点の記述）に戻る。D-017・D-018の本文（PostToolUse/SubagentStop/SessionEnd Hookの追加）自体は撤回しない。

---

## D-020: `.claude/rules/`（path-scoped rules）の不採用

- 日付: 2026-08-15
- 状態: 却下

### 背景
- T-020の監査で、ユーザーから「path-specificなルールを`.claude/rules/`へ分離できないか」という観点が挙がった。公式ドキュメント（code.claude.com/docs/en/claude-md）を確認した結果、`.claude/rules/`配下のファイルは`paths:`frontmatterを付けて初めてファイル種別ごとの条件ロードになり、`paths:`が無い場合はCLAUDE.mdと同一優先度で毎セッション無条件にロードされる（トークン削減効果はゼロ）ことが判明した。

### 決定
- project001自体には`.claude/rules/`によるpath-scoped ruleを新設しない。CLAUDE.md/AGENTS.mdの現行構成を維持する。

### 理由
- path-scoped rulesが効果を発揮するのは「ファイルの種類・ディレクトリによって適用したい規約が変わる」ケース（例: `src/api/**/*.ts`にだけ適用する規約）である。project001自体はアプリケーションコード（src/等）を持たないテンプレートであり、ファイル種別ごとに振る舞いを変えたい対象が存在しない。
- 現行のCLAUDE.md（37行）+AGENTS.md（52行、import経由で常時ロード）は公式の目安（200行未満）に対して十分小さく、`.claude/rules/`へ分割する動機（CLAUDE.mdの肥大化回避）自体が現時点で発生していない。
- 仮に`.claude/rules/`へ移してもpathsを指定しない限り常時ロードされる点は変わらないため、ファイルを分けるだけの移動はAGENTS.mdの判定ラダー1段目（そもそも必要か）で不採用となる。

### 影響
- 個別アプリのリポジトリ（project001から作成された派生リポジトリ）では、フロントエンド/バックエンド等でディレクトリごとに規約が異なるケースが生じうるため、そちら側で`.claude/rules/`（`paths:`指定）を採用する余地は残る。本決定はproject001自体（テンプレート）に限定する。

---

## D-021: Capability Layer Tier1（Agent-Reach/Code Review Graph/Context7/GitHub CLI）をTier2へ格下げ

- 日付: 2026-08-15
- 状態: 採用

### 背景
- T-020の監査で、Capability LayerのTier1に分類していた4件（Agent-Reach/Code Review Graph/Context7/GitHub CLI）が、本セッションを含む複数セッションのSessionStart Hook出力で一度も`available`にならず、常に`unavailable`のままであることを確認した。docs/context7.mdは元々ctx7のサブコマンド仕様を「本環境では未検証」と明記しており、Code Review Graphのみ過去（T-012のレビューループ）に一時的な検証用インストールで動作確認された実績はあるが、恒常的な統合環境ではない。
- Tier1（統合済み: Agentの振る舞いに組込み）は、`.claude/agents/*.md`・REVIEW.md本文に「利用可能なら優先利用する」という条件分岐を常設することを意味する。一度も検出されない状態でこの分岐を持ち続けることは、実質的に使われない条件分岐の記述コストだけが残る状態であり、Tier1の定義（統合済み）と実態が乖離していた。

### 決定
- Agent-Reach/Code Review Graph/Context7/GitHub CLIの4件をTier1からTier2（検出のみ、Agentの振る舞いには未組込み）へ格下げした。
- `.claude/agents/planner.md`・`developer.md`・`reviewer.md`・`researcher.md`・REVIEW.md・docs/research-workflow.mdから、これら4件を「利用可能なら優先利用する」とする条件分岐の記述を削除し、Read/Grep/Bash/WebFetch/WebSearchのみによる調査・実装・レビュー手順に統一した。
- docs/agent-reach.md・docs/code-review-graph.md・docs/context7.mdは削除せず、「現在Tier2」の注記を追加した上でTier1昇格時の利用方針として保持した（検出・動作確認が取れた時点で`.claude/bootstrap.sh`を変更せずそのままTier1へ復帰できる）。
- CLAUDE.md・docs/capability-layer.mdのTier表記述を実態に合わせて更新した。

### 理由
- AGENTS.mdの判定ラダー1段目（そもそも必要か）に照らすと、一度も動作確認が取れていない外部ツールへの「優先利用」という振る舞いをAgent定義ファイルに常設し続けることは、実態を伴わない記述であり、次回以降のセッションが「統合済みだが実際には使われたことがない」という状態を誤って前提にするリスクがある。
- `.claude/bootstrap.sh`の検出対象・docs/agent-reach.md等の内容自体は変更していないため、削除ではなく格下げにより、実際にツールが検出された場合の再統合コストを最小に保っている（Tier2→Tier1の昇格手順は既存の「新しいCapabilityを追加する手順」をそのまま使える）。
- Node.js/Python/PlaywrightがTier2である理由（project001自体に消費するタスクが無い）とは性質が異なるため、docs/capability-layer.mdのTier2表に「動作未検証」という別理由を明記し、混同を避けた。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてで、Agent-Reach等4件は当面Tier2（検出のみ）として引き継がれる。個別アプリのリポジトリで実際に動作確認が取れた場合は、そちら側でTier1へ昇格させてよい。
- Researcherの調査フローは、Agent-Reachが検出・動作確認されるまでWebFetch/WebSearchのみで完結する（docs/research-workflow.md）。Developer/Reviewerの影響範囲確認・API仕様確認も同様にRead/Grep/Bashのみで完結する。
- REVIEW.mdの検証パスの記述から、Code Review Graphの具体的なサブコマンド名（`impact --files`等）への言及がなくなった。詳細はdocs/code-review-graph.mdに委譲する。

---

## D-022: Auto Memoryとdocs/tasks.md・progress.md・decisions.mdの役割分担を明記

- 日付: 2026-08-15
- 状態: 採用

### 背景
- T-020の監査で、Claude Code本体の標準機能であるAuto Memory（Claude自身が訂正・学習をリポジトリごとのMEMORY.mdへ自動記録し、毎セッション自動ロードする機能）の存在を確認した。project001のdocs/tasks.md・docs/progress.md・docs/decisions.mdは、Auto Memory登場前のD-001（2026-08-03）に由来する独自運用であり、両者の役割分担がどこにも明記されていなかった。
- 役割分担が未整理のままだと、将来Auto Memoryがdocs/配下と類似の内容を書き始めた場合に二重管理になったり、逆にdocs/配下の運用をAuto Memoryへ安易に委譲する誤った変更が行われたりするリスクがある。

### 決定
- docs/agents.mdに「Auto Memoryとの役割分担」節を新設し、Auto Memory（個人的な気づき・訂正の自動学習、非公式）とdocs/配下（タスク状態・作業履歴・設計判断の公式な記録、PR経由でコミット、REVIEW.md・Hookで一貫性を補強）の違いを表で明記した。
- docs/配下の運用を縮小・簡略化する変更、Auto Memoryへ委譲する変更は行わない。

### 理由
- Auto MemoryはClaudeの裁量で書かれる非公式なノートであり、project001が重視するチーム共有・PRレビューを経た正式な記録（AGENTS.md設計原則、REVIEW.mdの敵対的検証）とは性質が異なる。置き換えると、設計判断の追跡可能性（docs/decisions.mdのADR形式）が失われる。
- 一方で役割分担を明記しないままだと、次回以降のセッションがAuto Memoryとdocs/配下のどちらに何を書くべきか迷い、重複記録や記録漏れが生じる可能性がある。ドキュメント新設ではなく既存のdocs/agents.mdへの節追加に留めたのは、判定ラダー（AGENTS.md）2段目（既存コードベースに同等の実装があるか）に照らし、新規docsファイルを作るほどの分量ではないため。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この役割分担の説明が引き継がれる。
- T-020監査で洗い出した4項目（taste-skill撤去、README修正、`.claude/rules/`不採用記録、Capability Layer Tier2格下げ、Auto Memory役割分担）がすべて完了した。

---

## D-023: Claude Code向けMarkdownの構成・書式監査

- 日付: 2026-08-15
- 状態: 採用

### 背景
- ユーザー依頼により、CLAUDE.md/AGENTS.md/REVIEW.md/docs/.claude/agents配下の全Markdownを対象に、Claude Codeの理解精度とコンテキスト効率を両立する構成・書式へ最適化する監査を行った。設計思想（Capability Layer、Agent構成、開発フロー、レビュー方針）は変更せず、構成・内容・書式のみを対象とした。

### 決定
- docs/agents.mdの「Hook構成」節を、Hookごとの`###`見出し＋箇条書きへ再構成した。従来は1つのHookの説明が1行の長大な文（最長で約800文字）に詰め込まれており、走査性が低かった。情報の削除は行っていない。
- CLAUDE.mdの「Skills / Capability Layer」節前半（Capability Layerの説明）を、密な1段落から箇条書きへ再構成した。Skills段落は、撤去理由の詳細（D-019に既出）をCLAUDE.md側では圧縮し「D-019参照」に委ねた。
- REVIEW.mdの「検証パスの判定」節、過剰指摘抑制ルールの長大な1段落を、除外条件3件を箇条書きに分離する形へ再構成した。
- docs/agent-reach.md・docs/context7.mdの「フォールバック方針」から、docs/capability-layer.mdの一般原則（「あれば使う、なければ今まで通り」）と同一文言の再掲を削除し、参照に一本化した。

### 理由
- 公式ドキュメント（code.claude.com/docs/en/claude-md）の「Structure: use markdown headers and bullets to group related instructions. Claude scans structure the same way readers do」という指針に沿い、密な段落を見出し・箇条書きへ分解した。情報量は変えず、走査性のみを改善している。
- CLAUDE.md/AGENTS.mdは公式の目安（200行未満）に対して合計94行と十分小さく、行数増加を許容しても常時ロードコストは低い。一方docs/agents.md等はオンデマンドロードのため、多少の行数増加より理解精度を優先した。
- docs/agent-reach.md・docs/context7.mdの重複削除は、`.claude/skills`のような常時ロードコストではなく、複数ファイルに同一文言が存在すること自体の保守コスト（ツール仕様変更時に複数箇所を直す必要がある）を下げる目的。docs/capability-layer.mdの「ツール固有の仕様は専用docsにのみ記載し複製しない」という既存原則の逆方向（一般原則をツール固有docsに複製しない）にも当てはまる。
- docs/decisions.md・docs/progress.md・docs/tasks.mdは対象から除外した。新しいエントリを末尾/先頭に追加する記録フォーマットの継続的な履歴であり、過去エントリの書式統一・要約はリポジトリの意思決定・作業履歴の追跡可能性を損なうため。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、再構成後の書式が引き継がれる。
- 情報の追加・削除は最小限（重複2箇所の削除のみ）で、既存の参照リンク・D番号・ファイルパスはすべて維持している。

---

## D-024: 外部リポジトリ調査（HirotakaAdachi/Rogue）から3項目を移植する

- 日付: 2026-08-26
- 状態: 採用

### 背景
- ユーザー依頼により、外部の個人開発リポジトリ（HirotakaAdachi/Rogue。ブラウザ単体で動く47,000行超の単一JSファイルからなるローグライクゲームで、AIエージェント向けの独自CLAUDE.md運用マニュアルを同梱している）を調査し、project001（本リポジトリ）へ転用できる設計思想を洗い出した。
- 調査の結果、6項目の転用候補（grepアンカー式ナビゲーション／Design Memos／ABSOLUTE RULE昇格／Common Pitfalls／通称⇄コード名対照表／単一ファイル運用）のうち、project001の既存原則（責務分離、重複排除、YAGNI）と矛盾せずそのまま組み込める3項目（○評価）のみをユーザーが選定した。単一ファイル運用（責務分離と矛盾）は不採用、Design MemosとCommon Pitfalls（△評価。新設ファイルの是非が既存の重複排除方針と要検討）は今回のスコープ外とした。

### 決定
- **AGENTS.md 設計原則9を新設**: 「大規模ファイルは検索可能な状態を保つ」。1ファイルが肥大化した際に`SECTION:`等のアンカーコメントを埋め込み、ドキュメントからの参照はアンカー文字列へのgrepコマンドで記載する規約を追加した。project001自体はアプリケーションコードを持たないが、本テンプレートから作成される個別アプリ（コピー先）が将来大規模化した際の実装規約として機能する。
- **REVIEW.mdに「同一種別の指摘が繰り返された場合の恒久化」を新設**: Reviewerが同種の`CONFIRMED`指摘を2回以上下した場合、対象アプリのCLAUDE.md相当の常時ロード文書へ`⚠️ 絶対ルール`のような形で恒久化するようManagerへ提案する運用を追記した。Reviewerのtoolsはread-onlyであり、ドキュメント編集はManager/Developerへの提案という形に留めた。
- **`.claude/agents/developer.md`に対照表運用の1行を追加**: ユーザーが機能・要素を指す通称と内部の識別子が異なる場合、対照表を1箇所に集約し複数箇所への重複記載を避ける方針を明記した。

### 理由（検討した代替案）
- **Design Memos（非自明な設計意図の集約）の新規ファイル化は今回不採用**: docs/decisions.mdは時系列の意思決定履歴（ADR形式）であり、Rogueの「機構ごとの順序依存・不変条件」を集約するDesign Memosとは役割が異なる。新設ファイルはD-006/D-008で問題視された重複再発のリスクがあり、ユーザーが今回のスコープから明示的に除外した。
- **Common Pitfalls（脆弱箇所一覧）の新規ファイル化も今回不採用**: 同じくユーザーが今回のスコープから除外した。将来必要になった場合はdecisions.mdとの役割分担を個別に検討する。
- **単一ファイル・ビルドレス運用は不採用**: AGENTS.md設計原則5「責務は分離する」と正面から矛盾するため、転用候補から明確に除外した。
- **AGENTS.md原則9をponytail原則と同列に統合する案は不採用**: 出典表記を分け、ponytail外の知見であることをAGENTS.md本文とdecisions.md（本エントリ）の双方に明記し、原典の帰属を誤認させないようにした。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この3項目（大規模ファイルのgrepアンカー規約、指摘恒久化の運用、通称対照表の実装規約）が引き継がれる。
- project001自体にはアプリケーションコードが無いため、AGENTS.md原則9とdeveloper.mdの対照表運用は実際に適用される場面が無いが、テンプレートとしての規約提供という役割（D-002）には合致する。
- trading-system等の個別アプリリポジトリへは、本決定は適用しない（ユーザー指示により今回のスコープ外）。

---

## D-025: project001へのDesign基盤統合（Designer Agent常設・Frontend Designプラグイン有効化）

- 日付: 2026-08-26
- 状態: 採用

### 背景
- ユーザーから、project001を「要件整理→UX設計→デザイン設計→実装→UI/UXレビュー→修正→検証」まで一貫して品質管理できる開発基盤にする依頼を受けた。表面的な要素禁止ではなく、制作物の種類・目的・ユーザー・利用環境・ブランドからデザインを判断する仕組みを求める内容だった。
- 調査の結果、本依頼はproject001の既存決定2点と衝突する可能性が高いことが判明した。(1) docs/agents.md「採用しなかったAgent」節の「project001自体にはUIコードが存在しないためUI固有Agentは個別アプリ側で追加する」という方針、(2) CLAUDE.md/docs/capability-layer.mdの「Claude Codeプラグインはproject scopeで既定有効化しない」という方針。
- 公式機能調査で、Anthropic公式のFrontend Designプラグイン（`claude-plugins-official`マーケットプレイス、フロントエンド実装前に目的・トーン・制約・差別化を検討させる4項目フレームワークを提供）と、Claude Design（`/design`。claude.aiアカウント・Pro/Max/Team/Enterpriseプラン・対応プラットフォームを要件とするアカウントレベルの機能）の存在を確認した。Artifacts公式ドキュメントには「デザイントークンをCLAUDE.md等に記録し、Claudeがそれを自身の既定より優先して参照する」という型が既に存在することも確認した。
- 上記2つの競合点についてユーザーに確認したところ、いずれも既定の推奨（見送り・非導入）を上書きし、Designer Agentの常設とFrontend Designプラグインの導入を明示的に選択した。

### 決定
- `.claude/agents/designer.md`を新設し、Planner/Developer/Reviewerと並ぶ常設Agentとして採用した。tools: Read/Glob/Grep/WebFetch/WebSearch、model: opus（Plannerと同じ理由）。
- `.claude/skills/design-principles/SKILL.md`を新設した。制作物の種類別デザイン優先順位、「AIっぽい」要素を理由なく使わない判断基準、独創性、モーション設計、UI状態設計、既存デザインシステムとの整合をまとめた。designer/developer/reviewerが参照する。
- `docs/design-workflow.md`を新設した。Designerの起動条件、Planner→Designer→Developer→Reviewerの流れ、Claude Design（`/design`）連携とフォールバック、Frontend Designプラグインの位置づけ、Reviewerが確認するデザイン品質基準を記載した。
- `.claude/settings.json`の`enabledPlugins`に`frontend-design@claude-plugins-official`を追加し、project scopeで有効化した。
- AGENTS.mdワークフロー、docs/agents.mdのAgent構成表・モデル構成表・「採用しなかったAgent」節、REVIEW.mdのレビュー観点、`.claude/agents/reviewer.md`、docs/capability-layer.mdの対象外表、CLAUDE.mdのSkills/Capability Layer節、README.mdを、それぞれ整合するよう更新した。

### 理由
- Designer Agentを常設する判断は、AGENTS.md判定ラダー1段目（そもそも必要か）の観点では、project001自体にUIタスクが無い以上本来は個別アプリ側で追加する方が整合的だが、ユーザーが「project001をコピーした時点でDesign基盤が使える状態にしたい」という明示的な意図でこれを上書きした。テンプレートとしての即応性を優先する判断であり、判定ラダーとの矛盾はユーザーの明示的判断として記録する。
- Frontend Designプラグインの導入は、判定ラダー2〜3段目（既存コードベース・成熟した外部実装で足りるか）に照らすと、独自のAIっぽさ回避ロジックを再実装するより公式プラグインを使う方が合理的という判断とも一致する。「project scopeで既定有効化しない」という既存方針自体は、Frontend Design以外の一般プラグインには引き続き適用する（本決定は限定的な例外として記録する）。
- Claude Design（`/design`）はアカウントレベルの機能でありproject001側にインストール対象が無いため、統合とは「呼び出し方法とフォールバックを文書化すること」を意味する。Designer Agent自体は account要件を満たさない環境でも仕様書ベースで機能するよう設計し、通常開発を止めない要件を満たした。
- デザイン判断原則は`.claude/skills/design-principles/SKILL.md`の1箇所に集約し、CLAUDE.md・designer.md・docs/design-workflow.mdには要点の参照のみを置いた（重複記載を避ける、CLAUDE.mdの削除テストにも合致する）。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、Designer Agent・design-principles Skill・design-workflow.md・Frontend Designプラグインの有効化設定が引き継がれる。
- project001自体の開発でDesignerが起動することは通常ない（UIコードが存在しないため）。個別アプリのリポジトリでUI実装タスクが発生した際に機能する。
- docs/agents.md「採用しなかったAgent」節のUI項目は、不採用から採用へ判断を変更した旨を追記し、変更履歴として残した（削除はしていない）。
- Frontend Designプラグインの有効化により、project001を初めて信頼するタイミングでこのプラグインが自動的に導入される（Claude Code公式仕様「チームメンバーがリポジトリフォルダを信頼した時点で、`.claude/settings.json`が指定するプラグインが導入される」動作に基づく）。

---

## D-026: 2026年9月最新環境への見直し（Concise出力スタイル・デザイントークン往復）

- 日付: 2026-09-10
- 状態: 採用

### 背景
- ユーザーから、project001全体を2026年9月時点の公式最新情報・実践知見に合わせて見直す依頼を受けた。用途はWebサイト/アプリ開発プロジェクトの基盤、目的はトークン効率の良い運用整備。Claude Designで先に作り込みDeveloperへ渡す流れがあるため、齟齬防止も求められた。
- 公式Weekly digest（2026-w22〜w34）を確認した結果、直近3週間（w35〜w37）に新しい公開ダイジェストは無く、T-026時点（w34まで）の調査から大きな変化は無いことを確認した。
- 実質的な更新点として2つを特定した。(1) 2026年8月の公式追加である`outputStyle: "Concise"`（前置き・振り返りを省き結果から述べる出力スタイル、詳細品質は落とさない）が`.claude/settings.json`で設定可能であり未設定だった。(2) Claude Design/Artifactsが「対象リポジトリのCLAUDE.md等に記録されたデザイントークンを自身の既定より優先する」という公式の型を、docs/design-workflow.mdが具体的な運用手順（誰が・いつ記録するか）まで落とし込めていなかった。

### 決定
- `.claude/settings.json`に`"outputStyle": "Concise"`を追加した。
- CLAUDE.mdの「トークン効率化」節に1行追記した。
- docs/design-workflow.mdに「デザイントークンの往復」節を新設し、`/design`呼び出し前の既存トークン確認、新規トークン採用時の記録主体（Designerが仕様書に明記→Developerがアプリ側CLAUDE.mdへ記録）を明記した。
- `.claude/agents/designer.md`・`.claude/agents/developer.md`にそれぞれ対応する1行を追加した。

### 理由
- `outputStyle: Concise`は公式機能であり、同じ作業品質を保ったまま応答の前置き・振り返りを削るため、判定ラダー3段目（標準機能で足りるか）に沿う。project001が重視するトークン効率化の方針（CLAUDE.md「トークン効率化」節）と直接合致する。
- デザイントークン往復の記録主体をDeveloperにしたのは、Designer（`.claude/agents/designer.md`のtools: Read/Glob/Grep/WebFetch/WebSearch）がファイル書き込み権限を持たないため。責務分離（AGENTS.md設計原則5）を保ったまま、Designerの仕様書に値を明記→Developerが記録という役割分担にした。
- Weekly digestで新しい大きな変更が確認できなかったため、Agent構成・Hook構成・Capability Layer等の既存決定（D-001〜D-025）は据え置いた。不要な作り直しを避ける判定ラダー1段目の適用でもある。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、Concise出力スタイルとデザイントークン往復ルールが引き継がれる。
- 個別アプリのリポジトリでDesigner→Developerの流れを使う際、CLAUDE.mdへのトークン記録が実際に機能する。project001自体はUIコードを持たないため適用対象がない。

---

## D-027: `CLAUDE_CODE_SUBAGENT_MODEL`によるModel Routing上書きリスクの文書化

- 日付: 2026-09-10
- 状態: 採用

### 背景
- ユーザー依頼によるproject001とClaude Codeの互換性解析で、`model:`フロントマター（Agent別固定）と現行仕様の整合を確認した。ベアエイリアス（`opus`/`sonnet`）自体は現行も有効だが、`CLAUDE_CODE_SUBAGENT_MODEL`環境変数が設定された実行環境では、これが全Agentの`model`フロントマターより優先され、docs/agents.mdのModel Routing表（Planner/Designer=opus、Developer/Reviewer/Researcher=sonnet）が無効化されることが判明した。project001側からはこの環境変数の有無を検知・警告できない。

### 決定
- docs/agents.mdの「モデル構成（Model Routing）」節に、この環境変数によるModel Routing無効化リスクを1段落で追記した。

### 理由
- 実際に修正できない外部要因（ユーザー環境の環境変数）であるため、コードやHookでの対処ではなく、Model Routingが意図通り機能しない場合の一次切り分け先としてドキュメント化するに留めた。判定ラダー1段目（そもそも対処が必要か）に照らし、検知の仕組みを新設するのは過剰と判断した。

### 影響
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、この注記が引き継がれる。挙動そのものは変わらない。

---

## D-028: 高性能モデル前提でのAgent定義の指示削減（外部記事の知見を反映）

- 日付: 2026-09-12
- 状態: 採用

### 背景
- ユーザーから、OpenAIの記事「Rethinking skills and prompts for GPT-6 Astra」の内容とこれまでの履歴を踏まえてSkill・AGENTS.mdを見直す依頼を受けた。
- 記事の主張は「モデルが高性能化した結果、従来の手取り足取りの指示が逆効果になる」というもので、具体的には(1)Skill記述は使用場面が分かる範囲で可能な限り短く、(2)複雑なSkillはルーター化して段階的に開示、(3)AGENTS.mdの参照指示は場面限定にする、(4)着手前に完了条件を定義する、の4点を挙げている。
- 記事はOpenAI/GPT-6 Astra向けであり、Claudeへ直接適用する根拠にはならない。ただし(1)(3)(4)は、project001が既にCLAUDE.mdへ持つ削除テスト（「消すとAIが判断を誤るか」で判定し、否なら削除する）およびT-024監査で確認した「Claudeが既定で行うことの指示は、他の指示の遵守率を下げる」という知見と結論が一致するため、その重なる範囲に限って採用した。

### 決定
- ツールのallowlistで機械的に担保済みの指示3行を削除した。planner.md「計画はコード変更を行わず」、reviewer.md「コードの変更は行わない」、designer.md「コードの実装は行わない」の3件で、いずれも当該Agentの`tools`にEdit/Writeが無く物理的に実行不可能。
- Claude Code本体のシステムプロンプト既定と重複する2行をdeveloper.mdから削除した。「コメントは基本的に書かず、非自明な理由がある場合のみ短く残す」（ハーネス側が同内容を既定で持つ）と「実装後は可能な範囲でテストやビルド・型チェックを実行し」（記事が名指しした典型例。現行モデルは自発的に実行する）。
- planner.mdの出力構成に「完了条件」を追加した（記事の principle 4）。
- AGENTS.mdのドキュメント参照行を、列挙型から場面限定型（「Agent追加・モデル選定・Hook挙動の確認は docs/agents.md、Researcher起動判断は…」）へ書き換えた（記事の principle 3）。

### 理由（採用しなかった項目とその理由も含む）
- developer.md「破壊的な操作は行わない」は、ハーネス側の安全規則と重複するが残した。削除して事故が起きた場合の損失が非対称に大きく、1行のコストに見合わない。
- researcher.md「コードの変更は行わない」も残した。researcherは`tools`にBashを持つため、機械的な担保が他3Agentほど強くない。
- Skill（design-principles）は変更しなかった。descriptionは約110字で使用場面（designer/developer/reviewerがUI/UX実装を伴うタスクを扱う際）が明示されており記事の基準を満たす。本文も約50行でルーター化を要する規模ではなく、Skillは1件のみのためdescription同士の矛盾も発生しない。
- AGENTS.mdの設計原則1〜9は変更しなかった。原則1「理解してから作る」は判定ラダーの前提として機能し、原則7の「全呼び出し元を確認し共有関数側を一度だけ直す」はハーネス既定に含まれないproject001固有の指示であるため。

### 影響
- Agent定義ファイルが計4行減り、常時ロードされるAGENTS.mdの参照行が場面限定になった。Agentの実際の権限（tools）は変更していないため、振る舞いの範囲は変わらない。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、Plannerが完了条件を先に定義する運用が引き継がれる。

---

## D-029: Agent別のeffort固定（外部記事のトークン節約知見からの採用1件）

- 日付: 2026-09-23
- 状態: 採用

### 背景
- ユーザーから、Miles Deutscher氏の記事「Never Hit Claude Usage Limits Ever Again」（トークン使用量の削減策を5ステップで解説）の内容を踏まえた見直し依頼を受けた。
- 記事の提案11項目をproject001の現状と照合した結果、10項目は既存の仕組みで担保済み、根拠不足、または採用すると品質を損なうものだった（次項に記録）。未対応だったのは`effort`（推論にかける労力の設定）の1点のみで、記事では「ほとんどのタスクでLowを選べる」として挙げられていた。
- Claude Code公式のsubagent frontmatterリファレンスで`effort`が正式フィールド（`low`/`medium`/`high`/`xhigh`/`max`、未指定時はセッション設定を継承）であることを確認した。

### 決定
- Planner・Designer・Reviewerに`effort: high`、Researcherに`effort: medium`を設定した。Developerは未指定（セッション継承）のままとした。
- docs/agents.mdのModel Routing表にeffort列を追加し、各値の選定理由を記載した。

### 理由
- D-007で`model`を役割ごとに固定した理由（セッション状態次第で品質・コストが意図せず変動し、役割に応じた予測可能性が崩れる）は`effort`にもそのまま当てはまる。未設定のままではModel Routingの前提が片側だけ担保されていない状態だった。
- Planner/Designerは設計判断の質が下流の手戻りに直結するため`high`。Reviewerは敵対的検証が仮説の列挙と反証の積み重ねで成立するため`high`。Researcherは成果の質が推論の深さより情報源の広さに依存するため`medium`とし、記事が推奨する`low`までは下げなかった（REVIEW.mdでReviewerが調査品質を検証する前提があり、信頼性評価の判断力を落とすリスクを取らない）。
- Developerを未指定にしたのは、担当タスクが軽微な修正から複雑なデバッグまで幅広く、単一の値が適さないため。Managerが起動時に指定できる余地を残した。

### 採用しなかった記事の提案とその理由
- **計画フェーズに安価なモデル（Haiku）を使う**: project001のPlanner=opusはD-007の「設計判断の質が下流の手戻り量に直結する」という根拠に基づく。記事自身も「計画に時間をかけるほど総コストが下がる」と主張しており、安価なモデルを勧めているのはブレインストーミング工程に対してである。project001に該当工程は存在しない。
- **Plan Modeの活用**: Plannerサブエージェントは調査過程をメインセッションから隔離するため、同じ計画作業でもコンテキスト消費の面で有利。D-004のオーケストレーション構成を変える理由にならない。
- **新規チャットへの切り替え・メガプロンプトによる引き継ぎ**: CLAUDE.mdの`/clear`運用とdocs/progress.md「次回開始位置」で担保済み。後者はリポジトリに永続化される分より確実。
- **Instructions.md + Memory.mdの自作**: CLAUDE.md・docs/配下とAuto Memoryの役割分担（D-022）で担保済み。記事の手法はClaude Code標準のAuto Memoryを手作業で再実装するものにあたる。
- **Conciseスタイル**: D-026で設定済み。
- **Claude Designは別枠の使用量である**: 公式ドキュメントで裏付けが取れなかった。D-013の前例（確認できない挙動を前提にした実装はしない）に従い、docs/design-workflow.mdへは記載しない。
- **拡張思考を常時オフにする**: Reviewerの敵対的検証・Plannerの設計判断の質を直接損なうため不採用。effortを役割ごとに固定する今回の対応が、同じ目的をより安全に満たす。
- **他社の安価なモデル（Kimi/DeepSeek等）の併用**: Claude Code向けテンプレートであるproject001の範囲外。
- **追加クレジット購入・`/usage`での使用量確認**: アカウント運用の話でリポジトリ設定の対象外。
- **Skillによる繰り返し作業の自動化**: CLAUDE.mdのSkills方針（再利用可能な具体的ワークフローが確認された時点で追加する）で既に扱っている。

### 影響
- Planner/Designer/Reviewerは、セッションのeffort設定に関わらず一定の推論深度で動作する。Researcherは中程度に固定され、セッションが高effortの場合は消費が下がる。
- 本リポジトリを雛形として作成される新規プロジェクトすべてに、このeffort設定が引き継がれる。

---

## D-030: 公式ベストプラクティスとの照合による原則・運用の見直し

### 背景
- ユーザーから、Claude Code最新情報に基づくアプリ開発指針の改善とトークン消費の削減、必要に応じたSkill・プラグイン導入の精査、およびproject001の原則自体の見直しを依頼された。
- 公式ドキュメント（best-practices / costs / code-review / plugins/code-intelligence）と公式プラグインsecurity-guidanceのREADMEを確認し、project001の現状と照合した。

### 決定
- **AGENTS.md設計原則3の修正**: 「廃止するパスはマイグレーションではなく削除する」を、コードパスに限定した。永続データ（DBスキーマ・保存形式）と外部から参照される公開API・URLは、データ損失・リンク切れを防ぐ移行を行う例外とした。
- **完了条件「動作確認済み」の具体化**: 合否を返すチェックを実行し、実行内容と結果を証拠として示すことを要件にした。Planner（完了条件に確認用チェックを含める）とDeveloper（自分でチェックを実行し結果を添えて報告する）にそれぞれ1行反映した。
- **CLAUDE.mdのセッション運用**: `/compact`は同じタスクを続ける場合に限定し、継続性が不要なら`/clear`を選ぶと明記した。Skill刈り込みの判断材料として`/usage`の内訳を示した。D-016（Capabilityとしての`/usage`統合）・D-029（アカウントの使用量確認）で対象外とした判断とは文脈が異なり、今回はCLAUDE.mdに既にあるSkill刈り込みルールの判断材料を示す参照に留まる。
- **派生プロジェクト向けプラグイン方針**: docs/capability-layer.mdに「派生プロジェクトでの推奨プラグイン」を追加した。Code intelligence（LSP）プラグインは型付き言語で推奨、security-guidanceは既定で有効化しない。README.mdの新規プロジェクト初期化手順から参照した。
- project001自体へのプラグイン・Skillの追加は行わない。

### 理由
- 原則3の旧文言は、アプリ開発で本番データを持つ場合にスキーマ変更をマイグレーションなしで行う判断を許してしまい、原則8と矛盾していた。公式Code Reviewの例でも「後方互換でないマイグレーション」を重大指摘の典型としている。コード内部の互換レイヤーを残さない方針自体は維持した。
- 公式best-practicesは「検証手段を与える」を最初の推奨に挙げ、失敗パターン「trust-then-verify gap」の対策を「検証できないものは出荷しない」としている。手戻り（修正ループの往復）はトークン消費の主因でもあるため、完了条件を証拠ベースにすることは品質とコストの両方に効く。
- 公式costsは`/compact`自体が大きなリクエストであり、新規に始めるなら`/clear`はコストがかからないと明記している。旧文言の「能動的に`/compact`」は、継続性が不要な場面でも圧縮を促す読み方ができた。
- Code intelligenceプラグインは公式のトークン削減策に含まれる（grepと候補ファイルの読込を定義参照1回に置き換える）。ただし言語依存で、言語サーバー本体の導入が必要、クラウドセッションでは動作しないため、コードを持たないproject001では有効化せず派生プロジェクトの判断に委ねた。
- security-guidanceはStop Hookでターンごとに別途LLMレビューを実行するため、トークン節約の目的と逆行する。Reviewerのセキュリティ観点と役割も重なるため、高リスク領域のアプリに限った選択肢とした。

### 採用しなかった候補
- **`maxEffortLevel`設定**: D-029でAgent別にeffortを固定済みで、上限を設けるとPlanner/Reviewerの`high`を意図せず制限しうる。
- **`/code-review`バンドルSkillをReviewerの代替・併用にする**: ローカル`/code-review`はREVIEW.mdを読まない（公式code-reviewページに明記）ため、敵対的検証の規約を適用できない。
- **テスト出力を絞るPreToolUse Hook（公式costsの例）**: project001にテストランナーが存在しない。派生プロジェクトでテストが導入された時点で検討する。
- **`/skill-doctor`の記載**: 公式commandsページで説明を確認できなかったため、D-013の前例に従い記載しない。Skill刈り込みの判断材料は公式costsで確認できた`/usage`の内訳とした。

### 影響
- 派生プロジェクトでは、DBスキーマ変更・公開API変更時に移行が必須となる。完了報告にはチェックの実行結果が必須となる。
- 常時ロード行数は増えていない（既存行の修正のみ）。
