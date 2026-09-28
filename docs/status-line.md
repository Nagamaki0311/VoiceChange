# サブエージェント進捗の可視化（Status Line）

project001は、サブエージェント（Planner/Researcher/Developer/Reviewer）の実行中のみ、Claude Codeの「エージェントパネル」に進捗を自然な日本語で表示する。通常のチャット中は何も表示されず、LLM呼び出しも一切行わない。

## 採用した仕組み: `subagentStatusLine`

Claude Codeには`.claude/settings.json`で設定できる`subagentStatusLine`という機能があり、サブエージェントがエージェントパネルに表示される際の行の中身をカスタマイズできる（公式ドキュメント: https://code.claude.com/docs/en/statusline#subagent-status-lines）。

### なぜHooksではなくこれを選んだか

要求では「Claude Code標準機能（Agent View・Hooks・Status Line等）を活用し、最適な実装方法を選択」とされていた。Hooksは会話コンテキストへテキストを注入する仕組みであり、進捗を継続的に表示しようとするとLLMのトークンを消費してしまう（「不要なLLMトークンを消費しない」という要件に反する）。`subagentStatusLine`はUI側（エージェントパネル）でレンダリングされるだけで、会話コンテキストには一切入らないため、この要件を構造的に満たす。

**Agent View**（サブエージェント実行時のみ自動的に現れるパネル）と**subagentStatusLine**（そのパネルの各行の表示内容をカスタマイズする設定）は competing な選択肢ではなく、後者は前者の中身をカスタマイズするものである。「通常チャットでは表示しない」「エージェント実行時のみ自動表示」という要件は、Agent View自体の既定の挙動（サブエージェントが動いていない時は表示されない）としてすでに満たされている。

## 実装

- `.claude/settings.json`の`subagentStatusLine`から`.claude/statusline-subagent.sh`を呼び出す
- スクリプトはjq/bashのみで完結し、LLM呼び出しは行わない

### 入力（stdin）の実際のスキーマ

Claude Codeは**リフレッシュのたびに1回**、現在パネルに表示されている全サブエージェントの情報をまとめて1つのJSONとしてスクリプトへ渡す（1エージェントにつき1回呼ばれるのではない）。

```json
{
  "session_id": "...",
  "columns": 60,
  "tasks": [
    {
      "id": "task-1",
      "name": "...",
      "type": "developer",
      "status": "running",
      "description": "Hookフィルタを修正",
      "label": "...",
      "startTime": "...",
      "model": "...",
      "effort": "...",
      "contextWindowSize": 200000,
      "tokenCount": 12345,
      "tokenSamples": [...],
      "cwd": "..."
    }
  ]
}
```

`tasks[].type`（無ければ`.name`にフォールバック）が`.claude/agents/*.md`のfrontmatter `name`（`planner`/`researcher`/`developer`/`reviewer`）に対応する。`status`の正確な値の一覧（`running`/`completed`/`failed`等）は公式ドキュメントに明記がないため未確認。本スクリプトは`fail*`/`complete*`/`wait*`等のプレフィックス一致で防御的に判定し、未知の値は「進行中」として扱う。

**現在実行中のツール名を表すフィールドは公式スキーマに存在しない。** そのため、当初検討していた「developer内で実装中/確認中を区別する」という細分けは実現できず、`developer`は一律「🛠 実装・動作確認をしています」という1つの文言にした（確認できない挙動を前提にした実装はしない。AGENTS.mdの「理解してから作る」原則）。

### 出力の実際の形式

「1行1タスクのJSON」（NDJSON）で、`{"id": "<タスクID>", "content": "<表示文言>"}`を上書きしたいタスクの数だけ出力する。`id`を省略した行は既定表示のまま、`content`を空文字にするとその行は非表示になる。

### 表示文言の対応表

| サブエージェント | 表示 |
|---|---|
| planner | 🟢 準備しています |
| researcher | 🔍 必要な情報を集めています |
| developer | 🛠 実装・動作確認をしています |
| reviewer | 🔎 品質を確認しています |
| 未知の種別（将来追加分） | 🔧 作業しています |
| status=completed系 | 📦 完了しました |
| status=failed系 | ⚠️ 問題が発生しました |
| status=waiting系 | ⏸ 待っています |

`description`（Agentの起動時に渡した短い説明）が取得できた場合、先頭20文字（jqによるUTF-8安全なスライス）を文言に括弧書きで付け加える。新しいサブエージェントを追加する場合は、スクリプト内の対応表に1行足すだけでよい。

## 実装時に発見・修正した問題（検証の記録）

project001はドキュメント作成前に必ず実機で確認する方針（REVIEW.mdの敵対的検証・AGENTS.mdの「理解してから作る」）を、この機能自体にも適用した。

1. **スキーマの誤り**: 最初にstatusline-setupエージェントへ依頼した際、`subagentStatusLine`の正式なstdinスキーマが不明なまま「1エージェント1回呼び出し、フラットなJSON」という誤った前提で実装された。WebSearchで公式ドキュメント（code.claude.com/docs/en/statusline）を確認したところ、実際は「リフレッシュごとに`tasks`配列で全件まとめて渡される」「出力はNDJSON形式」という全く異なる仕様だったため、スクリプトを書き直した。
2. **`cut -c`によるマルチバイト文字の破壊**: 当初は`columns`（行幅）に合わせて`cut -c`で切り詰めていたが、実行環境の`LC_CTYPE=POSIX`（Cロケール）では`cut -c`がバイト単位で切り詰め、日本語が途中で欠けることを実機テストで発見した。`columns`がコードポイント数・表示幅のどちらを指すかも公式ドキュメントに明記がなく確認できなかったため、行全体のtruncationは行わず、内容自体を短く保つ設計に変更した（description部分のみjqのUTF-8安全なスライスで軽く上限を設けている）。
3. **jqの整形出力によるNDJSON違反**: `jq -n`（`-c`なし）は複数行に整形して出力するため、「1行1タスク」という仕様に反していた。`-c`（compact）フラグを追加して修正した。

## 関連

- Agent構成: docs/agents.md
- Claude Code側の基本方針: CLAUDE.md
