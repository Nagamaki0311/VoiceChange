#!/bin/sh
# project001 Capability Layer 検出スクリプト。
# 一切のインストール・書き込みを行わず、利用可否の表示のみを行う（案内のみ）。
# 検出対象の追加は下記 CAPABILITIES の1行追加のみで完結する。
#
# 各行フォーマット: name:detect_cmd:purpose:install_hint
# purpose/install_hintは`--check`モードでは使用しない。
CAPABILITIES="
agent-reach:agent-reach:Researcherが外部情報収集（GitHub/Web/RSS/YouTube/Reddit/X等）で優先利用する:pip install agent-reach
code-review-graph:code-review-graph:Developer/Reviewerが変更の影響範囲（呼び出し元・依存関係）解析で優先利用する:pip install code-review-graph
ctx7:ctx7:Planner/Developer/Reviewer/Researcherが外部ライブラリの最新ドキュメント・型定義の確認で優先利用する:https://github.com/upstash/context7 のREADMEを参照
gh:gh:Researcherが調査で優先利用する:https://cli.github.com/ を参照
node:node:Context7 CLI(ctx7)の実行に必要(Node.js 18+前提):https://nodejs.org/ を参照
python3:python3:Agent-Reach・Code Review Graphの導入(pip)に必要:https://www.python.org/ を参照
playwright:playwright:派生プロジェクトでUIのブラウザ操作テストが必要になった場合に利用する:npm install -D playwright
"

check_available() {
  command -v "$1" >/dev/null 2>&1
}

run_check() {
  available=""
  unavailable=""
  old_ifs="$IFS"
  IFS='
'
  for line in $CAPABILITIES; do
    [ -z "$line" ] && continue
    name=$(echo "$line" | cut -d: -f1)
    detect=$(echo "$line" | cut -d: -f2)
    if check_available "$detect"; then
      available="$available $name"
    else
      unavailable="$unavailable $name"
    fi
  done
  IFS="$old_ifs"
  available=$(echo "$available" | sed 's/^ *//')
  unavailable=$(echo "$unavailable" | sed 's/^ *//')
  [ -z "$available" ] && available="(none)"
  [ -z "$unavailable" ] && unavailable="(none)"
  echo "Capabilities available: $available"
  echo "Capabilities unavailable: $unavailable"
}

run_human() {
  echo "project001 Capability Layer: 導入状況"
  echo
  old_ifs="$IFS"
  IFS='
'
  for line in $CAPABILITIES; do
    [ -z "$line" ] && continue
    name=$(echo "$line" | cut -d: -f1)
    detect=$(echo "$line" | cut -d: -f2)
    purpose=$(echo "$line" | cut -d: -f3)
    hint=$(echo "$line" | cut -d: -f4-)
    if check_available "$detect"; then
      version=$("$detect" --version 2>/dev/null | head -n 1)
      if [ -n "$version" ]; then
        echo "[導入済み] $name ($version)"
      else
        echo "[導入済み] $name"
      fi
    else
      echo "[未導入] $name"
      echo "  用途: $purpose"
      echo "  インストール例: $hint"
    fi
  done
  IFS="$old_ifs"
  echo
  echo "未導入のものがあってもproject001は完全に動作します（すべてOptional Dependencyです）。"
  echo "このスクリプトは表示のみを行い、インストールや設定の変更は一切行いません。"
}

case "$1" in
  --check)
    run_check
    ;;
  *)
    run_human
    ;;
esac

exit 0
