# デザインワークフロー

UI/UX実装を伴うタスクにおける、Planner→Designer→Developer→Reviewerの流れを定める。デザイン判断基準はSkill（`.claude/skills/design-principles/SKILL.md`）を参照（本ファイルでは重複記載しない）。

## いつDesignerを使うか

Managerは、Plannerの計画にUI/UX実装が含まれると判断された場合のみDesignerを起動する。バックエンド処理・ドキュメント整備等、UIを伴わないタスクではDesignerを起動しない（不要なAgent起動を避ける。AGENTS.md参照）。project001自体はUIを持たないテンプレートのため、project001自身の開発でDesignerが起動することは通常ない。個別アプリのリポジトリでUI実装タスクが発生した際に使う。

## 流れ

1. **Planner**: 計画にUI/UX実装が含まれるかを判断し、Managerに提案する。
2. **Manager**: 提案を踏まえDesignerを起動するか判断する。
3. **Designer**: 制作物の種類を判断し、UX・ビジュアル・モーション・レスポンシブ・アクセシビリティ方針を、Developerが実装できる粒度まで仕様化する（`.claude/agents/designer.md`参照）。
4. **Developer**: Designerの仕様を実装の根拠として反映する。
5. **Reviewer**: 実装だけでなく、デザイン意図との一致・既視感の有無等のUI/UXレビュー観点も確認する（REVIEW.md参照）。

## Claude Design（`/design`）との連携

Claude Design（`/design`）は、claude.aiアカウントでのログイン・Pro/Max/Team/Enterpriseプラン・対応プラットフォームを要件とするアカウントレベルの機能であり、project001側で有効化・インストールする対象ではない。要件を満たす場合、Managerがメインセッションで`/design`を呼び出し、Designerが作成した仕様をもとにアートボード（視覚的なモックアップ）を生成し、ユーザーの確認・修正を経てDeveloperへ渡す。要件を満たさない環境では、この工程を省略し、Designerの仕様書とFrontend Designプラグインの検討フレームワークのみでDeveloperへ渡す。いずれの場合も通常開発は止めない。

- 利用可能: Designer → Claude Design(`/design`) → 確認・修正 → Developer
- 利用不可: Designer（仕様書）→ Developer

`/design-sync`はAmazon Bedrock・Google Cloud Agent Platform・Microsoft Foundry・Claude Platform on AWSでは利用できない。project001はテンプレート元リポジトリであり特定アプリの永続的なデザインシステムを保持しないため、design-syncによる同期は個別アプリのリポジトリ側で必要に応じて行う。

### デザイントークンの往復（Design→Code時の齟齬防止）

Claude Design/Artifactsは、生成時に対象リポジトリのCLAUDE.md等に記録された配色・タイポグラフィ・余白トークンを自身の既定より優先して使う（公式仕様）。この仕組みを踏まえ、個別アプリのリポジトリでは次の順序を守る。

1. `/design`を呼ぶ前に、対象アプリのCLAUDE.md（またはテーマファイル）に既存のデザイントークンが記録されているか確認する。記録があれば、それを`/design`のプロンプトに明示し、Claude Designの出力を既存トークンに合わせる。
2. 記録が無い状態で`/design`が新しいトークン（配色・タイポグラフィ・余白）を採用した場合、Designerは仕様書にそのトークンを明記する。Designerはtoolsにread-onlyのもの（Read/Glob/Grep/WebFetch/WebSearch）しか持たないため、ファイルへの記録自体はDeveloperが実装の一環として対象アプリのCLAUDE.mdへ行う（形式は公式ドキュメントの例に準じる: 色・タイポグラフィ・スペーシングを列挙する短い節）。
3. 以降のDeveloper作業はこの記録済みトークンを参照し、Claude Design出力とは別に独自の値を決め打ちしない。

この往復を怠ると、`/design`が選択のたびに異なる既定値を使い、実装後に見た目の一貫性が崩れる（齟齬の主因）。project001自体はトークンを持たないため、この節はテンプレートとしての運用ルールであり、個別アプリ側で実際に機能する。

## Frontend Designプラグイン

Anthropic公式プラグイン（`claude-plugins-official`マーケットプレイス、project scopeで`.claude/settings.json`の`enabledPlugins`により有効化済み）。フロントエンド実装前に目的・トーン・制約・差別化の4点を検討させるフレームワークを提供し、「AIっぽい」既定のUIを避ける効果がある。Designer・Developerは、UI実装を伴うタスクでこのプラグインが提示する検討フレームワークを踏まえて作業する。project001は本プラグイン自体に実装上の依存を持たない（Claude Code側の機能であり、project001のコードから呼び出す対象ではない）。

## Reviewerが確認するデザイン品質基準

- AI生成UIのような既視感がないか
- 制作物の種類に適したデザインか
- 情報階層・タイポグラフィ・カラーが目的に合っているか
- レイアウト・コンポーネントに独自性があり、過剰にテンプレート化されていないか
- アニメーションに目的があり、過剰でないか
- レスポンシブ・タッチ操作・アクセシビリティに問題がないか
- ローディング・エラー・空状態等のUI状態が設計されているか
- 実装とデザイン意図が一致しているか
- 不十分な場合はManagerへ差し戻し、Designerへの追加検討を要求する（修正ループはAGENTS.md参照）

## トークン効率

- Designerは仕様書のみをManagerに返す。検討過程の試行錯誤や参照した外部情報の全文は要約に反映した上で破棄する。
- 既存のデザインシステム・コンポーネントが対象リポジトリにある場合、それを毎回作り直さず再利用する。
