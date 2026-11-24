#!/usr/bin/env python3
"""
The pinned plan on the deck's Bench app: what the tps-plan-board mod shows in
the terminal, without opening it. A plan is a PLANS/*.md with a "## Progreso"
(or "## Progress") checklist, "- [ ]" to do, "- [~]" doing, "- [x]" done,
grouped by its "###" headings. The session implementing it (any LLM, any
terminal) ticks the file; the agent sees the change and sends the checklist to
the Bench app as a read-only plan sheet: the deck shows the step in progress
(the first "[~]", else the first "[ ]") and the progress, and taps don't move
it. Only pinning opens the app; later updates refresh it in place.

    python touchdeck_agent.py plan              # the newest plan in this repo's PLANS/
    python touchdeck_agent.py plan funding      # the newest one whose file name has "funding"
    python touchdeck_agent.py plan PLANS/x.md   # that file
    python touchdeck_agent.py plan off          # unpin

The pin is ~/.touchdeck/plan.json; the running agent checks it and the plan's
mtime every few seconds (a stat, nothing more while nothing changes).
"""
import json
import os
import re
import threading
import time
import urllib.error
import urllib.request

PLAN_PIN = os.path.join(os.path.expanduser("~"), ".touchdeck", "plan.json")
PLANS_DIR = "PLANS"
SKIP = ("-e2e-retest.md",)
PROGRESS = re.compile(r"^##\s+(progreso|progress)\b", re.I)
CHECK_EVERY_S = 3
RETRY_S = 30          # the deck didn't take it: try again this much later
MAX_TEXT = 8000       # the Bench app's limit is 8 KB (BENCH_MAX_TEXT)
MAX_TITLE = 47
MARK = {"t": "[ ]", "d": "[~]", "x": "[x]"}


def clip(text, max_bytes):
    return (text or "").encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


def plan_steps(text):
    """[(state, group, text)] of "## Progreso", state "t" todo, "d" doing, "x" done; None without it."""
    lines = text.split("\n")
    start = next((i for i, l in enumerate(lines) if PROGRESS.match(l)), None)
    if start is None:
        return None
    steps, group = [], ""
    for line in lines[start + 1:]:
        if re.match(r"^#{1,2}\s", line):
            break
        h = re.match(r"^###\s+(.*)$", line)
        if h:
            group = h.group(1).strip()
            continue
        m = re.match(r"^\s*[-*] \[( |x|X|~)\]\s+(.*)$", line)
        if m:
            steps.append(({" ": "t", "~": "d"}.get(m.group(1), "x"), group, m.group(2).strip()))
    return steps


def plan_title(path, text):
    return next((l[2:].strip() for l in text.split("\n") if l.startswith("# ")),
                os.path.splitext(os.path.basename(path))[0])


def sheet_text(steps):
    """The checklist as a Bench sheet: "# group" headings, "[x] step" lines, markdown marks gone."""
    out, group = [], None
    for state, g, txt in steps:
        if g != group and g:
            out.append("# " + g)
        group = g
        out.append("%s %s" % (MARK[state], re.sub(r"(\*\*|__|`)", "", txt)))
    text = "\n".join(out)
    while len(text.encode("utf-8")) > MAX_TEXT and "\n" in text:  # whole lines off the end
        text = text.rsplit("\n", 1)[0]
    return text


def find_plans(cwd):
    """The PLANS/*.md of cwd's repo (cwd or a parent), newest first."""
    d = os.path.abspath(cwd)
    for _ in range(6):
        folder = os.path.join(d, PLANS_DIR)
        if os.path.isdir(folder):
            paths = [os.path.join(folder, n) for n in os.listdir(folder) if n.endswith(".md") and not n.endswith(SKIP)]
            return sorted((p for p in paths if os.path.isfile(p)), key=os.path.getmtime, reverse=True)
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return []


def read_pin():
    try:
        with open(PLAN_PIN, encoding="utf-8") as f:
            pin = json.load(f)
        return pin if isinstance(pin, dict) and pin.get("path") else None
    except (OSError, ValueError):
        return None


def pin_plan(arg, cwd):
    """touchdeck_agent.py plan [file | part of its name | off]: what it did, as a line."""
    arg = (arg or "").strip()
    if arg.lower() in ("off", "none", "unpin"):
        try:
            os.remove(PLAN_PIN)
        except FileNotFoundError:
            pass
        return "deck plan: unpinned (the Bench app keeps the last sheet)"
    path = os.path.abspath(os.path.join(cwd, os.path.expanduser(arg))) if arg else ""
    if not os.path.isfile(path):
        plans = find_plans(cwd)
        if arg:
            plans = [p for p in plans if arg.lower() in os.path.basename(p).lower()]
        if not plans:
            raise SystemExit("deck plan: no plan in %s/ %s" % (PLANS_DIR, 'matching "%s"' % arg if arg else "here"))
        path = plans[0]
    with open(path, encoding="utf-8") as f:
        steps = plan_steps(f.read())
    if steps is None:
        raise SystemExit('deck plan: %s has no "## Progreso" checklist' % path)
    os.makedirs(os.path.dirname(PLAN_PIN), mode=0o700, exist_ok=True)
    with open(PLAN_PIN + ".tmp", "w", encoding="utf-8") as f:
        json.dump({"path": path, "at": time.time()}, f)  # "at": a re-pin of the same plan opens it again
    os.replace(PLAN_PIN + ".tmp", PLAN_PIN)
    done = sum(1 for s in steps if s[0] == "x")
    return "deck plan: %s (%d/%d done); the agent sends it within %d s" % (path, done, len(steps), CHECK_EVERY_S)


class PlanWatch(threading.Thread):
    """Sends the pinned plan to the Bench app when the pin or the plan file changes."""

    def __init__(self, deck_addr, pc_id, verbose=False):
        super().__init__(daemon=True, name="td_plan")
        self.deck_addr, self.pc_id, self.verbose = deck_addr, pc_id, verbose
        self.sent_pin = None   # the pin last sent ({"path", "at"})
        self.sent_key = None   # (path, mtime) last sent
        self.retry_at = 0.0
        self.started = time.time()  # a pin from before the agent started is refreshed quietly

    def run(self):
        while True:
            try:
                self.check()
            except Exception as e:  # never stop watching
                if self.verbose:
                    print("plan: %s" % e, flush=True)
            time.sleep(CHECK_EVERY_S)

    def check(self):
        pin = read_pin()
        if not pin:
            self.sent_pin = self.sent_key = None
            return
        path = pin["path"]
        try:
            key = (path, os.path.getmtime(path))
        except OSError:
            return  # gone (a branch switch?): keep the deck as it is
        new_pin = pin != self.sent_pin
        if not new_pin and key == self.sent_key:
            return
        if time.monotonic() < self.retry_at and not new_pin:
            return
        with open(path, encoding="utf-8") as f:
            text = f.read()
        steps = plan_steps(text) or []
        quiet = not new_pin or (self.sent_pin is None and (pin.get("at") or 0) < self.started)
        ok, msg = self.send(clip(plan_title(path, text), MAX_TITLE), sheet_text(steps), quiet)
        if ok:
            self.sent_pin, self.sent_key = pin, key
            if self.verbose:
                print("plan: sent %s (%d steps%s)" % (os.path.basename(path), len(steps), ", quiet" if quiet else ""),
                      flush=True)
        else:
            self.retry_at = time.monotonic() + RETRY_S
            if self.verbose:
                print("plan: not sent: %s" % msg, flush=True)

    def send(self, title, text, quiet):
        if not text:
            return True, "empty checklist"
        addr = self.deck_addr()
        if addr == "255.255.255.255":
            return False, "deck not found yet"
        body = json.dumps({"pc_id": self.pc_id, "title": title, "text": text, "plan": True, "quiet": quiet},
                          ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request("http://%s/api/pc/bench" % addr, data=body,
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                r.read()
            return True, "ok"
        except urllib.error.HTTPError as e:
            return False, "HTTP %d" % e.code
        except (urllib.error.URLError, OSError) as e:
            return False, str(getattr(e, "reason", e))
