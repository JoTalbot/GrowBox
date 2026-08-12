#!/usr/bin/env python3
"""Read recent GrowBox status messages from ntfy."""
from __future__ import annotations

import json
import os
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_topic() -> str:
    if "NTFY_TOPIC" in os.environ:
        return os.environ["NTFY_TOPIC"]
    env_path = ROOT / ".remote.env"
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            if line.startswith("NTFY_TOPIC="):
                return line.split("=", 1)[1].strip()
    return ""


def main() -> int:
    topic = load_topic()
    if len(topic) < 5:
        print("Нет NTFY_TOPIC", file=sys.stderr)
        return 2
    url = f"https://ntfy.sh/{topic}/json?poll=1&since=2h"
    req = urllib.request.Request(url, headers={"User-Agent": "GrowBox-agent/6.3"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        raw = resp.read().decode("utf-8", "replace")
    if not raw.strip():
        print("(пусто — коробка ещё не стучалась или топик другой)")
        return 0
    for line in raw.splitlines():
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print(line)
            continue
        if msg.get("event") != "message":
            continue
        title = msg.get("title", "")
        body = msg.get("message", "")
        print(f"[{title or '-'}] {body}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
