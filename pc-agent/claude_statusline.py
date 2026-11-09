#!/usr/bin/env python3
"""
Claude Code statusLine hook for the ESP32-C3 Desktop Companion.
---------------------------------------------------------------
Claude Code invokes this script on every statusline update and pipes a JSON
payload to stdin that includes the account's REAL 5-hour and weekly quota
usage (`rate_limits.five_hour` / `rate_limits.seven_day`). That data is not
available anywhere else on disk, so this script caches it to a small JSON
file that companion_agent.py reads instead of estimating usage from token
counts.

Setup (in ~/.claude/settings.json):
{
  "statusLine": {
    "type": "command",
    "command": "python3 /absolute/path/to/pc-agent/claude_statusline.py"
  }
}

Note: rate_limits is only populated for Pro/Max subscription accounts (not
API-key billing), and only after the first API response of a session.
"""
import json
import os
import sys

CACHE_PATH = os.path.expanduser(os.path.join("~", ".claude", "companion_rate_limits.json"))


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        payload = {}

    rate_limits = payload.get("rate_limits") or {}
    five_hour = rate_limits.get("five_hour") or {}
    seven_day = rate_limits.get("seven_day") or {}

    five_pct = five_hour.get("used_percentage")
    week_pct = seven_day.get("used_percentage")

    if five_pct is not None or week_pct is not None:
        cache = {
            "five_hour_pct": five_pct,
            "five_hour_resets_at": five_hour.get("resets_at"),
            "week_pct": week_pct,
            "week_resets_at": seven_day.get("resets_at"),
        }
        try:
            os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
            with open(CACHE_PATH, "w", encoding="utf-8") as f:
                json.dump(cache, f)
        except Exception:
            pass

    # Keep a normal-looking statusline so nothing regresses in the terminal.
    model = (payload.get("model") or {}).get("display_name", "Claude")
    cwd = (payload.get("workspace") or {}).get("current_dir", "")
    five_txt = f"{five_pct:.0f}%" if five_pct is not None else "--"
    week_txt = f"{week_pct:.0f}%" if week_pct is not None else "--"
    print(f"{model} | {os.path.basename(cwd)} | 5h:{five_txt} wk:{week_txt}")


if __name__ == "__main__":
    main()
