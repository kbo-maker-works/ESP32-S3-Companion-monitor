#!/usr/bin/env python3
"""
CompaDeck PC agent
==================
Feeds the CompaDeck (Guition JC3248W535) from this PC over WiFi:

  * PC metrics for the metric tiles: CPU, RAM, NVIDIA GPU (nvidia-smi), disk,
    network. One UDP packet a second to the deck's port 4210 (V2.1 protocol,
    see src/services/pc_metrics.h in the firmware).
  * Claude Code's state for the "Mascota Claude" widget: thinking, coding,
    waiting for you, turn done, failed, asleep. It comes from Claude Code hooks
    that run this same script as `touchdeck_agent.py hook` (see README.md).
  * GitLab and Jira for the Work app (td_work.py), set up on the settings page
    (td_portal.py, http://127.0.0.1:4213) or in ~/.touchdeck/work.json.
    Runs on macOS and Linux too (Now playing / PC control: Windows only).

Usage
-----
  python touchdeck_agent.py                     # deck at companion.local
  python touchdeck_agent.py --host 192.168.1.225
  python touchdeck_agent.py --once --verbose    # build one packet and print it
  python touchdeck_agent.py hook                # Claude Code hook (reads stdin)
  python touchdeck_agent.py settings            # open the settings page (127.0.0.1:4213)

Only standard library + psutil. Nothing is sent anywhere but the deck.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import zlib

DECK_PORT = 4210
SEND_EVERY_S = 1.0
PACKET_MAX = 1400   # the deck's WiFiUDP reads 1460 bytes at most; under it, no IP fragments either
RESOLVE_EVERY_S = 60.0

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "touchdeck")
SESSIONS_DIR = os.path.join(STATE_DIR, "sessions")
EVENT_FILE = os.path.join(STATE_DIR, "event.json")
USAGE_DIR = os.path.join(STATE_DIR, "usage")   # written by td_statusline.py (or the touchdeck-usage mod)
LIMITS_FRESH_S = 3 * 24 * 3600   # plan windows: the last reading, if not ancient
CTX_FRESH_S = 6 * 3600           # a session idle longer no longer shows its context

EVENT_SHOW_S = 30      # an event (input / done / fail) is sent this long
SESSION_IDLE_S = 600   # a session quiet this long no longer counts as working
SLEEP_AFTER_S = 900    # no Claude Code activity this long: the mascot sleeps

# Firmware field sizes (bytes incl. the terminator): see ClaudeLive / PcMetric.
MAX_STATUS, MAX_EV_TXT, MAX_EV_SUB = 31, 31, 39
MAX_ACCT, MAX_ORG = 39, 31  # ClaudeUsage::acct / org


def clip(text, max_bytes):
    """Cuts UTF-8 text to max_bytes without splitting a character."""
    b = (text or "").encode("utf-8")[:max_bytes]
    return b.decode("utf-8", "ignore")


def write_json_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------
# Claude Code hook: record what just happened, quickly, and never fail the hook
# ---------------------------------------------------------------------------
def tool_summary(name, tool_input):
    """'Bash: pio run', 'Edit: main.cpp' ... short enough for the deck."""
    ti = tool_input if isinstance(tool_input, dict) else {}
    detail = ""
    if name in ("Bash", "PowerShell"):
        detail = (ti.get("description") or ti.get("command") or "").splitlines()[0:1]
        detail = detail[0] if detail else ""
    elif name in ("Edit", "Write", "Read", "NotebookEdit", "MultiEdit"):
        detail = os.path.basename(ti.get("file_path") or ti.get("notebook_path") or "")
    elif name in ("Grep", "Glob"):
        detail = ti.get("pattern") or ""
    elif name in ("WebFetch", "WebSearch"):
        detail = ti.get("url") or ti.get("query") or ""
    elif name in ("Agent", "Task"):
        detail = ti.get("description") or ""
    name = name.replace("mcp__", "").split("__")[-1]
    return clip("%s: %s" % (name, detail) if detail else name, MAX_STATUS)


def tool_failed(resp):
    """Best effort: tool responses differ per tool."""
    if not isinstance(resp, dict):
        return False
    if resp.get("is_error") or resp.get("interrupted"):
        return True
    for key in ("exitCode", "exit_code", "returnCode", "return_code"):
        code = resp.get(key)
        if isinstance(code, int) and code != 0:
            return True
    return False


TITLE_TAIL = 256 * 1024  # bytes read from the end of the transcript on each hook


def session_title(transcript, whole=False):
    """The name given to the session with /rename: the transcript's latest
    "custom-title" entry (Claude Code writes it again every so often, so the
    end of the file has it). None when what was read has none."""
    if not transcript:
        return None
    try:
        with open(transcript, "rb") as f:
            f.seek(0, os.SEEK_END)
            start = 0 if whole else max(0, f.tell() - TITLE_TAIL)
            f.seek(start)
            lines = f.read().split(b"\n")
    except OSError:
        return None
    if start:
        lines = lines[1:]  # cut in the middle of a line
    for line in reversed(lines):
        if b'"custom-title"' not in line:
            continue
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if isinstance(entry, dict) and entry.get("type") == "custom-title":
            return str(entry.get("customTitle") or "").strip()
    return None


def run_hook():
    try:
        payload = json.loads(sys.stdin.buffer.read().decode("utf-8-sig"))
    except (ValueError, UnicodeDecodeError, AttributeError):
        return
    if not isinstance(payload, dict):
        return
    event = payload.get("hook_event_name") or ""
    # A prompt waiting on the deck is over once its tool ran, or the turn ended
    # or a new one started (td_ask.py).
    if event in ("PostToolUse", "PostToolUseFailure", "UserPromptSubmit", "Stop", "SessionEnd"):
        try:
            import td_ask
            tool = (payload.get("tool_name") or "") if event.startswith("PostToolUse") else None
            td_ask.dismiss(payload.get("session_id") or "", tool, payload.get("tool_input") if tool else None)
        except Exception:
            pass
    sid = clip(payload.get("session_id") or "default", 64)
    project = os.path.basename((payload.get("cwd") or "").rstrip("\\/")) or "Claude Code"
    path = os.path.join(SESSIONS_DIR, "%s.json" % hashlib.sha1(sid.encode()).hexdigest()[:16])
    now = time.time()

    if event == "SessionEnd":
        # Its state and its context (td_statusline.py names it the same way).
        for p in (path, os.path.join(USAGE_DIR, os.path.basename(path))):
            try:
                os.remove(p)
            except OSError:
                pass
        return

    s = read_json(path) or {}
    s.update(project=project, t=now)
    # The session's name when it has one (/rename), else its folder. The whole
    # transcript is read once per session, when its end has no name yet.
    transcript = payload.get("transcript_path") or ""
    title = session_title(transcript)
    if title is None and not s.get("title_scanned"):
        title = session_title(transcript, whole=True) or ""
        s["title_scanned"] = True
    if title is not None:
        s["title"] = title
    name = s.get("title") or project
    if "start" not in s:
        s["start"] = now
    ev = None

    def set_state(state, detail=""):
        # The session list on the deck: working / input / done / idle, since when.
        if s.get("state") != state:
            s["since"] = now
        s.update(state=state, detail=detail)

    if event == "SessionStart":
        if payload.get("source") == "compact":  # compacted in the middle of a turn
            s.update(mood="thinking", status="Thinking...", working=True)
            set_state("working", "Compacting")
        else:
            s.update(mood="happy", status="", working=False)
            set_state("idle")
    elif event == "UserPromptSubmit":
        s.update(mood="thinking", status="Thinking...", working=True)
        set_state("working", "Thinking...")
    elif event == "PreToolUse":
        status = tool_summary(payload.get("tool_name") or "", payload.get("tool_input"))
        s.update(mood="coding", status=status, working=True)
        set_state("working", status)
    elif event in ("PostToolUse", "PostToolUseFailure"):
        s.update(mood="thinking", working=True)
        set_state("working", s.get("detail") or "Thinking...")
        if event == "PostToolUseFailure" or tool_failed(payload.get("tool_response")):
            tool = (payload.get("tool_name") or "Tool").replace("mcp__", "").split("__")[-1]
            ev = ("fail", "%s failed!" % tool, name)
    elif event == "Notification":
        msg = (payload.get("message") or "").lower()
        kind = payload.get("notification_type") or ""
        permission = kind == "permission_prompt" or "permission" in msg or "permiso" in msg
        idle = kind == "idle_prompt" or "waiting for your input" in msg
        if idle and s.get("state") in ("done", "idle"):
            pass  # a finished turn reminding you: it stays done
        else:
            sub = "needs permission" if permission else "waiting for you"
            s.update(working=False)
            set_state("input", sub.capitalize())
            ev = ("input", name, sub)
    elif event in ("Stop", "SubagentStop"):
        if event == "Stop":
            s.update(mood="happy", status="", working=False)
            set_state("done")
            ev = ("done", name, "turn done")
    elif event == "PermissionRequest":
        # The terminal's dialog is up; its Notification brings the mascot's event.
        ask = payload.get("tool_name") == "AskUserQuestion"
        s.update(working=False)
        set_state("input", "Has a question" if ask else "Needs permission")
    write_json_atomic(path, s)
    if ev:
        write_json_atomic(EVENT_FILE, {
            "ev": ev[0], "ev_txt": clip(ev[1], MAX_EV_TXT), "ev_sub": clip(ev[2], MAX_EV_SUB),
            "ev_id": zlib.crc32(("%s|%s|%f" % (sid, ev[0], now)).encode()) or 1, "t": now,
        })
    if event == "PermissionRequest":
        # Blocks while the prompt waits on the deck (a hook of its own in
        # settings.json, not async, with a long timeout: README.md).
        import td_ask
        td_ask.run_permission_hook(payload, name)


# ---------------------------------------------------------------------------
# Claude state for the mascot, merged from every session's file
# ---------------------------------------------------------------------------
STALE_FILE_S = 24 * 3600  # a session or usage file untouched this long is left over


def remove_quietly(path):
    try:
        os.remove(path)
    except OSError:
        pass


def claude_state(now):
    sessions = []
    try:
        names = os.listdir(SESSIONS_DIR)
    except OSError:
        names = []
    for name in names:
        if name.endswith(".json"):
            path = os.path.join(SESSIONS_DIR, name)
            s = read_json(path)
            try:
                t = float(s.get("t") or 0) if isinstance(s, dict) else 0.0
            except (TypeError, ValueError):
                t = 0.0
            if now - t < STALE_FILE_S:
                s["t"] = t
                sessions.append(s)
            else:  # a session that crashed or was killed never ran SessionEnd
                remove_quietly(path)

    latest = max(sessions, key=lambda s: s.get("t", 0)) if sessions else None
    working = [s for s in sessions if s.get("working") and now - s.get("t", 0) < SESSION_IDLE_S]
    if not latest or now - latest.get("t", 0) > SLEEP_AFTER_S:
        mood, status = "sleeping", ""
    elif len(working) >= 2:
        mood, status = "hyped", "%d sessions at once" % len(working)
    elif working:
        w = max(working, key=lambda s: s.get("t", 0))
        mood, status = w.get("mood") or "thinking", w.get("status") or ""
    else:
        mood, status = "happy", ""
    out = {"mood": mood, "status": clip(status, MAX_STATUS)}

    ev = read_json(EVENT_FILE)
    if isinstance(ev, dict) and now - float(ev.get("t", 0)) < EVENT_SHOW_S:
        out.update({k: ev.get(k) for k in ("ev", "ev_txt", "ev_sub", "ev_id")})
    out.update(usage_state(now))
    out["ss"] = session_list(sessions, now)
    return out


# The deck's session list (AI app): [name, state, seconds in that state, detail]
# with state w (working), i (waiting for you), d (turn done), z (idle). Waiting
# ones first, then working, done, idle; newest first inside each group.
SESSION_ORDER = {"i": 0, "w": 1, "d": 2, "z": 3}
SESSION_LIST_MAX = 6
SESSION_SHOW_S = 12 * 3600   # older sessions (closed without SessionEnd) drop off
STALE_WORKING_S = 1800       # "working" with no hook for this long: it is not


def session_list(sessions, now):
    rows = []
    for s in sessions:
        t = float(s.get("t", 0))
        if now - t > SESSION_SHOW_S:
            continue
        state = s.get("state") or ("working" if s.get("working") else "done")
        code = {"working": "w", "input": "i", "done": "d"}.get(state, "z")
        if code == "w" and now - t > STALE_WORKING_S:
            code = "z"
        detail = s.get("detail") or ""
        rows.append([s.get("title") or s.get("project") or "Claude Code", code, int(now - float(s.get("since") or t)),
                     detail if code in ("w", "i") else "", float(s.get("start") or t), t])
    # Same name twice: number them by start time ("desktop-companion 2").
    seen = {}
    for r in sorted(rows, key=lambda r: r[4]):
        seen[r[0]] = seen.get(r[0], 0) + 1
        if seen[r[0]] > 1:
            r[0] = "%s %d" % (r[0], seen[r[0]])
    rows.sort(key=lambda r: (SESSION_ORDER[r[1]], -r[5]))
    return [[clip(r[0], 23), r[1], max(0, r[2]), clip(r[3], 31)] for r in rows[:SESSION_LIST_MAX]]


# ---------------------------------------------------------------------------
# Plan usage and context, from the touchdeck-usage mod's files (one per session)
# ---------------------------------------------------------------------------
def iso_epoch(text):
    """ISO text or Unix seconds (td_statusline.py) -> Unix seconds, 0 = unknown."""
    if not text:
        return 0
    if isinstance(text, (int, float)):
        return int(text)
    try:
        return int(datetime.datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp())
    except ValueError:
        return 0


def pretty_model(model):
    """'claude-opus-5-5[1m]' -> 'Opus 5.5 1M'; anything else as it came."""
    m = re.match(r"(?:claude-)?([a-z]+)-(\d+)(?:-(\d{1,2}))?(?:-\d{8})?(\[1m\])?$", model or "")
    if not m:
        return clip(model or "", 19)
    name = m.group(1).capitalize() + " " + m.group(2) + ("." + m.group(3) if m.group(3) else "")
    return clip(name + (" 1M" if m.group(4) else ""), 19)


def usage_state(now):
    out = {}
    limits_done = False
    # td_statusline.py: the windows per account, and which one is logged in.
    store = read_json(os.path.join(USAGE_DIR, "limits.json"))
    if isinstance(store, dict) and isinstance(store.get("accounts"), dict):
        limits_done = True
        entry = store["accounts"].get(store.get("current")) or {}
        out["acct"] = clip(entry.get("email") or "", MAX_ACCT)
        out["org"] = clip(entry.get("org") or "", MAX_ORG)
        limits = entry.get("limits") or {}
        for kind, key in (("five_hour", "q5"), ("seven_day", "q7")):
            w = limits.get(kind)
            if isinstance(w, dict) and w.get("pct") is not None:
                out[key] = round(float(w["pct"]), 1)
                out[key + "r"] = iso_epoch(w.get("resets_at"))
            else:
                out[key] = -1  # this account has none yet: not the previous account's
    files = []
    try:
        names = os.listdir(USAGE_DIR)
    except OSError:
        return {}
    for name in names:
        if name.endswith(".json") and name not in ("limits.json", "account.json"):
            path = os.path.join(USAGE_DIR, name)
            try:
                mtime = os.path.getmtime(path)
            except OSError:  # its session just ended
                continue
            if now - mtime > STALE_FILE_S:  # left by a session that never ended cleanly
                remove_quietly(path)
            else:
                files.append((mtime, path))
    files.sort(reverse=True)
    ctx_done = False
    for mtime, path in files:
        if limits_done and ctx_done:
            break
        u = read_json(path)
        if not isinstance(u, dict):
            continue
        limits = u.get("limits") or {}
        # The windows are the account's: the newest file that has them.
        if not limits_done and limits and now - mtime < LIMITS_FRESH_S:
            limits_done = True
            for kind, key in (("five_hour", "q5"), ("seven_day", "q7")):
                w = limits.get(kind)
                if isinstance(w, dict) and w.get("pct") is not None:
                    out[key] = round(float(w["pct"]), 1)
                    out[key + "r"] = iso_epoch(w.get("resets_at"))
        # The context is a session's: the newest one still open.
        if not ctx_done and not u.get("ended") and now - mtime < CTX_FRESH_S and u.get("ctx_pct") is not None:
            ctx_done = True
            out.update(ctx=round(float(u["ctx_pct"]), 1), ctx_tok=int(u.get("ctx_tokens") or 0),
                       ctx_win=int(u.get("ctx_window") or 0), proj=clip(u.get("project") or "", 23),
                       model=pretty_model(u.get("model")))
    if not ctx_done:
        out["ctx"] = -1
    return out


# ---------------------------------------------------------------------------
# PC metrics
# ---------------------------------------------------------------------------
class Gpu:
    """NVIDIA GPU through one long-running `nvidia-smi -l 1` (no process per sample)."""
    FIELDS = "utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw,fan.speed,clocks.gr"

    def __init__(self):
        self.values = None
        exe = shutil.which("nvidia-smi")
        if not exe:
            return
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        try:
            self.proc = subprocess.Popen(
                [exe, "--query-gpu=" + self.FIELDS, "--format=csv,noheader,nounits", "-l", "1"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, creationflags=flags)
        except OSError:
            return
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            vals = []
            for p in line.split(",")[:7]:
                try:
                    vals.append(float(p))
                except ValueError:
                    vals.append(None)  # "[N/A]" (a laptop's power, a fanless card): that field only
            if len(vals) < 7 or None in vals[:4]:
                continue
            util, temp, used, total, power, fan, clock = vals
            self.values = (util, temp, 100.0 * used / total if total else 0.0, used / 1024.0, power, fan, clock,
                           total / 1024.0)


class Metrics:
    BATTERY_EVERY_S = 30  # it moves slowly; reading it costs a few ms on macOS

    def __init__(self):
        import psutil  # noqa: imported here so `hook` runs without it
        self.ps = psutil
        self.ps.cpu_percent(interval=None)  # first call primes the counter
        self.net = self.ps.net_io_counters()
        self.net_t = time.monotonic()
        self.gpu = Gpu()
        self.hwmon = None
        if os.name == "nt":
            import td_hwmon  # RTSS and MSI Afterburner, read only while they run
            self.hwmon = td_hwmon
        self.disk_path = os.environ.get("SystemDrive", "C:") + "\\" if os.name == "nt" else "/"
        # macOS: "/" is the sealed system volume (a few GB); the files live on Data.
        if sys.platform == "darwin" and os.path.isdir("/System/Volumes/Data"):
            self.disk_path = "/System/Volumes/Data"
        self.batt, self.batt_t = None, -self.BATTERY_EVERY_S

    def _disk(self):
        d = self.ps.disk_usage(self.disk_path)
        if sys.platform == "darwin":  # as the Mac shows it: the APFS container's free space against its size
            return 100.0 * (d.total - d.free) / d.total if d.total else 0.0
        return d.percent

    def _battery(self):
        """The battery's charge (%), None without one."""
        if time.monotonic() - self.batt_t >= self.BATTERY_EVERY_S:
            self.batt_t = time.monotonic()
            try:
                b = self.ps.sensors_battery()
                self.batt = float(b.percent) if b else None
            except (AttributeError, OSError, RuntimeError):
                self.batt = None
        return self.batt

    def read(self):
        ps = self.ps
        mem = ps.virtual_memory()
        net, t = ps.net_io_counters(), time.monotonic()
        dt = max(t - self.net_t, 0.001)
        down = (net.bytes_recv - self.net.bytes_recv) / 1024.0 / dt
        up = (net.bytes_sent - self.net.bytes_sent) / 1024.0 / dt
        self.net, self.net_t = net, t
        # Stable ids: deck tiles bind to them. KB/s go x10 (the firmware divides by 10).
        # A fifth value is the metric's full scale ("max"), for an amount the
        # deck can't scale by its unit: a GB bar fills against the total.
        m = [
            (1, "CPU", "%", ps.cpu_percent(interval=None)),
            (2, "RAM", "%", mem.percent),
            (3, "RAM USED", "GB", mem.used / 1024 ** 3, mem.total / 1024 ** 3),
            (8, "DISK", "%", self._disk()),
            (9, "DOWN", "KB/s", round(down * 10)),
            (10, "UP", "KB/s", round(up * 10)),
        ]
        if self.gpu.values:
            util, temp, vram, vram_gb, power, fan, clock, vram_total = self.gpu.values
            m += [(4, "GPU", "%", util), (5, "GPUT", "C", temp), (6, "VRAM", "%", vram),
                  (18, "VRAM USED", "GB", vram_gb, vram_total)]
            m += [x for x in ((7, "GPUW", "W", power), (14, "GPU FAN", "%", fan), (15, "GPU CLOCK", "MHz", clock))
                  if x[3] is not None]
        hw = self.hwmon.read() if self.hwmon else {}
        for mid, name, unit, key in ((12, "FPS", "FPS", "fps"), (13, "FRAMETIME", "ms", "frametime"),
                                     (16, "CPUT", "C", "cpu_temp"), (17, "CPUW", "W", "cpu_power")):
            if key in hw:
                m.append((mid, name, unit, hw[key]))
        batt = self._battery()
        if batt is not None:  # laptops: the deck's "PC stats" widget rings it
            m.append((11, "BATT", "%", batt))
        m.sort()
        out = []
        for i, n, u, v, *full in m:
            o = {"id": i, "name": n, "unit": u, "value": round(float(v), 1)}
            if full and full[0]:
                o["max"] = round(float(full[0]), 1)
            out.append(o)
        return out


# ---------------------------------------------------------------------------
# Sending
# ---------------------------------------------------------------------------
def pc_identity():
    host = platform.node() or "PC"
    pc_id = hashlib.sha1(host.encode()).hexdigest()[:12]
    # macOS says "Name.local": cut to 23 that left "Name." on the deck. The id keeps the full name.
    name = host[:-6] if host.lower().endswith(".local") and len(host) > 6 else host
    return pc_id, clip(name, 23)


BROADCAST = "255.255.255.255"


class Target:
    """The deck's address, resolved by name now and then (from several threads).
    A failed lookup keeps the last good address and is retried RESOLVE_EVERY_S
    later, not on every call: an mDNS miss can block for seconds. Never resolved
    yet: BROADCAST (the main loop then sends only what anyone may see)."""

    def __init__(self, host):
        self.host, self.addr, self.when = host, None, -RESOLVE_EVERY_S
        self.lock = threading.Lock()

    def get(self):
        with self.lock:
            every = RESOLVE_EVERY_S if self.addr else 10.0  # not found yet (booting, network down): sooner
            if time.monotonic() - self.when < every:
                return self.addr or BROADCAST
            self.when = time.monotonic()
        try:
            addr = socket.gethostbyname(self.host)
        except OSError:
            addr = None
        with self.lock:
            if addr:
                self.addr = addr
            return self.addr or BROADCAST


class PcSide:
    """Now playing and cover server (Windows, macOS), volume (Windows), and signed
    commands from the deck (td_pc.py, any OS once paired); None parts if unavailable.
    handlers: extra commands by prefix, e.g. {"open:": worker.open_link}.
    awake: td_awake.Awake, whose state goes in pcstate for the deck's tile.
    batteries: td_batteries.Batteries, this PC's and its accessories' ("batt")."""

    def __init__(self, pc_id, handlers=None, awake=None, batteries=None, deck=None):
        self.deck = deck  # () -> the deck's address (Target.get), for the cover server
        self.media = self.volume = self.commands = self.keyring = None
        self.awake = awake
        self.batteries = batteries
        try:
            import td_pc
        except ImportError as e:
            print("media/control off: %s" % e, flush=True)
            return
        self.td = td_pc
        self.keyring = td_pc.Keyring()
        if sys.platform == "win32":  # Windows media controls and audio (SMTC, pycaw)
            try:
                self.volume = td_pc.Volume()
            except Exception as e:
                print("volume off: %s" % e, flush=True)
            try:
                self.media = td_pc.Media()
                td_pc.start_art_server(self.media, self._from_deck)
            except Exception as e:
                print("media off: %s" % e, flush=True)
                self.media = None
        elif sys.platform == "darwin":  # Now playing through Apple's python3 (td_media_mac.py)
            try:
                import td_media_mac
                py = td_media_mac.apple_python()
                if py:
                    self.media = td_media_mac.MacMedia(py)
                    td_pc.start_art_server(self.media, self._from_deck)
                else:
                    print("media off: needs Apple's Command Line Tools (xcode-select --install)", flush=True)
            except Exception as e:
                print("media off: %s" % e, flush=True)
                self.media = None
        else:
            print("media/volume: Windows and macOS only for now", flush=True)
        try:
            self.commands = td_pc.Commands(pc_id, self.media, self.volume, self.keyring, handlers)
        except OSError as e:
            print("commands off: %s" % e, flush=True)

    def _from_deck(self, ip):
        # The cover goes to the deck only; while its name doesn't resolve, to anyone.
        addr = self.deck() if self.deck else BROADCAST
        return addr == BROADCAST or ip == addr

    def fill(self, pkt):
        state = {}
        if self.volume:
            vol, mute = self.volume.read()
            if vol is not None:
                state.update(vol=vol, mute=mute)
        if self.awake:
            state["awake"] = self.awake.on
            # Seconds since the last keyboard or mouse input: with two PCs the
            # deck follows the one in use (its Auto active PC).
            idle = self.awake.idle()
            if idle is not None:
                state["idle"] = idle
        batt = self.batteries.get() if self.batteries else None
        if batt:
            state["batt"] = batt
        media = self.media.snapshot() if self.media else None
        if media:
            state["media"] = media
            pkt["art_port"] = self.td.ART_PORT
        if state:
            pkt["pcstate"] = state
        # Commands only once paired: the deck signs them with the shared key.
        if self.commands and self.keyring.get():
            pkt["cmd_port"] = self.td.CMD_PORT
            pkt["nonces"] = self.commands.issue()


def start_work(target, pc_id, pc_name, verbose):
    """GitLab and Jira for the Work app (td_work.py, set up in ~/.touchdeck/work.json
    or on the settings page). Runs with no sources too: the page can add them."""
    try:
        import td_work
        worker = td_work.Worker(td_work.load_config() or {}, target.get, pc_id, pc_name, verbose)
        worker.start()
        print("work: %s" % (", ".join(worker.sources) or "no sources yet"), flush=True)
        return worker
    except Exception as e:  # a bad work.json must not stop the metrics
        print("work off: %s" % e, flush=True)
        return None


def start_bambu(target, pc_id, pc_name, verbose):
    """Bambu printers through Bambu Cloud (td_bambu.py, signed in on the settings
    page). Runs signed out too: the page signs in and picks the printers."""
    try:
        import td_bambu
        cloud = td_bambu.Cloud(target.get, pc_id, pc_name, verbose)
        cloud.start()
        names = [p.name for p in cloud.printers.values()]
        print("bambu: %s" % (", ".join(names) or "no printers picked"), flush=True)
        return cloud
    except Exception as e:  # no paho-mqtt, a bad bambu.json: the rest goes on
        print("bambu off: %s" % e, flush=True)
        return None


def start_awake():
    """Keep awake (td_awake.py): the deck's "awake_toggle" and the settings page."""
    try:
        import td_awake
        awake = td_awake.Awake()
        if not awake.os:
            print("awake: Windows and macOS only for now", flush=True)
            return None
        if awake.on:
            print("awake: on", flush=True)
        return awake
    except Exception as e:
        print("awake off: %s" % e, flush=True)
        return None


def start_batteries(pc_name):
    """This PC's battery and its accessories' for the deck (td_batteries.py)."""
    try:
        import td_batteries
        return td_batteries.Batteries(pc_name)
    except Exception as e:
        print("batteries off: %s" % e, flush=True)
        return None


def start_portal(port, worker, host, target, pc_name, awake=None, bambu=None):
    """The settings page on 127.0.0.1 (td_portal.py)."""
    if not port:
        return
    try:
        import td_portal
        portal = td_portal.Portal(worker, host, target.get, pc_name, port, awake=awake, bambu=bambu)
        if portal.serve():
            print("settings: %s" % portal.url, flush=True)
            return portal
        print("settings off: port %d is taken (another agent running?)" % port, flush=True)
    except Exception as e:
        print("settings off: %s" % e, flush=True)
    return None


def opener(url):
    """A deck command that opens one fixed page in this PC's browser (the deck's
    Settings app): url() gives it, None = not available. Never a URL from the deck."""
    def run(rest):
        if rest:
            return False, "unknown command"
        u = url()
        if not u:
            return False, "not available"
        try:
            import td_work
            td_work.open_url(u)
        except Exception as e:
            return False, clip(str(e), 60)
        return True, ""
    return run


def build_packets(metrics, pc_id, pc_name, pc=None, asks=None):
    """This second's packets: one, or two when everything won't fit in one,
    after the prompt a Claude Code session waits on (td_ask.py), if any."""
    now = time.time()
    pkt = {
        "pc_id": pc_id,
        "pc_name": pc_name,
        "timestamp": time.strftime("%H:%M"),
        "status": 1,
        "metrics": metrics.read(),
    }
    # Only a machine whose Claude Code runs the hooks (or the usage mod) speaks
    # for Claude: another PC's empty state would overwrite it on the deck.
    if os.path.isdir(SESSIONS_DIR) or os.path.isdir(USAGE_DIR):
        pkt["claude"] = claude_state(now)
    if pc:
        pc.fill(pkt)
    # Over PACKET_MAX the deck gets a cut packet and drops it. Claude's state
    # then goes on its own, first (the deck takes a Claude-only packet without
    # touching the metrics or the PC state); it carries the command port and
    # nonces too, as the deck resets them on every packet.
    packets = [pkt]
    if len(packet_bytes(pkt)) > PACKET_MAX and "claude" in pkt:
        solo = {k: pkt[k] for k in ("pc_id", "pc_name", "cmd_port", "art_port", "nonces") if k in pkt}
        solo["claude"] = pkt.pop("claude")
        packets.insert(0, solo)
    for p in packets:  # what still doesn't fit is shed, least useful first
        while len(packet_bytes(p)) > PACKET_MAX and shed(p):
            pass
    # Only once paired: the deck answers with a signed command. The prompt goes
    # on its own packet (with the nonces, as the deck resets them on every
    # packet), and Claude's state names it: without it the deck closes the card.
    ask = asks.current() if asks and "nonces" in pkt else None
    for p in packets:
        if "claude" in p:
            p["claude"]["ask"] = ask["id"] if ask else ""
    if ask:
        solo = {k: pkt[k] for k in ("pc_id", "pc_name", "cmd_port", "art_port", "nonces") if k in pkt}
        solo["ask"] = ask
        while len(packet_bytes(solo)) > PACKET_MAX and shed_ask(ask):
            pass
        packets.insert(0, solo)
    return packets


def shed_ask(ask):
    """Shortens the prompt for the packet: option descriptions, then the text."""
    if ask.pop("od", None) is not None:
        return True
    x = ask.get("x") or ""
    if len(x.encode()) > 60:
        ask["x"] = clip(x, len(x.encode()) // 2)
        return True
    return False


def shed(pkt):
    """Drops the least useful part of pkt; False when nothing is left to drop."""
    claude, state = pkt.get("claude") or {}, pkt.get("pcstate") or {}
    ss = claude.get("ss") or []
    media = state.get("media") or {}
    if any(r[3] for r in ss):  # the session list's details
        for r in ss:
            r[3] = ""
    elif ss:                   # then its rows
        ss.pop()
    elif len((media.get("title") or "").encode()) > 60 or len((media.get("artist") or "").encode()) > 60:
        for k in ("title", "artist"):
            if k in media:
                media[k] = clip(media[k], 60)
    elif state.get("batt"):
        state["batt"].pop()
    elif claude.get("acct") or claude.get("org"):
        claude["acct"] = claude["org"] = ""
    elif "pcstate" in pkt:
        del pkt["pcstate"]
    else:
        return False
    return True


def packet_bytes(pkt):
    # Raw UTF-8: the default \uXXXX escapes would double a title in CJK.
    return json.dumps(pkt, separators=(",", ":"), ensure_ascii=False).encode()


def main():
    ap = argparse.ArgumentParser(description="CompaDeck PC agent")
    ap.add_argument("mode", nargs="?", default="run", choices=["run", "hook", "pair", "settings"],
                    help="run (default), hook (Claude Code), pair (get the key for PC control), "
                         "settings (open the settings page of the running agent)")
    ap.add_argument("--host", default=os.environ.get("TOUCHDECK_HOST", "companion.local"),
                    help="deck name or IP (default companion.local, env TOUCHDECK_HOST)")
    ap.add_argument("--once", action="store_true", help="build and print one packet, send nothing")
    ap.add_argument("--portal-port", type=int, default=int(os.environ.get("TOUCHDECK_PORTAL_PORT", 4213)),
                    help="settings page on 127.0.0.1 (default 4213, 0 = off)")
    ap.add_argument("--log", metavar="FILE",
                    help="append the output to FILE (windows/touchdeck.ps1: pythonw has no console)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.mode == "hook":
        try:
            run_hook()
        except Exception:  # a hook must never get in Claude Code's way
            pass
        return

    if args.mode == "settings":
        import webbrowser
        url = "http://127.0.0.1:%d/" % args.portal_port
        import td_portal
        if not td_portal.port_in_use(args.portal_port):
            raise SystemExit("the agent is not running (start it first: python touchdeck_agent.py)")
        print(url)
        webbrowser.open(url)
        return

    if args.log:  # under pythonw print() would go nowhere, tracebacks included
        os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
        sys.stdout = sys.stderr = open(args.log, "a", encoding="utf-8", buffering=1)
        print("%s started" % time.strftime("%Y-%m-%d %H:%M:%S"), flush=True)

    pc_id, pc_name = pc_identity()
    if args.mode == "pair":
        import td_pc
        td_pc.pair(Target(args.host).get(), pc_id)
        return

    metrics = Metrics()
    if args.once:
        pc = PcSide(pc_id, batteries=start_batteries(pc_name))
        time.sleep(2.5)  # the media thread's first poll
        for pkt in build_packets(metrics, pc_id, pc_name, pc):
            print(json.dumps(pkt, indent=2, ensure_ascii=False))
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    target = Target(args.host)
    print("CompaDeck agent: %s -> %s:%d" % (pc_name, args.host, DECK_PORT), flush=True)
    worker = start_work(target, pc_id, pc_name, args.verbose)
    awake = start_awake()
    bambu = start_bambu(target, pc_id, pc_name, args.verbose)
    portal = start_portal(args.portal_port, worker, args.host, target, pc_name, awake, bambu)
    # Taps on the deck's Work rows open their MR / ticket / pipeline here; a
    # Keep awake tile toggles it.
    handlers = {}
    if worker:
        handlers["open:"] = worker.open_link
    if awake:
        handlers["awake_toggle"] = awake.command
    # The deck's Settings app: this agent's settings page, the deck's own portal.
    handlers["open_agent"] = opener(lambda: portal.url if portal else None)
    handlers["open_deck"] = opener(
        lambda: "http://%s/" % (target.get() if target.get() != BROADCAST else args.host))
    # Claude Code's permission prompts and questions, answered on the deck.
    asks = None
    try:
        import td_ask
        asks = td_ask.Asks()
        handlers["claude:"] = asks.command
    except Exception as e:
        print("claude prompts off: %s" % e, flush=True)
    pc = PcSide(pc_id, handlers or None, awake, start_batteries(pc_name), target.get)
    failing = None  # the last send error, logged once (the service's log stays small)
    while True:
        t0 = time.monotonic()
        try:
            addr = target.get()
            packets = build_packets(metrics, pc_id, pc_name, pc, asks)
            if addr == BROADCAST:  # the whole LAN hears it: only the PC's name and metrics
                packets = [{k: v for k, v in packets[-1].items()
                            if k in ("pc_id", "pc_name", "timestamp", "status", "metrics")}]
            elif asks and pc.commands and pc.keyring.get():
                asks.beat()  # hooks may wait for the deck's answer
            sizes = []
            for pkt in packets:
                data = packet_bytes(pkt)
                sock.sendto(data, (addr, DECK_PORT))
                sizes.append(str(len(data)))
            if failing:
                print("%s send OK again -> %s" % (time.strftime("%H:%M:%S"), addr), flush=True)
                failing = None
            if args.verbose:
                print("%s %s B -> %s" % (time.strftime("%H:%M:%S"), "+".join(sizes), addr), flush=True)
        except Exception as e:  # a bad session file or a sensor glitch must not end the agent
            if str(e) != failing:
                what = "send failed" if isinstance(e, OSError) else "packet failed (%s)" % type(e).__name__
                print("%s %s: %s (logged once until it changes)" % (time.strftime("%H:%M:%S"), what, e), flush=True)
                failing = str(e)
        time.sleep(max(0.0, SEND_EVERY_S - (time.monotonic() - t0)))


if __name__ == "__main__":
    main()
