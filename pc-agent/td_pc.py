"""
CompaDeck agent: the PC side of the deck's Now playing and PC control tiles.

  * Media    what Windows' media controls (SMTC) report: YouTube Music's PWA in
             Chrome, Spotify, a browser tab... the session that plays, else the
             current one. Title, artist, timeline, cover. On macOS the agent uses
             td_media_mac.MacMedia instead, with the same interface.
  * ArtServer  GET /art?id=<crc32>&s=<px> -> the cover, px x px raw RGB565
             (little-endian), as the deck's Now playing tile fetches it.
  * Volume   master volume and mute (pycaw).
  * Commands  signed commands from the deck on UDP: {"pc","cmd","nonce","sig"},
             sig = hex HMAC-SHA256(key, "pc_id|nonce|cmd") over a single-use
             nonce from our latest packet. Answered with {"ack","ok","msg"} to
             the deck's port 4210. Media and volume commands run here (Windows; media on macOS);
             the agent adds others by prefix ("open:<link>", any OS).
  * pair()   gets the shared key from the deck (tap Allow on its screen).
"""
import asyncio
import datetime
import hashlib
import hmac
import io
import json
import os
import secrets
import socket
import threading
import time
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ART_PORT = 4211
CMD_PORT = 4212
DECK_PORT = 4210
NONCE_TTL_S = 60
VOLUME_STEP = 5
CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "agent.json")

PLAYING, PAUSED = 4, 5  # GlobalSystemMediaTransportControlsSessionPlaybackStatus


def _clip(text, n):
    return (text or "").encode("utf-8")[:n].decode("utf-8", "ignore")


# ---------------------------------------------------------------------------
# Media (Windows SMTC)
# ---------------------------------------------------------------------------
class Media:
    def __init__(self):
        self.lock = threading.Lock()
        self.state = None      # pcstate "media" object, None = no session
        self.art_id = ""
        self.art_png = None
        self.art_cache = {}    # px -> RGB565 bytes
        self._track = None
        self._track_at = 0.0
        self._mgr = None
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._run, daemon=True, name="media").start()

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._main())

    async def _main(self):
        from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as Manager
        self._mgr = await Manager.request_async()
        while True:
            try:
                await self._poll()
            except Exception as e:  # a session can vanish mid-call
                print("media: %s" % e, flush=True)
            await asyncio.sleep(1.0)

    def _session(self):
        cur = self._mgr.get_current_session()
        if cur and int(cur.get_playback_info().playback_status) == PLAYING:
            return cur
        for s in self._mgr.get_sessions():
            if int(s.get_playback_info().playback_status) == PLAYING:
                return s
        return cur

    async def _poll(self):
        s = self._session()
        if not s:
            with self.lock:
                self.state = None
            return
        status = int(s.get_playback_info().playback_status)
        props = await s.try_get_media_properties_async()
        tl = s.get_timeline_properties()
        dur = max(0, int((tl.end_time - tl.start_time).total_seconds()))
        pos = tl.position.total_seconds()
        if status == PLAYING and tl.last_updated_time:
            pos += (datetime.datetime.now(datetime.timezone.utc) - tl.last_updated_time).total_seconds()
        pos = max(0.0, min(pos, dur or pos))

        # The cover can arrive a moment after the title: re-read it for a while.
        track = (props.title, props.artist, props.album_title)
        if track != self._track:
            self._track, self._track_at = track, time.time()
        if time.time() - self._track_at < 10 or not self.art_png:
            await self._read_cover(props)

        media = {
            "status": "playing" if status == PLAYING else "paused" if status == PAUSED else "stopped",
            "title": _clip(props.title, 190),
            "artist": _clip(props.artist, 190),
            "dur": dur,
            "pos": int(pos),
            "t0": int(time.time() - pos) if status == PLAYING else 0,
        }
        with self.lock:
            if self.art_id:
                media["art"] = self.art_id
            self.state = media

    async def _read_cover(self, props):
        png = None
        if props.thumbnail:
            from winrt.windows.storage.streams import DataReader
            stream = await props.thumbnail.open_read_async()
            reader = DataReader(stream)
            await reader.load_async(stream.size)
            buf = bytearray(stream.size)
            reader.read_bytes(buf)
            png = bytes(buf)
        art_id = "%08x" % zlib.crc32(png) if png else ""
        with self.lock:
            if art_id != self.art_id:
                self.art_id, self.art_png, self.art_cache = art_id, png, {}

    def snapshot(self):
        with self.lock:
            return dict(self.state) if self.state else None

    def cover(self, art_id, px):
        """The cover as px x px RGB565 (little-endian), centre-cropped; None if stale."""
        with self.lock:
            if not art_id or art_id != self.art_id or not self.art_png:
                return None
            if px in self.art_cache:
                return self.art_cache[px]
            png = self.art_png
        from PIL import Image
        img = Image.open(io.BytesIO(png)).convert("RGB")
        w, h = img.size
        side = min(w, h)
        img = img.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
        raw = img.resize((px, px), Image.LANCZOS).tobytes()
        out = bytearray(px * px * 2)
        for i in range(px * px):
            r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[2 * i] = v & 0xFF
            out[2 * i + 1] = v >> 8
        out = bytes(out)
        with self.lock:
            if art_id == self.art_id:
                self.art_cache[px] = out
        return out

    def command(self, cmd):
        """media_play_pause / media_next / media_prev / media_stop on the session."""
        async def run():
            s = self._session()
            if not s:
                return False
            call = {"media_play_pause": s.try_toggle_play_pause_async, "media_next": s.try_skip_next_async,
                    "media_prev": s.try_skip_previous_async, "media_stop": s.try_stop_async}[cmd]
            return bool(await call())
        if not self._mgr:
            return False
        return asyncio.run_coroutine_threadsafe(run(), self.loop).result(timeout=5)


# ---------------------------------------------------------------------------
# Volume (pycaw), one COM endpoint per thread
# ---------------------------------------------------------------------------
class Volume:
    def __init__(self):
        self.local = threading.local()

    def _ep(self):
        ep = getattr(self.local, "ep", None)
        if ep is not None and time.monotonic() - self.local.at < 30:
            return ep
        import comtypes
        comtypes.CoInitialize()
        from pycaw.pycaw import AudioUtilities
        dev = AudioUtilities.GetSpeakers()
        ep = getattr(dev, "EndpointVolume", None)
        if ep is None:  # older pycaw: a raw IMMDevice
            from ctypes import POINTER, cast
            from comtypes import CLSCTX_ALL
            from pycaw.pycaw import IAudioEndpointVolume
            ep = cast(dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None), POINTER(IAudioEndpointVolume))
        self.local.ep, self.local.at = ep, time.monotonic()
        return ep

    def read(self):
        try:
            ep = self._ep()
            return int(round(ep.GetMasterVolumeLevelScalar() * 100)), bool(ep.GetMute())
        except Exception:
            self.local.ep = None
            return None, None

    def command(self, cmd):
        ep = self._ep()
        vol = ep.GetMasterVolumeLevelScalar() * 100
        if cmd == "volume_mute":
            ep.SetMute(0 if ep.GetMute() else 1, None)
            return True
        if cmd == "volume_up":
            vol += VOLUME_STEP
        elif cmd == "volume_down":
            vol -= VOLUME_STEP
        else:  # volume_set:N
            vol = float(cmd.split(":", 1)[1])
        ep.SetMasterVolumeLevelScalar(max(0.0, min(100.0, vol)) / 100.0, None)
        if vol > 0 and ep.GetMute():
            ep.SetMute(0, None)
        return True


# ---------------------------------------------------------------------------
# Cover server
# ---------------------------------------------------------------------------
def start_art_server(media, allowed=None):
    """allowed(ip) -> bool: who may fetch the cover (the deck), None = anyone."""
    class Handler(BaseHTTPRequestHandler):
        timeout = 5  # a slow client must not hold a thread for ever

        def do_GET(self):
            if allowed and not allowed(self.client_address[0]):
                self.send_error(403)
                return
            u = urlparse(self.path)
            q = parse_qs(u.query)
            data = None
            try:
                if u.path == "/art":
                    px = int(q.get("s", ["0"])[0])
                    if 32 <= px <= 200:
                        data = media.cover(q.get("id", [""])[0], px)
            except Exception as e:
                print("art: %s" % e, flush=True)
            if not data:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("0.0.0.0", ART_PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True, name="art").start()
    return server


# ---------------------------------------------------------------------------
# Pairing key and commands
# ---------------------------------------------------------------------------
class Keyring:
    """The key the deck gave in pairing, in ~/.touchdeck/agent.json (re-read when it changes)."""

    def __init__(self):
        self.key, self.mtime = None, None

    def get(self):
        try:
            m = os.path.getmtime(CONFIG_FILE)
        except OSError:
            self.key, self.mtime = None, None
            return None
        if m != self.mtime:
            self.mtime = m
            try:
                with open(CONFIG_FILE, encoding="utf-8") as f:
                    self.key = bytes.fromhex(json.load(f)["key"])
            except (OSError, ValueError, KeyError):
                self.key = None
        return self.key


MEDIA_CMDS = {"media_play_pause", "media_next", "media_prev", "media_stop"}
VOLUME_CMDS = {"volume_up", "volume_down", "volume_mute"}


class Commands:
    """handlers: {"prefix:": fn(rest) -> (ok, msg)} for commands beyond media and volume."""

    def __init__(self, pc_id, media, volume, keyring, handlers=None):
        self.pc_id, self.media, self.volume, self.keyring = pc_id, media, volume, keyring
        self.handlers = dict(handlers or {})
        self.lock = threading.Lock()
        self.nonces = {}  # nonce -> issued at
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", CMD_PORT))
        threading.Thread(target=self._serve, daemon=True, name="commands").start()

    def issue(self):
        """Four fresh single-use nonces for the next packet."""
        now = time.time()
        fresh = [secrets.token_hex(8) for _ in range(4)]
        with self.lock:
            self.nonces = {n: t for n, t in self.nonces.items() if now - t < NONCE_TTL_S}
            for n in fresh:
                self.nonces[n] = now
        return fresh

    def _serve(self):
        refused_at = 0.0
        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
                msg = json.loads(data.decode("utf-8"))
                if not isinstance(msg, dict):  # "1", "[]", "null" from anyone on the LAN
                    continue
                nonce, cmd = str(msg.get("nonce", "")), str(msg.get("cmd", ""))
                ok, text = self._check(msg, nonce, cmd)
                if ok:
                    ok, text = self._run(cmd)
                # Refusals at most every 30 s: junk on the port must not grow the log.
                if ok or time.monotonic() - refused_at >= 30:
                    if not ok:
                        refused_at = time.monotonic()
                    print("command %s from %s: %s %s" % (cmd[:40], addr[0], "ok" if ok else "refused", text), flush=True)
                ack = json.dumps({"ack": nonce, "ok": ok, "msg": text}).encode()
                self.sock.sendto(ack, (addr[0], DECK_PORT))
            except (OSError, ValueError):
                continue
            except Exception as e:  # never let one packet end deck control
                print("commands: %s" % e, flush=True)

    def _check(self, msg, nonce, cmd):
        key = self.keyring.get()
        if not key:
            return False, "not paired"
        if msg.get("pc") != self.pc_id:
            return False, "wrong PC"
        with self.lock:
            known = nonce in self.nonces
        if not known:
            return False, "stale nonce"
        want = hmac.new(key, ("%s|%s|%s" % (self.pc_id, nonce, cmd)).encode(), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, str(msg.get("sig", "")).lower()):
            return False, "bad signature"  # the nonce stays: junk can't burn the deck's nonces
        with self.lock:
            if self.nonces.pop(nonce, None) is None:  # used meanwhile
                return False, "stale nonce"
        return True, ""

    def _run(self, cmd):
        try:
            for prefix, fn in self.handlers.items():
                if cmd.startswith(prefix):
                    return fn(cmd[len(prefix):])
            if cmd in MEDIA_CMDS and self.media:
                return (True, "") if self.media.command(cmd) else (False, "no media playing")
            if (cmd in VOLUME_CMDS or cmd.startswith("volume_set:")) and self.volume:
                return self.volume.command(cmd), ""
        except Exception as e:
            return False, _clip(str(e), 60)
        return False, "not enabled in this agent"


def pair(deck, pc_id):
    """Asks the deck for a key: tap Allow on its screen. Saves it to CONFIG_FILE."""
    def post(path):
        req = urllib.request.Request("http://%s%s" % (deck, path), data=b"", method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read().decode() or "{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode() or "{}")

    code, r = post("/api/pc/pair?id=%s&mode=tap" % pc_id)
    if code != 200:
        raise SystemExit("pairing refused (%d): %s" % (code, r.get("message")))
    token = r["token"]
    print("Tap 'Allow' on the deck screen...", flush=True)
    for _ in range(90):
        time.sleep(1)
        code, r = post("/api/pc/pair/confirm?id=%s&token=%s" % (pc_id, token))
        if code == 200 and r.get("key"):
            os.makedirs(os.path.dirname(CONFIG_FILE), mode=0o700, exist_ok=True)
            # The shared key signs commands: readable by this user only.
            fd = os.open(CONFIG_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump({"key": r["key"], "deck": deck, "pc_id": pc_id}, f)
            try:
                os.chmod(CONFIG_FILE, 0o600)
            except OSError:
                pass
            print("Paired. Key saved to %s" % CONFIG_FILE)
            return
        if code != 202:
            raise SystemExit("pairing failed (%d): %s" % (code, r.get("message")))
    post("/api/pc/pair/cancel")
    raise SystemExit("nobody tapped 'Allow' in time")
