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
    wait_sec = 0
    since = "2h"
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--wait" and i + 1 < len(args):
            wait_sec = int(args[i + 1])
            i += 2
        elif args[i] == "--since" and i + 1 < len(args):
            since = args[i + 1]
            i += 2
        else:
            print(f"неизвестный аргумент: {args[i]}", file=sys.stderr)
            return 2
    topic = load_topic()
    if len(topic) < 5:
        print("Нет NTFY_TOPIC", file=sys.stderr)
        return 2
    import time
    deadline = time.time() + wait_sec
    url = f"https://ntfy.sh/{topic}/json?poll=1&since={since}"
    shown = 0
    while True:
        req = urllib.request.Request(url, headers={"User-Agent": "GrowBox-agent/6.3"})
        with urllib.request.urlopen(req, timeout=20) as resp:
            raw = resp.read().decode("utf-8", "replace")
        if raw.strip():
            for line in raw.splitlines():
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    print(line)
                    shown += 1
                    continue
                if msg.get("event") != "message":
                    continue
                title = msg.get("title", "")
                body = msg.get("message", "")
                print(f"[{title or '-'}] {body}")
                shown += 1
            if wait_sec <= 0 or shown:
                return 0
        if wait_sec <= 0:
            print("(пусто — коробка ещё не стучалась или топик другой)")
            return 0
        if time.time() >= deadline:
            print("(таймаут ожидания статуса)")
            return 1
        time.sleep(5)


if __name__ == "__main__":
    raise SystemExit(main())
