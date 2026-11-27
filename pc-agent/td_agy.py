#!/usr/bin/env python3
"""
Antigravity's quota while agy only runs headless (agy -p): print mode never runs
the status line, so td_agy_statusline.py would keep an old reading. An
interactive agy hands the status line its quota a second or two after it
starts, before any message (so it spends nothing). This opens agy for a moment
in a pseudo-terminal, lets td_agy_statusline.py save the quota, and closes it:

  - after agy was used: a conversation (agy -p, the CLI, or the Antigravity
    app) newer than the last reading, quiet for QUIET_S;
  - when there is no reading yet.

Only when agy's status line is td_agy_statusline.py (settings.json) and on
macOS / Linux (a pty). touchdeck_agent.py starts it.
"""
import glob
import json
import os
import select
import shutil
import signal
import subprocess
import threading
import time

HOME = os.path.expanduser("~")
USAGE_FILE = os.path.join(HOME, ".touchdeck", "agy_usage.json")
SETTINGS = os.path.join(HOME, ".gemini", "antigravity-cli", "settings.json")
CONVERSATIONS = (os.path.join(HOME, ".gemini", "antigravity-cli", "conversations"),
                 os.path.join(HOME, ".gemini", "antigravity", "conversations"))
CHECK_EVERY_S = 60
QUIET_S = 20        # a conversation still being written: wait for it to end
MIN_GAP_S = 120     # between two refreshes
RUN_S = 15          # how long agy may take to hand over the quota


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def reading_at():
    data = read_json(USAGE_FILE)
    return int(data.get("at") or 0) if isinstance(data, dict) else 0


def last_use():
    """mtime of the newest conversation, 0 = none."""
    newest = 0.0
    for folder in CONVERSATIONS:
        for path in glob.glob(os.path.join(folder, "*.db")):
            try:
                newest = max(newest, os.path.getmtime(path))
            except OSError:
                pass
    return newest


def find_agy():
    """agy on PATH, else Homebrew's (a LaunchAgent's PATH may lack it)."""
    found = shutil.which("agy")
    if found:
        return found
    for path in ("/opt/homebrew/bin/agy", "/usr/local/bin/agy", os.path.join(HOME, ".local", "bin", "agy")):
        if os.access(path, os.X_OK):
            return path
    return None


def settings():
    data = read_json(SETTINGS)
    return data if isinstance(data, dict) else {}


def ours(conf):
    return "td_agy_statusline.py" in str((conf.get("statusLine") or {}).get("command") or "")


def workdir(conf):
    """A trusted workspace (no trust prompt in the way), else home."""
    for d in conf.get("trustedWorkspaces") or []:
        if isinstance(d, str) and os.path.isdir(d):
            return d
    return HOME


def refresh(agy, cwd):
    """Opens agy in a pty until the status line wrote a new reading. True if it did."""
    import pty  # POSIX only
    before = reading_at()
    master, slave = pty.openpty()
    env = dict(os.environ, TERM="xterm-256color")
    p = subprocess.Popen([agy], stdin=slave, stdout=slave, stderr=slave, cwd=cwd, env=env,
                         start_new_session=True, close_fds=True)
    os.close(slave)
    ok = False
    try:
        end = time.monotonic() + RUN_S
        while time.monotonic() < end and p.poll() is None:
            ready, _, _ = select.select([master], [], [], 0.3)
            if ready:
                try:
                    os.read(master, 65536)  # drain it, or agy blocks on a full terminal
                except OSError:
                    break
            if reading_at() != before:
                ok = True
                break
    finally:
        for sig, wait in ((signal.SIGTERM, 3), (signal.SIGKILL, 3)):
            if p.poll() is not None:
                break
            try:
                os.killpg(p.pid, sig)
            except OSError:
                pass
            try:
                p.wait(wait)
            except subprocess.TimeoutExpired:
                pass
        os.close(master)
    return ok


class AgyQuota(threading.Thread):
    def __init__(self, verbose=False):
        super().__init__(daemon=True, name="td_agy")
        self.verbose = verbose
        self.tried = 0.0

    def run(self):
        while True:
            try:
                self.check()
            except Exception as e:  # never stop watching
                if self.verbose:
                    print("agy: %s" % e, flush=True)
            time.sleep(CHECK_EVERY_S)

    def check(self):
        if os.name == "nt":
            return
        agy = find_agy()
        conf = settings()
        if not agy or not ours(conf):
            return
        now = time.time()
        at, used = reading_at(), last_use()
        due = not at or (used > at and now - used > QUIET_S)
        if not due or now - self.tried < MIN_GAP_S:
            return
        self.tried = now
        ok = refresh(agy, workdir(conf))
        if self.verbose:
            print("agy: quota %s" % ("refreshed" if ok else "not refreshed"), flush=True)


def available():
    return os.name != "nt" and bool(find_agy()) and ours(settings())


if __name__ == "__main__":
    conf = settings()
    print("refreshed" if available() and refresh(find_agy(), workdir(conf)) else "not refreshed")
