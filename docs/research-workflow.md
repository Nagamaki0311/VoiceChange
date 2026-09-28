# 調査ワークフロー

外部情報の調査が必要なタスクにおける、Planner→Researcher→Developer→Reviewerの流れを定める。Capability Layerの規約はdocs/capability-layer.mdを参照（Agent-Reachは現在Tier2で、本ワークフローには組み込まれていない）。

## いつResearcherを使うか

Managerは、Plannerの計画に外部情報の裏付けが必要と判断された場合のみResearcherを起動する。単純な内部リファクタリングやドキュメント整理など、外部調査が不要なタスクではResearcherを起動しない（不要なAgent起動を避ける。AGENTS.md参照）。

## 流れ

1. **Planner**: 何を調べる必要があるか（調査対象）と、どのチャネル（GitHub/Web/RSS/YouTube/Reddit/X等）が適切かを整理し、Managerに提案する。実際の情報収集は行わない。
2. **Manager**: 提案を踏まえResearcherを起動するか判断する。
3. **Researcher**: WebFetch/WebSearchで情報収集する（Agent-Reachは現在Tier2のため組み込まない。Tier1昇格後の利用方法はdocs/agent-reach.md参照）。
   - 複数ソースから得た情報の重複を除去する
   - 情報源の信頼性（一次情報か、更新日時、著者/組織の実在性等）を評価する
   - 結果を出典（URL・取得日時）付きで要約し、Managerに返す。生の検索結果は会話コンテキストに残さない
4. **Developer**: Researcherの調査結果を実装の根拠として反映する。
5. **Reviewer**: 実装だけでなく、調査結果の妥当性も確認する（次項）。

## Reviewerが確認する調査品質基準

- 情報源が適切か（一次情報・公式ドキュメントが優先されているか）
- 情報が最新か（古い情報に基づいていないか）
- 重複取得がないか（同じ情報を複数チャネルから冗長に集めていないか）
- 不十分な場合はManagerへ差し戻し、追加調査を要求する（修正ループはAGENTS.md参照）

## トークン効率

- Researcherは要約と出典のみをManagerに返す。生データ（HTML全文、動画の全文字幕等）は要約に反映した上で破棄する。
- 同一タスク内で同じチャネル・同じクエリの再検索を避ける。
