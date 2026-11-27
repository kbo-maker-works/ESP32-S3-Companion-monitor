#!/usr/bin/env python3
"""
Antigravity CLI (agy) status line for the CompaDeck: the plan's quotas for the
"AI usage" widget, the same way td_statusline.py does it for Claude Code.

agy runs the status line command on every update (and refreshes the quota
during long turns) and pipes it a JSON payload whose "quota" holds the real
windows, each {"remaining_fraction", "reset_time", "reset_in_seconds"}:

  gemini-5h, gemini-weekly   Gemini models
  3p-5h, 3p-weekly           third-party models in Antigravity (Claude, GPT)

No password, no API call: this only writes what agy hands over to
~/.touchdeck/agy_usage.json, where touchdeck_agent.py reads it:

  {"at": <epoch>, "plan": "Google AI Pro",
   "quota": {"gemini-5h": {"pct": <used %>, "resets_at": <epoch>}, ...}}

The quota is the account's, so the Antigravity app spends the same windows;
they only move here while agy runs. It prints a normal status line:
  Gemini 4 Hydrogen (High) | ctx:3% | 5h:11% wk:3%

Setup, in ~/.gemini/antigravity-cli/settings.json:
  "statusLine": {"type": "command",
                 "command": "/path/to/pc-agent/.venv/bin/python -I /path/to/pc-agent/td_agy_statusline.py"}

Standard library only, so it starts fast.
"""
import json
import os
import sys
import tempfile
import time
from datetime import datetime

USAGE_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "agy_usage.json")
BUCKETS = ("gemini-5h", "gemini-weekly", "3p-5h", "3p-weekly")


def epoch(v):
    """reset_time as Unix seconds (ISO text), None when unknown."""
    if not v:
        return None
    try:
        return int(datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp())
    except ValueError:
        return None


def windows(payload, now):
    """{bucket: {"pct", "resets_at"}} of the payload's quota."""
    out = {}
    quota = payload.get("quota") or {}
    for name in BUCKETS:
        q = quota.get(name)
        if not isinstance(q, dict) or q.get("remaining_fraction") is None:
            continue
        try:
            used = (1.0 - float(q["remaining_fraction"])) * 100.0
        except (TypeError, ValueError):
            continue
        reset = epoch(q.get("reset_time"))
        if reset is None and isinstance(q.get("reset_in_seconds"), (int, float)):
            reset = int(now + q["reset_in_seconds"])
        out[name] = {"pct": round(max(0.0, min(100.0, used)), 1), "resets_at": reset}
    return out


def save(data):
    folder = os.path.dirname(USAGE_FILE)
    os.makedirs(folder, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".agy_usage")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, USAGE_FILE)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def status_line(payload, quota):
    model = ((payload.get("model") or {}).get("display_name") or "Antigravity").strip()
    parts = [model]
    ctx = (payload.get("context_window") or {}).get("used_percentage")
    parts.append("ctx:%s" % ("%.0f%%" % ctx if isinstance(ctx, (int, float)) else "--"))
    q5, q7 = quota.get("gemini-5h"), quota.get("gemini-weekly")
    if q5 or q7:
        parts.append("5h:%s wk:%s" % ("%.0f%%" % q5["pct"] if q5 else "--", "%.0f%%" % q7["pct"] if q7 else "--"))
    return " | ".join(parts)


def main():
    try:
        payload = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    now = time.time()
    quota = windows(payload, now)
    if quota:  # a payload without them (before the first response) keeps the last ones
        save({"at": int(now), "plan": str(payload.get("plan_tier") or "")[:40], "quota": quota})
    print(status_line(payload, quota))


if __name__ == "__main__":
    main()
