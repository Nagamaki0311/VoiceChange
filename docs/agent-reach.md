# Agent-Reach 対応

project001はAgent-Reach（[Panniantong/Agent-Reach](https://github.com/Panniantong/Agent-Reach)、MIT License）をOptional Dependencyとして扱う。project001自体はAgent-Reachへの依存を持たず、実行環境にインストールされている場合のみResearcherが活用する。

**現在Tier2（検出のみ）**: 動作未検証のまま複数セッションで`unavailable`が続いているため、`.claude/agents/researcher.md`への振る舞い統合は見送っている（docs/capability-layer.md、D-021参照）。以下は検出・動作確認が取れた際にTier1へ昇格する場合の利用方針として残す。

## Agent-Reachとは

- `pip install agent-reach` で導入するCLIツール（`agent-reach`コマンド）。
- GitHub / Web / RSS / YouTube / Reddit / X / Bilibili 等の複数チャネルを横断して情報収集するために、上流ツール（`gh`, `curl`, `yt-dlp`, `feedparser` 等）の呼び出し方をSKILL.md経由でAgentに教える仕組み。単一の「検索コマンド」を提供するのではなく、Agentが直接上流ツールを呼び出す構成である。
- `agent-reach doctor` で、どのチャネルが利用可能かを診断できる。

project001はAgent-Reach本体を組み込まない。バージョン更新の影響を受けないよう、検出とフォールバックのみを実装する。

## 検出方法

Researcherは、調査開始前に以下で利用可否を確認する。

```bash
command -v agent-reach >/dev/null 2>&1 && agent-reach doctor
```

- コマンドが存在し `doctor` が正常終了する場合: 利用可能。`doctor` の出力から使えるチャネルを把握し、それらを優先利用する。
- コマンドが存在しない、または `doctor` が失敗する場合: 利用不可と判断し、即座に通常のWebFetch/WebSearchへフォールバックする。インストールを促したり、エラーで停止したりしない。
- 未導入環境でこのコマンドが `exit 1` で安全に失敗し、フォールバック分岐に入ることを確認済み（docs/progress.md T-009参照）。

## フォールバック方針

個別チャネルの取得コマンドが失敗した場合も、そのチャネルだけスキップして次の情報源に進むか、WebFetch/WebSearchで代替する。タスク全体を止めない（一般原則はdocs/capability-layer.md参照）。

## 疎結合設計（アーキテクチャ）

```
Planner ─(調査対象・チャネル方針)─> Manager ─> Researcher
                                              │
                                  command -v agent-reach?
                                   ├─ Yes → agent-reach doctor
                                   │         ├─ 成功 → 対応チャネルを優先利用
                                   │         └─ 失敗 → WebFetch/WebSearchへフォールバック
                                   └─ No  → WebFetch/WebSearchへフォールバック
                                              │
                                  重複除去・信頼性評価・要約
                                              │
                                       Manager（出典付き要約のみ）
```

- project001のリポジトリにAgent-Reachへの依存記述（package.json, pyproject.toml等）を追加しない。
- Agent-Reach固有のコマンド例・仕様の詳細はこのファイルにのみ記載する（researcher.md等には検出コマンドの呼び出し方針のみを書き、複製しない）。Agent-Reach側のインターフェースが変わった場合、更新箇所はこのファイル1つで済む。
- 検出は起動時の `command -v` チェックと `doctor` の終了コードのみに依存し、Agent-Reachの内部実装やバージョンに依存しない。

## 提供チャネル（利用可能時に優先利用）

GitHub, Web, RSS, YouTube, Reddit, X（その他 `agent-reach doctor` が利用可能と報告するチャネル）

## 関連

- Agent構成・モデル: docs/agents.md
- 調査ワークフロー: docs/research-workflow.md
- Claude Code側の基本方針: CLAUDE.md
