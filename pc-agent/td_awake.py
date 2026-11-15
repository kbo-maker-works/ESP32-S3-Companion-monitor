"""
TouchDeck agent: Keep awake, a USB mouse jiggler in software.

While on:
  * Awake: the computer does not sleep and the screen neither sleeps nor locks
    from idleness (macOS: `caffeinate -d -i`, tied to the agent's life; Windows:
    SetThreadExecutionState). Needs no permission.
  * Stay active (on by default): after JIGGLE_IDLE_S without mouse or keyboard,
    the pointer moves 1 px and back, so chat apps (Slack, Teams) keep showing
    you active and screen savers keep away. Never while you use the computer.
    macOS lets a program post mouse events only with the Accessibility
    permission (System Settings > Privacy & Security > Accessibility, the
    agent's Python); without it only Awake works and status() says so.

Toggled from the deck (PC command "awake_toggle", signed like the others) and
the settings page; kept in ~/.touchdeck/awake.json across restarts. The agent
sends "awake": true/false in pcstate for the deck's tile.
"""
import ctypes
import ctypes.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "awake.json")
JIGGLE_IDLE_S = 60   # idle this long: move the pointer (chat apps go away after 5-10 min)
TICK_S = 10


class _Mac:
    def __init__(self):
        self.proc = None
        cg = self.cg = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        self.cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]
        cg.CGEventSourceSecondsSinceLastEventType.restype = ctypes.c_double
        cg.CGEventSourceSecondsSinceLastEventType.argtypes = [ctypes.c_int, ctypes.c_uint32]
        cg.CGEventCreate.restype = ctypes.c_void_p
        cg.CGEventCreate.argtypes = [ctypes.c_void_p]
        cg.CGEventGetLocation.restype = _Point
        cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
        cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
        cg.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, _Point, ctypes.c_uint32]
        cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        cg.CGPreflightPostEventAccess.restype = ctypes.c_bool
        cg.CGRequestPostEventAccess.restype = ctypes.c_bool

    def hold(self, on):
        running = self.proc is not None and self.proc.poll() is None
        if on and not running:
            # -w: it ends with the agent, even a killed one.
            self.proc = subprocess.Popen(["/usr/bin/caffeinate", "-d", "-i", "-w", str(os.getpid())])
        elif not on and self.proc is not None:
            if running:
                self.proc.terminate()
            self.proc.wait()
            self.proc = None

    def idle(self):
        return self.cg.CGEventSourceSecondsSinceLastEventType(1, 0xFFFFFFFF)  # HID state, any input

    def can_jiggle(self):
        return bool(self.cg.CGPreflightPostEventAccess())

    def ask(self):
        """macOS's prompt to grant the Accessibility permission (once per program)."""
        return bool(self.cg.CGRequestPostEventAccess())

    def jiggle(self):
        cg, cf = self.cg, self.cf
        e = cg.CGEventCreate(None)
        at = cg.CGEventGetLocation(e)
        cf.CFRelease(e)
        for p in (_Point(at.x + 1, at.y), at):
            m = cg.CGEventCreateMouseEvent(None, 5, p, 0)  # kCGEventMouseMoved
            cg.CGEventPost(0, m)  # kCGHIDEventTap: counts as input, as a real mouse would
            cf.CFRelease(m)


class _Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class _Win:
    ES_CONTINUOUS, ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED = 0x80000000, 0x1, 0x2

    def __init__(self):
        from ctypes import wintypes
        self.k32, self.u32 = ctypes.windll.kernel32, ctypes.windll.user32

        class LastInput(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]

        class MouseInput(ctypes.Structure):
            _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                        ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]

        class Input(ctypes.Structure):
            _fields_ = [("type", wintypes.DWORD), ("mi", MouseInput)]

        self.LastInput, self.MouseInput, self.Input = LastInput, MouseInput, Input
        self.k32.SetThreadExecutionState.restype = ctypes.c_uint32
        self.k32.SetThreadExecutionState.argtypes = [ctypes.c_uint32]
        self.k32.GetTickCount.restype = wintypes.DWORD
        self.u32.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]

    def hold(self, on):
        # Per thread: Awake's own thread calls this every tick and never ends.
        es = self.ES_CONTINUOUS | (self.ES_SYSTEM_REQUIRED | self.ES_DISPLAY_REQUIRED if on else 0)
        self.k32.SetThreadExecutionState(es)

    def idle(self):
        li = self.LastInput(ctypes.sizeof(self.LastInput), 0)
        if not self.u32.GetLastInputInfo(ctypes.byref(li)):
            return 0.0
        return ((self.k32.GetTickCount() - li.dwTime) & 0xFFFFFFFF) / 1000.0

    def can_jiggle(self):
        return True

    def ask(self):
        return True

    def jiggle(self):
        moves = (self.Input * 2)(self.Input(0, self.MouseInput(1, 0, 0, 1, 0, 0)),   # INPUT_MOUSE, MOUSEEVENTF_MOVE
                                 self.Input(0, self.MouseInput(-1, 0, 0, 1, 0, 0)))
        self.u32.SendInput(2, moves, ctypes.sizeof(self.Input))


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        return {"on": bool(cfg.get("on")), "jiggle": bool(cfg.get("jiggle", True))}
    except (OSError, ValueError, AttributeError):
        return {"on": False, "jiggle": True}


class Awake:
    def __init__(self, path=CONFIG_FILE):
        self.path = path
        self.lock = threading.Lock()
        self.cfg = _load(path)
        self.os = _Mac() if sys.platform == "darwin" else _Win() if sys.platform == "win32" else None
        self.can_jiggle = False
        self.jiggles, self.last_jiggle = 0, 0.0
        self.error = None
        self.wake = threading.Event()
        if self.os:
            threading.Thread(target=self._run, daemon=True, name="awake").start()

    def _run(self):
        while True:
            with self.lock:
                on, jiggle = self.cfg["on"], self.cfg["jiggle"]
            try:
                self.os.hold(on)
                self.can_jiggle = self.os.can_jiggle()
                if on and jiggle and self.can_jiggle and self.os.idle() >= JIGGLE_IDLE_S:
                    self.os.jiggle()
                    self.jiggles, self.last_jiggle = self.jiggles + 1, time.time()
                if self.error:
                    print("awake: working again", flush=True)
                    self.error = None
            except Exception as e:
                if str(e) != self.error:
                    print("awake: %s" % e, flush=True)
                    self.error = str(e)
            self.wake.wait(TICK_S)
            self.wake.clear()

    @property
    def on(self):
        with self.lock:
            return self.cfg["on"]

    def set(self, on=None, jiggle=None):
        with self.lock:
            if on is not None:
                self.cfg["on"] = bool(on)
            if jiggle is not None:
                self.cfg["jiggle"] = bool(jiggle)
            cfg = dict(self.cfg)
        os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".awake.", dir=os.path.dirname(self.path))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        os.replace(tmp, self.path)
        self.wake.set()  # apply now, not at the next tick
        print("awake: %s" % ("on" if cfg["on"] else "off"), flush=True)
        return self.status()

    def command(self, rest):
        """The deck's "awake_toggle" (a Commands handler: rest is what follows it)."""
        if rest:
            return False, "unknown command"
        if not self.os:
            return False, "not on this system"
        self.set(on=not self.on)
        return True, ""

    def ask_permission(self):
        ok = self.os.ask() if self.os else False
        self.wake.set()
        return ok

    def status(self):
        with self.lock:
            cfg = dict(self.cfg)
        idle = None
        if self.os:
            try:
                idle = round(self.os.idle())
            except Exception:
                pass
        return {"supported": self.os is not None, "on": cfg["on"], "jiggle": cfg["jiggle"],
                "can_jiggle": self.can_jiggle, "needs_permission": sys.platform == "darwin" and not self.can_jiggle,
                "program": os.path.realpath(sys.executable),  # the one to allow in Accessibility
                "jiggle_after_s": JIGGLE_IDLE_S, "idle_s": idle, "jiggles": self.jiggles,
                "last_jiggle": self.last_jiggle or None, "error": self.error}
