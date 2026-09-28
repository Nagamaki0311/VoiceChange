#!/usr/bin/env python3
import json
import os
import subprocess
import sys


def has_pending_change(project_dir, rel_path):
    result = subprocess.run(
        ["git", "-C", project_dir, "status", "--porcelain", "--", rel_path],
        capture_output=True,
        text=True,
    )
    return bool(result.stdout.strip())


def main():
    payload = json.load(sys.stdin)
    if payload.get("agent_type") != "developer":
        return
    project_dir = os.environ.get("CLAUDE_PROJECT_DIR", os.getcwd())
    if has_pending_change(project_dir, "docs/progress.md"):
        return
    print(
        "[subagent-doc-check] docs/progress.mdへの記録が見当たりません。"
        "完了と報告する前に、実施内容・結果・次回開始位置をdocs/progress.mdへ追記してください。",
        file=sys.stderr,
    )
    sys.exit(2)


if __name__ == "__main__":
    main()
