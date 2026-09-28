#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

RULES_PATH = Path(__file__).resolve().parent.parent / "ja-style-rules.json"
TARGET_SUFFIXES = (".md",)
SENTENCE_END_RE = re.compile(r"(ます|ました|です|でした)[。！？]")
REPEAT_THRESHOLD = 3


def load_rules():
    if not RULES_PATH.exists():
        return []
    return json.loads(RULES_PATH.read_text(encoding="utf-8"))


def check_line_rules(text, rules):
    hits = []
    for rule in rules:
        for m in re.finditer(rule["pattern"], text):
            hits.append((m.group(0), rule["note"], rule["good"]))
    return hits


def check_sentence_ending_repetition(text):
    hits = []
    run_val, run_len = None, 0
    for ending in SENTENCE_END_RE.findall(text):
        run_len = run_len + 1 if ending == run_val else 1
        run_val = ending
        if run_len == REPEAT_THRESHOLD:
            hits.append(ending)
    return hits


def authored_text(payload):
    tool_input = payload.get("tool_input", {})
    if payload.get("tool_name") == "Write":
        return tool_input.get("content", "")
    return tool_input.get("new_string", "")


def main():
    payload = json.load(sys.stdin)
    if payload.get("tool_name") not in ("Write", "Edit"):
        return
    file_path = payload.get("tool_input", {}).get("file_path", "")
    if not file_path.endswith(TARGET_SUFFIXES):
        return

    text = authored_text(payload)
    line_hits = check_line_rules(text, load_rules())
    repeat_hits = check_sentence_ending_repetition(text)
    if not line_hits and not repeat_hits:
        return

    messages = [
        f"[ja-style-check] {file_path} への書き込みに日本語の癖を検出しました。単語だけの置き換えは禁止、該当文全体を書き直してください。"
    ]
    for matched, note, good in line_hits:
        messages.append(f"- 「{matched}」: {note} → 例: {' / '.join(good)}")
    for ending in repeat_hits:
        messages.append(f"- 文末「{ending}」が{REPEAT_THRESHOLD}文以上連続しています。文末のバリエーションを増やしてください。")

    print("\n".join(messages), file=sys.stderr)
    sys.exit(2)


if __name__ == "__main__":
    main()
