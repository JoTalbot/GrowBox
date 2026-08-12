#!/usr/bin/env python3
"""Send a command to GrowBox through ntfy (works behind NAT)."""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_env() -> dict[str, str]:
    env = {}
    for name in (ROOT / ".remote.env", Path.home() / ".growbox.remote.env"):
        if not name.exists():
            continue
        for line in name.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    env.update({k: os.environ[k] for k in ("NTFY_TOPIC", "REMOTE_KEY") if k in os.environ})
    return env


def post(topic: str, message: str) -> None:
    req = urllib.request.Request(
        f"https://ntfy.sh/{topic}",
        data=message.encode("utf-8"),
        headers={
            "Title": "cmd",
            "Tags": "robot",
            "Content-Type": "text/plain",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        print(resp.read().decode("utf-8", "replace"))


def main() -> int:
    parser = argparse.ArgumentParser(description="GrowBox remote command")
    parser.add_argument("cmd", help="ping|status|ota|flash|checkota|stage|mode|water|auto|reboot")
    parser.add_argument("arg", nargs="?", default="", help="argument (URL, stage, zone, light:on)")
    parser.add_argument("--json", action="store_true", help="send JSON instead of plaintext")
    args = parser.parse_args()

    env = load_env()
    topic = env.get("NTFY_TOPIC", "")
    key = env.get("REMOTE_KEY", "")
    if len(topic) < 5:
        print("Заполни NTFY_TOPIC в .remote.env", file=sys.stderr)
        return 2
    if args.json:
        payload = json.dumps({"cmd": args.cmd, "arg": args.arg, "key": key}, ensure_ascii=False)
    else:
        parts = [args.cmd]
        if args.arg:
            parts.append(args.arg)
        if key:
            parts.append(key)
        payload = " ".join(parts)
    print(f"→ ntfy.sh/{topic}: {payload}")
    try:
        post(topic, payload)
    except urllib.error.URLError as exc:
        print(f"fail: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
