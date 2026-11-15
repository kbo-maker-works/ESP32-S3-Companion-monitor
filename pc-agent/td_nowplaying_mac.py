#!/usr/bin/env python3
"""
TouchDeck agent helper: what macOS is playing now, for td_media_mac.py.

Since macOS 15.4 only Apple-signed programs may read MediaRemote, the private
framework behind Control Centre's Now Playing. The agent's own Python (pyenv,
Homebrew, python.org) gets nothing back; Apple's python3 from the Command Line
Tools (com.apple.python3) gets what Control Centre shows: Music, Spotify,
YouTube Music or a tab in Chrome or Safari... So td_media_mac.py runs this file
under that Python. Standard library only (ctypes), Python 3.9.

stdout, one JSON object per line:
  {"t": "state", "media": null | {"status": "playing"|"paused", "title", "artist",
   "album", "app", "dur", "pos", "key"}}            every second
  {"t": "art", "key": <the track's key>, "data": <base64 JPEG/PNG>}
                                                     when a track's cover arrives
  {"t": "ack", "id": n, "ok": bool}                  a command's result
stdin, one JSON object per line: {"id": n, "cmd": "media_play_pause" | "media_next"
  | "media_prev" | "media_stop"}. Exits when stdin closes (the agent stopped).
"""
import base64
import ctypes
import ctypes.util
import json
import os
import sys
import threading
import time
from ctypes import CFUNCTYPE, POINTER, Structure, c_bool, c_char_p, c_double, c_int, c_long, c_uint32, c_ulong, c_void_p

POLL_S = 1.0
ART_WAIT_S = 15  # a cover can arrive a while after its title; then give up until the next track
MR_PATH = "/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote"
# MRMediaRemoteCommand
COMMANDS = {"media_play_pause": 2, "media_stop": 3, "media_next": 4, "media_prev": 5}

objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
libsystem = ctypes.cdll.LoadLibrary("/usr/lib/libSystem.B.dylib")
cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
mr = ctypes.cdll.LoadLibrary(MR_PATH)

objc.objc_getClass.restype = c_void_p
objc.objc_getClass.argtypes = [c_char_p]
objc.sel_registerName.restype = c_void_p
objc.sel_registerName.argtypes = [c_char_p]
objc.objc_autoreleasePoolPush.restype = c_void_p
objc.objc_autoreleasePoolPop.argtypes = [c_void_p]
cf.CFStringCreateWithCString.restype = c_void_p
cf.CFStringCreateWithCString.argtypes = [c_void_p, c_char_p, c_uint32]
libsystem.dispatch_get_global_queue.restype = c_void_p
libsystem.dispatch_get_global_queue.argtypes = [c_long, c_ulong]
mr.MRMediaRemoteGetNowPlayingInfo.argtypes = [c_void_p, c_void_p]
mr.MRMediaRemoteSendCommand.restype = c_bool
mr.MRMediaRemoteSendCommand.argtypes = [c_int, c_void_p]


def _send(restype, *argtypes):
    return CFUNCTYPE(restype, c_void_p, c_void_p, *argtypes)(("objc_msgSend", objc))


send_id = _send(c_void_p)
send_id_id = _send(c_void_p, c_void_p)
send_bool = _send(c_bool)
send_bool_id = _send(c_bool, c_void_p)
send_double = _send(c_double)
send_str = _send(c_char_p)
send_ulong = _send(c_ulong)
_sels = {}


def sel(name):
    if name not in _sels:
        _sels[name] = objc.sel_registerName(name.encode())
    return _sels[name]


def ns_string(s):
    return cf.CFStringCreateWithCString(None, s.encode(), 0x08000100)  # kCFStringEncodingUTF8, kept for good


NSString, NSNumber, NSData, NSDate = (objc.objc_getClass(n) for n in (b"NSString", b"NSNumber", b"NSData", b"NSDate"))
Request = objc.objc_getClass(b"MRNowPlayingRequest")
KEYS = {k: ns_string("kMRMediaRemoteNowPlayingInfo" + k) for k in (
    "Title", "Artist", "Album", "Duration", "ElapsedTime", "Timestamp", "PlaybackRate",
    "ArtworkIdentifier", "ArtworkData")}


def is_a(o, cls):
    return bool(o) and send_bool_id(o, sel("isKindOfClass:"), cls)


def value(info, key, kind):
    o = send_id_id(info, sel("objectForKey:"), KEYS[key])
    if kind == "text":
        return (send_str(o, sel("UTF8String")) or b"").decode("utf-8", "replace") if is_a(o, NSString) else ""
    if kind == "num":
        return send_double(o, sel("doubleValue")) if is_a(o, NSNumber) else None
    if kind == "date":
        return send_double(o, sel("timeIntervalSince1970")) if is_a(o, NSDate) else None
    if kind == "data":
        if not is_a(o, NSData):
            return None
        n = send_ulong(o, sel("length"))
        return ctypes.string_at(send_id(o, sel("bytes")), n) if n else None
    raise ValueError(kind)


def now_playing():
    """The current player's track, None when nothing is loaded."""
    pool = objc.objc_autoreleasePoolPush()
    try:
        item = send_id(Request, sel("localNowPlayingItem"))
        info = send_id(item, sel("nowPlayingInfo")) if item else None
        if not info:
            return None
        title = value(info, "Title", "text")
        if not title:
            return None
        artist, album = value(info, "Artist", "text"), value(info, "Album", "text")
        playing = send_bool(Request, sel("localIsPlaying"))
        rate = value(info, "PlaybackRate", "num") or 0.0
        dur = value(info, "Duration", "num") or 0.0
        pos = value(info, "ElapsedTime", "num") or 0.0
        at = value(info, "Timestamp", "date")
        if playing and at:  # elapsed is as of its timestamp
            pos += (time.time() - at) * (rate or 1.0)
        pos = max(0.0, min(pos, dur) if dur else pos)
        app = ""
        path = send_id(Request, sel("localNowPlayingPlayerPath"))
        client = send_id(path, sel("client")) if path else None
        name = send_id(client, sel("displayName")) if client else None
        if is_a(name, NSString):
            app = (send_str(name, sel("UTF8String")) or b"").decode("utf-8", "replace")
        key = value(info, "ArtworkIdentifier", "text") or "|".join((title, artist, album))
        return {"status": "playing" if playing else "paused", "title": title, "artist": artist,
                "album": album, "app": app, "dur": round(dur, 1), "pos": round(pos, 1), "key": key}
    finally:
        objc.objc_autoreleasePoolPop(pool)


# MRMediaRemoteGetNowPlayingInfo(queue, ^(CFDictionaryRef info)): the only call that
# hands over the cover. A global block (no captures) built by hand for ctypes.
class _BlockDescriptor(Structure):
    _fields_ = [("reserved", c_ulong), ("size", c_ulong)]


class _Block(Structure):
    _fields_ = [("isa", c_void_p), ("flags", c_int), ("reserved", c_int), ("invoke", c_void_p),
                ("descriptor", POINTER(_BlockDescriptor))]


_art_lock = threading.Lock()
_art_done = threading.Event()
_art_result = []


@CFUNCTYPE(None, c_void_p, c_void_p)
def _on_info(_block, info):
    try:
        _art_result.append(value(info, "ArtworkData", "data") if info else None)
    except Exception:
        _art_result.append(None)
    finally:
        _art_done.set()


_descriptor = _BlockDescriptor(0, ctypes.sizeof(_Block))
_block = _Block(ctypes.addressof(c_void_p.in_dll(libsystem, "_NSConcreteGlobalBlock")), 1 << 28, 0,
                ctypes.cast(_on_info, c_void_p), ctypes.pointer(_descriptor))  # BLOCK_IS_GLOBAL


def artwork(timeout=3.0):
    """The current track's cover (JPEG or PNG bytes), None if it has none yet."""
    with _art_lock:
        _art_done.clear()
        del _art_result[:]
        mr.MRMediaRemoteGetNowPlayingInfo(libsystem.dispatch_get_global_queue(0, 0), ctypes.byref(_block))
        if not _art_done.wait(timeout):
            return None
        return _art_result[0] if _art_result else None


_out_lock = threading.Lock()


def emit(msg):
    try:
        with _out_lock:
            sys.stdout.write(json.dumps(msg, separators=(",", ":")) + "\n")
            sys.stdout.flush()
    except (BrokenPipeError, OSError):
        os._exit(0)


def read_commands():
    for line in sys.stdin:
        try:
            msg = json.loads(line)
            code = COMMANDS.get(msg.get("cmd"))
            ok = code is not None and bool(mr.MRMediaRemoteSendCommand(code, None))
        except (ValueError, AttributeError):
            continue
        emit({"t": "ack", "id": msg.get("id"), "ok": ok})
    os._exit(0)  # the agent closed our stdin: it is gone


def main():
    threading.Thread(target=read_commands, daemon=True, name="commands").start()
    art_key, art_since, art_sent = None, 0.0, False
    while True:
        m = now_playing()
        emit({"t": "state", "media": m})
        if m:
            if m["key"] != art_key:
                art_key, art_since, art_sent = m["key"], time.monotonic(), False
            if not art_sent and time.monotonic() - art_since < ART_WAIT_S:
                data = artwork()
                if data:
                    art_sent = True
                    emit({"t": "art", "key": art_key, "data": base64.b64encode(data).decode("ascii")})
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
