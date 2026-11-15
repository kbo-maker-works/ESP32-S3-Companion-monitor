#!/usr/bin/env python3
"""
Claude Code status line for the TouchDeck: the plan's quotas and each
session's context for the AI app and the "Claude usage" widget.

Claude Code runs the status line command on every update and pipes it a JSON
payload that already holds the account's real usage windows
(rate_limits.five_hour / seven_day: used_percentage, resets_at) and the
session's context (context_window). No password, no keychain, no API call:
this only writes what Claude Code hands over, under ~/.claude/touchdeck/usage/,
where touchdeck_agent.py reads it:

  limits.json        {"current": <account id>, "accounts": {<id>: {"email", "org",
                     "limits": {"five_hour": {"pct", "resets_at"}, "seven_day": {...}}}}}
                     The windows are an account's: kept per account (several
                     accounts on one computer, switched with /login) and merged
                     across that account's sessions (an idle session's older
                     snapshot never overwrites a newer one).
  <session>.json     {"ctx_pct", "ctx_tokens", "ctx_window", "project", "model", "account"}
                     (removed by the agent's SessionEnd hook)
  account.json       cache of the logged-in account, read from Claude Code's
                     config (~/.claude.json, or $CLAUDE_CONFIG_DIR/.claude.json)

It prints a normal status line for the terminal:
  Opus 5.5 | my-repo | ctx:12% | 5h:45% wk:21%

Setup, in ~/.claude/settings.json:
  "statusLine": {"type": "command",
                 "command": "/path/to/pc-agent/.venv/bin/python -I /path/to/pc-agent/td_statusline.py"}

The windows exist only on a Pro / Max subscription, after a session's first
response. Standard library only, so it starts fast.
"""
import hashlib
import json
import os
import sys
import tempfile
import time
from datetime import datetime

USAGE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "touchdeck", "usage")
LIMITS_FILE = os.path.join(USAGE_DIR, "limits.json")
ACCOUNT_CACHE = os.path.join(USAGE_DIR, "account.json")
WINDOWS = ("five_hour", "seven_day")


def epoch(v):
    """resets_at as Unix seconds: from seconds, milliseconds or ISO text."""
    if v is None or v == "":
        return None
    if isinstance(v, (int, float)):
        return int(v / 1000 if v > 1e12 else v)
    try:
        return int(datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp())
    except ValueError:
        return None


def merge_window(old, new, now):
    """One window: several sessions write it and each only knows its own last
    response, so an older snapshot of the same window must not win."""
    if not new or new.get("pct") is None:
        return old
    if not old or old.get("pct") is None:
        return new
    o, n = old.get("resets_at"), new.get("resets_at")
    if o is not None and o <= now:
        return new  # the stored window is over
    if o is None or n is None:
        return {"pct": max(old["pct"], new["pct"]), "resets_at": n if n is not None else o}
    if n > o:
        return new  # a new window started
    if n < o:
        return old  # a stale snapshot
    return {"pct": max(old["pct"], new["pct"]), "resets_at": o}  # same window: usage only grows


def write_json(path, data):
    fd, tmp = tempfile.mkstemp(prefix=".td.", suffix=".tmp", dir=USAGE_DIR)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def config_file():
    d = os.environ.get("CLAUDE_CONFIG_DIR")
    return os.path.join(d, ".claude.json") if d else os.path.join(os.path.expanduser("~"), ".claude.json")


def account():
    """The logged-in account {"id", "email", "org"} from Claude Code's config,
    cached by the file's mtime and size (it is a few hundred KB)."""
    path = config_file()
    try:
        st = os.stat(path)
    except OSError:
        return {}
    stamp = [path, st.st_mtime, st.st_size]
    cache = read_json(ACCOUNT_CACHE) or {}
    if cache.get("stamp") == stamp:
        return cache.get("account") or {}
    a = (read_json(path) or {}).get("oauthAccount") or {}
    acct = {"id": a.get("accountUuid") or a.get("emailAddress") or "", "email": a.get("emailAddress") or "",
            "org": a.get("organizationName") or ""}
    write_json(ACCOUNT_CACHE, {"stamp": stamp, "account": acct})
    return acct


def save(payload, now):
    os.makedirs(USAGE_DIR, exist_ok=True)
    acct = account()
    aid = acct.get("id") or "-"
    store = read_json(LIMITS_FILE) or {}
    if "accounts" not in store:  # the first format: one set of windows, whoever's
        store = {"accounts": {}}
    accounts = store["accounts"]
    entry = accounts.get(aid) or {}
    limits = entry.get("limits") or {}

    rl = payload.get("rate_limits") or {}
    new = {}
    for w in WINDOWS:
        d = rl.get(w) or {}
        if d.get("used_percentage") is not None:
            new[w] = {"pct": float(d["used_percentage"]), "resets_at": epoch(d.get("resets_at"))}
    if new:
        merged = {w: merge_window(limits.get(w), new.get(w), now) for w in WINDOWS}
        limits = {w: v for w, v in merged.items() if v}
    entry = {"email": acct.get("email", ""), "org": acct.get("org", ""), "limits": limits}
    if accounts.get(aid) != entry or store.get("current") != aid:
        accounts[aid] = entry
        store["current"] = aid
        write_json(LIMITS_FILE, store)

    sid = str(payload.get("session_id") or "")
    cw = payload.get("context_window") or {}
    if sid and cw.get("used_percentage") is not None:
        cwd = (payload.get("workspace") or {}).get("current_dir") or payload.get("cwd") or ""
        model = payload.get("model") or {}
        size = cw.get("context_window_size") or cw.get("size") or 0
        pct = float(cw["used_percentage"])
        session = {"ctx_pct": pct, "ctx_window": int(size or 0),
                   "ctx_tokens": int(cw.get("total_tokens") or (size * pct / 100 if size else 0)),
                   "project": os.path.basename(cwd.rstrip("\\/")),
                   "model": model.get("id") or model.get("display_name") or "", "account": aid}
        # Same name as the hook's session file (touchdeck_agent.py run_hook).
        name = hashlib.sha1(sid[:64].encode()).hexdigest()[:16] + ".json"
        path = os.path.join(USAGE_DIR, name)
        if read_json(path) != session:
            write_json(path, session)
        else:
            os.utime(path)  # still open: keep it fresh
    return limits


def main():
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        payload = {}
    try:
        limits = save(payload, time.time())
    except Exception:  # never break the status line
        limits = {}
    pct = lambda w: "%.0f%%" % limits[w]["pct"] if w in limits else "--"  # noqa: E731
    model = (payload.get("model") or {}).get("display_name") or "Claude"
    cwd = (payload.get("workspace") or {}).get("current_dir") or ""
    ctx = (payload.get("context_window") or {}).get("used_percentage")
    ctx_txt = "%.0f%%" % ctx if ctx is not None else "--"
    print("%s | %s | ctx:%s | 5h:%s wk:%s" % (model, os.path.basename(cwd), ctx_txt, pct("five_hour"), pct("seven_day")))


if __name__ == "__main__":
    main()
