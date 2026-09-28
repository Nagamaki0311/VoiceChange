# タスク管理

現在のタスク、優先順位、状態を管理する。

## 状態の定義

- `未着手`: まだ着手していない
- `計画中`: plannerによる計画作成中/完了
- `調査中`: researcherによる外部情報収集中（外部調査が必要なタスクのみ）
- `実装中`: developerによる実装中
- `レビュー中`: reviewerによる確認中
- `完了`: 完了条件（AGENTS.md参照）を満たした

## タスク一覧

| ID | タスク | 優先度 | 状態 | 担当エージェント | 備考 |
|----|--------|--------|------|------------------|------|
| T-001 | 長期開発用AI開発環境の整備（tasks/progress/decisions） | 高 | 完了 | claude | docs配下に3ファイルを作成し、CLAUDE.mdに参照ルールを追加 |
| T-002 | project001を共通AI開発エージェント用テンプレートへ転換 | 高 | 完了 | claude | CLAUDE.mdに「プロジェクトの役割」「トークン効率化ルール」を追加。個別アプリの仕様・コードは保持しない方針を明記（D-002参照） |
| T-003 | ponytail（DietrichGebert/ponytail）のコード品質ルールを導入 | 中 | 完了 | claude | AGENTS.mdの内容をCLAUDE.mdに「コード品質ルール（Ponytail）」として統合（D-003参照） |
| T-004 | AI開発OS化: Manager導入とドキュメント/Agent構成の整理 | 高 | 完了 | claude | CLAUDE.mdを大幅簡潔化し、Manager役割（このセッション自身）を明記。docs/agents.mdを新設しAgent構成とPonytail原則を集約（D-003の内容を移設）（D-004参照） |
| T-005 | SessionStart/PreCompact Hookの導入 | 中 | 完了 | claude | .claude/settings.jsonを新設。tasks.md/progress.mdの自動表示と圧縮前リマインダーを1行shellコマンドで実装（D-005参照） |
| T-006 | AI開発OS全体レビュー（重複排除・Hook環境検証） | 高 | 完了 | claude | CLAUDE.mdのAgent説明重複を除去、Manager-Hook接続を明文化、Hook環境依存性を文書化（D-006参照） |
| T-007 | Agent別モデル最適化（Model Routing）の導入 | 中 | 完了 | claude | Planner=opus/Developer・Reviewer=sonnetに固定。軽量レビューはAgent呼び出し時のmodelパラメータ上書きで対応（D-007参照） |
| T-008 | ルートAGENTS.md新設、CLAUDE.mdとの責務分離 | 高 | 完了 | claude | 新7原則とPonytailを統合したAGENTS.mdを新設。CLAUDE.mdはClaude Code固有設定のみに縮小し@AGENTS.mdをimport（D-008参照） |
| T-009 | Agent-Reach対応（Optional Dependency・Researcher追加） | 高 | 完了 | claude | researcher Agentを新設し、Agent-Reachを検出できれば優先利用・不可なら自動フォールバックする疎結合設計を実装。docs/agent-reach.md・docs/research-workflow.mdを新設（D-009参照） |
| T-010 | レビュー方針を敵対的検証（Adversarial Review）へ変更 | 高 | 完了 | claude | REVIEW.mdを新設し、姿勢・18観点・7手順・重要度分類・検証パス（CONFIRMED/PLAUSIBLE）を集約。reviewer.mdを更新（D-010参照） |
| T-011 | 新規プロジェクト初期化手順の整備とSessionStart Hookの完了タスク除外 | 高 | 完了 | claude | README.mdに初期化手順を新設、AGENTS.mdから参照を追加。SessionStart Hookのcommandを完了タスク除外に変更し、docs/agents.mdの説明を更新（D-011参照） |
| T-012 | Capability Layer（Agent-Reach・Code Review Graphの検出規約）の統合 | 高 | 完了 | claude | SessionStart HookにCapability検出を追加し、planner/developer/reviewer/researcher.md・REVIEW.md・CLAUDE.md・docs/agents.md・docs/research-workflow.mdへ参照を追加。Code Review GraphはCLI直接呼び出しのみ統合（D-012参照） |
| T-013 | サブエージェント進捗の可視化（subagentStatusLine） | 中 | 完了 | claude | .claude/statusline-subagent.shを新設し、エージェントパネルに日本語で進捗表示。当初の実装は公式スキーマの誤認識・マルチバイト文字のtruncationバグがあり、実機検証で発見・修正（D-013参照） |
| T-014 | ユーザー環境のClaude Codeプラグイン導入状況の確認 | 低 | 完了 | claude | 導入済み7プラグインの名前空間衝突・自動起動有無を公式ドキュメントで調査。「Codegraph」プラグインとdocs/code-review-graph.mdが参照するpip製CLIは別プロダクトである旨をdocs/code-review-graph.mdへ追記。Ponytailプラグインとの重複はユーザー環境側の設定判断のためdocsへの反映なし |
| T-015 | AGENTS.md設計原則の圧縮（Ponytailプラグイン重複対応） | 低 | 完了 | claude | Ponytailプラグインとの重複を理由とした「参照のみ」化は、サブエージェントにSkillツールがないこと・このセッションにプラグイン自体が存在しないことの2点から不採用と判断。本文は維持し説明文のみ圧縮（D-014参照） |
| T-016 | project001をClaude Code Starter Kit化（bootstrap.sh・Context7統合） | 高 | 完了 | claude | .claude/bootstrap.shを新設し案内のみのCapability検出に集約。Context7・GitHub CLIをTier1へ追加、Claude Codeプラグインはproject scopeで既定有効化しない方針を維持。/init-projectコマンドを新設（D-015参照） |
| T-017 | Claude Code 2026運用ナレッジの適用（REVIEW.mdの過剰指摘抑制ルール等） | 中 | 完了 | claude | REVIEW.mdへ過剰指摘抑制ルールを追加、CLAUDE.mdへ削除テストと/compact表記を追記、docs/agents.mdのStop Hook不採用理由を補強（D-016参照） |
| T-018 | PostToolUse Hookによる日本語文体チェックの導入 | 中 | 完了 | claude | .claude/hooks/ja-style-check.py・.claude/ja-style-rules.jsonを新設し.claude/settings.jsonへ登録。書き込み単位（Write/Editの新規テキストのみ）を検査し、docs/agents.mdのPostToolUse不採用理由を更新（D-017参照） |
| T-019 | SubagentStop/SessionEnd Hookによる記録漏れの機械的検知 | 中 | 完了 | claude | .claude/hooks/subagent-doc-check.pyを新設しSubagentStop（developer限定）へ登録、SessionEndにgit statusベースのリマインドを追加。docs/agents.mdのHook構成を更新（D-018参照） |
| T-020 | Claude Code公式仕様に基づく開発基盤監査、taste-skill 13件の撤去 | 高 | 完了 | claude | CLAUDE.md/AGENTS.md/.claude配下/docs/Capability Layer/レビューフローを2026年公式仕様と照らして監査。最優先事項としてtaste-skill 13件を削除しCLAUDE.mdを更新（D-019参照）。残り論点はバックログへ |
| T-021 | `.claude/rules/`不採用の記録とREADME.mdのHook記述修正 | 低 | 完了 | claude | docs/decisions.mdにD-020（`.claude/rules/`不採用）を追加。README.mdの`.claude/settings.json`記述をHook 5種の実態に合わせて更新 |
| T-022 | Capability Layer Tier1（Agent-Reach/Code Review Graph/Context7/GitHub CLI）をTier2へ格下げ | 中 | 完了 | claude | 動作未検証のまま常に`unavailable`だった4件をTier2へ格下げ。.claude/agents/*.md・REVIEW.md・docs/research-workflow.md・CLAUDE.md・docs/capability-layer.mdから「優先利用」の条件分岐を削除（D-021参照）。docs/agent-reach.md等は昇格時の参照として保持 |
| T-023 | Auto Memoryとdocs/tasks.md等の役割分担を明記 | 中 | 完了 | claude | docs/agents.mdに「Auto Memoryとの役割分担」節を新設し、表形式で違いを整理（D-022参照）。T-020監査で洗い出した4項目すべて完了 |
| T-024 | Claude Code向けMarkdownの構成・書式監査 | 中 | 完了 | claude | docs/agents.mdのHook構成節を見出し単位へ再構成、CLAUDE.md/REVIEW.mdの密な段落を箇条書き化、docs/agent-reach.md・context7.mdの重複記述を削除（D-023参照）。設計思想は変更なし |
| T-025 | 外部リポジトリ調査（HirotakaAdachi/Rogue）から3項目を移植 | 中 | 完了 | claude | AGENTS.mdに原則9（大規模ファイルのgrepアンカー規約）を新設、REVIEW.mdに指摘恒久化（ABSOLUTE RULE昇格）節を新設、developer.mdに通称⇄コード名対照表の運用を追加（D-024参照）。trading-system等の個別アプリには適用しない |
| T-026 | project001へのDesign基盤統合（Designer Agent常設・Frontend Designプラグイン有効化） | 高 | 完了 | claude | .claude/agents/designer.mdと.claude/skills/design-principles/SKILL.mdを新設、docs/design-workflow.mdを新設、.claude/settings.jsonにfrontend-design@claude-plugins-officialを有効化。AGENTS.md/docs/agents.md/REVIEW.md/reviewer.md/docs/capability-layer.md/CLAUDE.md/README.mdを整合するよう更新（D-025参照）。既存の「UI Agent不採用」「プラグイン既定非有効化」方針をユーザー明示判断で上書き |
| T-027 | 2026年9月最新環境への見直し（Concise出力スタイル・デザイントークン往復） | 中 | 完了 | claude | .claude/settings.jsonにoutputStyle: Conciseを追加、docs/design-workflow.mdに「デザイントークンの往復」節を新設、designer.md/developer.mdに対応する記録責務を追記（D-026参照）。公式Weekly digest確認で他の大きな変更は無し |
| T-028 | project001とClaude Codeの互換性解析、Model Routing上書きリスクの文書化 | 低 | 完了 | claude | Agent frontmatter/Hook/settings.jsonの現行仕様との整合を確認（非互換なし）。CLAUDE_CODE_SUBAGENT_MODEL環境変数がModel Routingを無効化しうる点をdocs/agents.mdへ追記（D-027参照） |
| T-029 | 高性能モデル前提でのAgent定義の指示削減（外部記事の知見を反映） | 中 | 完了 | claude | OpenAI記事「Rethinking skills and prompts for GPT-6 Astra」の4原則のうち、project001の削除テストと結論が一致する範囲を採用。tools allowlistで担保済みの3行とハーネス既定と重複する2行を削除、planner.mdに完了条件を追加、AGENTS.mdの参照行を場面限定型へ変更（D-028参照）。Skillは基準を満たすため変更なし |
| T-030 | Agent別のeffort固定（外部記事のトークン節約知見からの採用1件） | 中 | 完了 | claude | Miles Deutscher氏の記事の提案11項目を照合し、未対応だったeffort設定のみ採用。Planner/Designer/Reviewerにhigh、Researcherにmediumを設定、Developerは未指定のまま。docs/agents.mdのModel Routing表にeffort列を追加（D-029参照）。不採用10項目の理由も記録 |
| T-031 | 公式ベストプラクティス照合による原則・運用の見直し | 中 | 完了 | claude | AGENTS.md原則3に永続データ・公開APIの移行例外を追加、完了条件を証拠ベースに具体化（Planner/Developerへ反映）、CLAUDE.mdの/compact・/clear運用を明確化、派生プロジェクト向けプラグイン方針（LSP推奨・security-guidance既定無効）を追加（D-030参照） |

## バックログ（未着手・優先度未確定）

- （ここに新しいタスク候補を追記する）
- progress.mdが将来肥大化した場合、docs/progress-archive.md等への分割を検討する（D-006時点では未実施・優先度未確定）

## メモ

- 新しいタスクを追加したら、必ず優先度と状態を設定すること。
- タスクの状態が変わったら都度このファイルを更新する（作業完了後にまとめて更新しない）。
- 詳細な作業内容や経緯は [progress.md](./progress.md) を参照。
- 設計上の判断が必要になった場合は [decisions.md](./decisions.md) に記録する。
- **状態列の値は必ず「状態の定義」にある6値を完全一致（前後の空白のみ許容）で使うこと**。SessionStart Hookの完了タスクフィルタ（`.claude/settings.json`）が状態列の完全一致で判定しているため、`完了(要再確認)`のような接尾辞付きの値は「未完了」として扱われる（安全側だが、フィルタが効かなくなる）。既知の制約としてT-011のレビューループで確認済み（docs/progress.md参照）。
- **タスク名・備考欄に未エスケープの`|`を含めないこと**。SessionStart Hookは`docs/tasks.md`を`awk -F'|'`で列分割しており、セル内に`|`があると以降の列がずれる。Markdownテーブルとしても不正な記法になるため、通常の運用では発生しない想定。
