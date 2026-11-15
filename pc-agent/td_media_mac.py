"""
TouchDeck agent: Now playing on macOS for the deck's Now playing tile (title,
artist, timeline, cover, play/pause/next/previous), with the interface of
td_pc.Media so the cover server and the deck's commands take it as it is.

macOS 15.4 and later let only Apple-signed programs read what is playing
(MediaRemote), so td_nowplaying_mac.py does the reading under Apple's own
python3 from the Command Line Tools and streams it here, one JSON per line. The
cover is cut square and resized with sips (part of macOS) into the deck's RGB565.
No extra packages, no password, no permission prompt.
"""
import base64
import json
import os
import struct
import subprocess
import tempfile
import threading
import time
import zlib

HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "td_nowplaying_mac.py")
CMD_TIMEOUT_S = 3


def apple_python():
    """Apple's python3 (Command Line Tools or Xcode), None if not installed. Never
    runs /usr/bin/python3 to find out: without the tools that opens an install dialog."""
    try:
        dev = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    py = os.path.join(dev, "usr", "bin", "python3") if dev else ""
    return py if py and os.access(py, os.X_OK) else None


def _clip(text, n):
    return (text or "").encode("utf-8")[:n].decode("utf-8", "ignore")


def _sips(*args):
    subprocess.run(["/usr/bin/sips", *args], check=True, capture_output=True, timeout=10)


def _bmp_size(path):
    with open(path, "rb") as f:
        head = f.read(26)
    return struct.unpack_from("<i", head, 18)[0], abs(struct.unpack_from("<i", head, 22)[0])


def square_rgb565(image, px):
    """An image (JPEG, PNG...) cut to its centred square and resized to px x px,
    as raw RGB565 little-endian rows, top first."""
    with tempfile.TemporaryDirectory(prefix="td-art-") as d:
        src, full, sq, out = (os.path.join(d, n) for n in ("src", "full.bmp", "sq.bmp", "out.bmp"))
        with open(src, "wb") as f:
            f.write(image)
        _sips("-s", "format", "bmp", src, "--out", full)
        w, h = _bmp_size(full)
        if w != h:  # sips crops around the centre; crop and resize in one call misorders them
            side = min(w, h)
            _sips("-c", str(side), str(side), full, "--out", sq)
            full = sq
        _sips("-z", str(px), str(px), "-s", "format", "bmp", full, "--out", out)
        with open(out, "rb") as f:
            bmp = f.read()
    start = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp = struct.unpack_from("<H", bmp, 28)[0] // 8
    if w != px or abs(h) != px or bpp not in (3, 4):
        raise ValueError("sips gave %dx%d at %d bits" % (w, h, bpp * 8))
    stride = (w * bpp + 3) & ~3
    rows = range(px) if h < 0 else range(px - 1, -1, -1)  # negative height: top-down
    out = bytearray(px * px * 2)
    i = 0
    for y in rows:
        row = start + y * stride
        for x in range(px):
            b, g, r = bmp[row + x * bpp], bmp[row + x * bpp + 1], bmp[row + x * bpp + 2]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i], out[i + 1] = v & 0xFF, v >> 8
            i += 2
    return bytes(out)


class MacMedia:
    def __init__(self, python):
        self.python = python
        self.lock = threading.Lock()
        self.state = None       # the helper's last "media", None = nothing loaded
        self.art_key = None     # the track the cover belongs to
        self.art_id = ""
        self.art_data = None
        self.art_cache = {}     # px -> RGB565 bytes
        self.proc = None
        self.write_lock = threading.Lock()
        self.acks = {}          # command id -> [Event, ok]
        self.next_id = 0
        threading.Thread(target=self._run, daemon=True, name="media").start()

    def _run(self):
        fails = 0
        while True:
            started = time.monotonic()
            try:
                # stderr goes to the agent's log: a traceback there says why it stopped.
                self.proc = subprocess.Popen([self.python, "-I", HELPER], stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE, text=True, bufsize=1)
                for line in self.proc.stdout:
                    self._take(line)
            except Exception as e:
                print("media: %s" % e, flush=True)
            finally:
                if self.proc:
                    self.proc.kill()
                    self.proc.wait()
                with self.lock:
                    self.state = None
            fails = fails + 1 if time.monotonic() - started < 60 else 1
            if fails == 1 or fails % 10 == 0:
                print("media: the Now playing helper stopped, restarting", flush=True)
            time.sleep(min(60, 2 ** min(fails, 6)))

    def _take(self, line):
        try:
            msg = json.loads(line)
        except ValueError:
            return
        t = msg.get("t")
        if t == "state":
            with self.lock:
                self.state = msg.get("media") or None
        elif t == "art":
            try:
                data = base64.b64decode(msg.get("data") or "")
            except ValueError:
                return
            art_id = "%08x" % zlib.crc32(data) if data else ""
            with self.lock:
                self.art_key = msg.get("key")
                if art_id != self.art_id:
                    self.art_id, self.art_data, self.art_cache = art_id, data, {}
        elif t == "ack":
            with self.lock:
                waiter = self.acks.get(msg.get("id"))
            if waiter:
                waiter[1] = bool(msg.get("ok"))
                waiter[0].set()

    def snapshot(self):
        """The pcstate "media" object, as td_pc.Media's."""
        with self.lock:
            m = self.state
            if not m:
                return None
            playing = m.get("status") == "playing"
            dur = max(0, int(m.get("dur") or 0))
            pos = max(0.0, float(m.get("pos") or 0))
            media = {
                "status": "playing" if playing else "paused",
                "title": _clip(m.get("title"), 190),
                "artist": _clip(m.get("artist"), 190),
                "dur": dur,
                "pos": int(pos),
                "t0": int(time.time() - pos) if playing else 0,
            }
            if self.art_id and self.art_key == m.get("key"):
                media["art"] = self.art_id
            return media

    def cover(self, art_id, px):
        """The cover as px x px RGB565 (little-endian), centre-cropped; None if stale."""
        with self.lock:
            if not art_id or art_id != self.art_id or not self.art_data:
                return None
            if px in self.art_cache:
                return self.art_cache[px]
            data = self.art_data
        out = square_rgb565(data, px)
        with self.lock:
            if art_id == self.art_id:
                self.art_cache[px] = out
        return out

    def command(self, cmd):
        """media_play_pause / media_next / media_prev / media_stop on the current player."""
        proc = self.proc
        if not proc or proc.poll() is not None:
            return False
        with self.lock:
            self.next_id += 1
            cid = self.next_id
            waiter = self.acks[cid] = [threading.Event(), False]
        try:
            with self.write_lock:
                proc.stdin.write(json.dumps({"id": cid, "cmd": cmd}) + "\n")
                proc.stdin.flush()
            waiter[0].wait(CMD_TIMEOUT_S)
            return waiter[1]
        except OSError:
            return False
        finally:
            with self.lock:
                self.acks.pop(cid, None)
