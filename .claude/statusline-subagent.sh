#!/usr/bin/env bash
# subagentStatusLine 用スクリプト。
#
# Claude Codeはリフレッシュのたびに、現在表示されている全サブエージェントの
# 情報を1つのJSON（`tasks`配列を含む）としてこのスクリプトのstdinへ渡す。
# 通常のチャット中（サブエージェントが1つも動いていない時）はこのスクリプト
# 自体が呼び出されないため、待機時に何かを出す処理は不要。
#
# 出力は「1行1タスク」のNDJSON形式で、各行が {"id": "<タスクID>", "content": "<表示文言>"}
# という形をとる。idを省略した行を出すとそのタスクは既定表示のままになるため、
# 上書きしたい全タスクに対して必ずidを含めて出力する。
#
# 公式スキーマ（docs/status-line.md参照）: tasks[] の各要素は
# id, name, type, status, description, label, startTime, model, effort,
# contextWindowSize, tokenCount, tokenSamples, cwd を持つ。
# 「現在使用中のツール名」を表すフィールドは公式スキーマに存在しないため、
# developer内での「実装中/確認中」のようなツール単位の細分けはできない
# （project001/AGENTS.mdの「理解してから作る」原則に従い、確認できない
# 挙動を前提にした実装はしない）。
#
# 注記: 当初はcolumns（行幅）に合わせてshellの`cut -c`で切り詰めていたが、
# 実機検証でLC_CTYPE=POSIX環境ではcut -cがバイト単位で切り詰め、日本語が
# 文字化けすることが判明した。またcolumnsがコードポイント数・表示幅の
# どちらを指すかも未確認。誤ったtruncationで表示を壊すより、内容自体を
# 短く保ち（表示文言は元々10〜20文字程度）、はみ出した場合の折り返し・
# 省略はClaude Code側のレンダリングに委ねる方針にした。descriptionだけは
# jqのUTF-8安全なスライスで軽く上限を設ける。

input=$(cat) || exit 0

echo "$input" | jq -e '.tasks' >/dev/null 2>&1 || exit 0

echo "$input" | jq -c '.tasks[]' 2>/dev/null | while read -r task; do
  id=$(echo "$task" | jq -r '.id // empty' 2>/dev/null)
  [ -z "$id" ] && continue

  # 種別は type を優先し、無ければ name にフォールバックする
  # （公式スキーマ上は type だが、念のため name も見る防御的な実装）
  agent_type=$(echo "$task" | jq -r '.type // .name // empty' 2>/dev/null)
  status=$(echo "$task" | jq -r '(.status // "") | ascii_downcase' 2>/dev/null)
  # descriptionは念のため20文字（コードポイント単位、jqはUTF-8安全）で上限を設ける
  description=$(echo "$task" | jq -r '(.description // "")[0:20]' 2>/dev/null)

  case "$status" in
    fail*|error*)
      content="⚠️ 問題が発生しました"
      ;;
    complete*|done|success*)
      content="📦 完了しました"
      ;;
    wait*|block*|pending*|paused)
      content="⏸ 待っています"
      ;;
    *)
      case "$agent_type" in
        planner)    content="🟢 準備しています" ;;
        researcher) content="🔍 必要な情報を集めています" ;;
        developer)  content="🛠 実装・動作確認をしています" ;;
        reviewer)   content="🔎 品質を確認しています" ;;
        *)          content="🔧 作業しています" ;;
      esac
      if [ -n "$description" ]; then
        content="$content（$description）"
      fi
      ;;
  esac

  jq -nc --arg id "$id" --arg content "$content" '{id: $id, content: $content}' 2>/dev/null
done
