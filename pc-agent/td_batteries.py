"""
TouchDeck agent: batteries for the deck's "PC stats" widget, Batteries
variants: this computer's own battery and, on macOS, its wireless
accessories' (keyboards, mice, trackpads, AirPods and their case, headsets,
game controllers...), what a Mac's Batteries widget shows.

macOS publishes accessory batteries as power sources (IOKit's
IOPSCopyPowerSourcesByType, Type "Accessory Source"; `pmset -g accps` lists the same), readable with no
permission. Only devices that report their battery to macOS show up: AirPods,
Apple's keyboards and mice, Bluetooth LE devices with the standard battery
service... A device that macOS shows without a battery has none here either.
Elsewhere: the computer's own battery only (psutil).

In pcstate as "batt": [[name, kind, percent, charging], ...], up to MAX, this
computer first. kind: l computer, k keyboard, m mouse, t trackpad, h
headphones or headset, c a case, g game controller, s speaker, p phone,
w watch, "" other.
"""
import ctypes
import ctypes.util
import plistlib
import sys
import threading
import time

MAX = 6
READ_EVERY_S = 30
NAME_BYTES = 23


def _clip(text, n):
    return (text or "").encode("utf-8")[:n].decode("utf-8", "ignore").strip()


def _kind(category):
    c = (category or "").lower()
    for words, k in ((("case",), "c"), (("keyboard",), "k"), (("trackpad",), "t"), (("mouse",), "m"),
                     (("head", "ear", "audio"), "h"), (("game", "controller", "joystick"), "g"),
                     (("speaker",), "s"), (("phone",), "p"), (("watch",), "w")):
        if any(w in c for w in words):
            return k
    return ""


class _MacSources:
    """IOKit power sources as plain dicts (each one turned into a plist and parsed)."""

    def __init__(self):
        iokit = self.iokit = ctypes.cdll.LoadLibrary(ctypes.util.find_library("IOKit"))
        cf = self.cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
        vp, lg, i = ctypes.c_void_p, ctypes.c_long, ctypes.c_int
        iokit.IOPSCopyPowerSourcesByType.restype = vp
        iokit.IOPSCopyPowerSourcesByType.argtypes = [i]
        iokit.IOPSCopyPowerSourcesList.restype = vp
        iokit.IOPSCopyPowerSourcesList.argtypes = [vp]
        iokit.IOPSGetPowerSourceDescription.restype = vp
        iokit.IOPSGetPowerSourceDescription.argtypes = [vp, vp]
        cf.CFArrayGetCount.restype = lg
        cf.CFArrayGetCount.argtypes = [vp]
        cf.CFArrayGetValueAtIndex.restype = vp
        cf.CFArrayGetValueAtIndex.argtypes = [vp, lg]
        cf.CFPropertyListCreateData.restype = vp
        cf.CFPropertyListCreateData.argtypes = [vp, vp, i, lg, vp]
        cf.CFDataGetLength.restype = lg
        cf.CFDataGetLength.argtypes = [vp]
        cf.CFDataGetBytePtr.restype = vp
        cf.CFDataGetBytePtr.argtypes = [vp]
        cf.CFRelease.argtypes = [vp]

    def accessories(self):
        iokit, cf = self.iokit, self.cf
        out = []
        blob = iokit.IOPSCopyPowerSourcesByType(0)  # all; the accessories are picked by "Type" below
        if not blob:
            return out
        lst = iokit.IOPSCopyPowerSourcesList(blob)
        try:
            for n in range(cf.CFArrayGetCount(lst) if lst else 0):
                desc = iokit.IOPSGetPowerSourceDescription(blob, cf.CFArrayGetValueAtIndex(lst, n))
                if not desc:
                    continue
                data = cf.CFPropertyListCreateData(None, desc, 100, 0, None)  # kCFPropertyListXMLFormat_v1_0
                if not data:
                    continue
                try:
                    out.append(plistlib.loads(ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data))))
                except Exception:
                    pass
                finally:
                    cf.CFRelease(data)
        finally:
            if lst:
                cf.CFRelease(lst)
            cf.CFRelease(blob)
        return out


def _accessory(info):
    if info.get("Type") != "Accessory Source" or info.get("Is Present") is False:
        return None
    cur, top = info.get("Current Capacity"), info.get("Max Capacity") or 100
    if not isinstance(cur, (int, float)) or not top:
        return None
    pct = max(0, min(100, int(round(100.0 * cur / top))))
    kind, name = _kind(info.get("Accessory Category")), info.get("Name") or ""
    if kind == "c" and name.endswith(" Case"):  # "AirPods ... Case": keep the word the clip would cut
        name = _clip(name[:-5], NAME_BYTES - 5) + " Case"
    return [_clip(name, NAME_BYTES), kind, pct, bool(info.get("Is Charging"))]


class Batteries:
    def __init__(self, pc_name):
        self.pc_name = pc_name
        self.lock = threading.Lock()
        self.items, self.at = [], -READ_EVERY_S
        self.mac = None
        self.error = None
        if sys.platform == "darwin":
            try:
                self.mac = _MacSources()
            except OSError as e:
                print("batteries: accessories off: %s" % e, flush=True)

    def _read(self):
        items = []
        try:
            import psutil
            b = psutil.sensors_battery()
            if b:
                items.append([_clip(self.pc_name, NAME_BYTES), "l", int(round(b.percent)),
                              bool(b.power_plugged) and b.percent < 100])
        except Exception:
            pass
        if self.mac:
            for info in self.mac.accessories():
                a = _accessory(info)
                if a and a[0]:
                    items.append(a)
        return items[:MAX]

    def get(self):
        """The list for pcstate, re-read every READ_EVERY_S."""
        with self.lock:
            if time.monotonic() - self.at >= READ_EVERY_S:
                self.at = time.monotonic()
                try:
                    self.items = self._read()
                    self.error = None
                except Exception as e:  # never break the packet
                    if str(e) != self.error:
                        print("batteries: %s" % e, flush=True)
                        self.error = str(e)
            return [list(x) for x in self.items]


if __name__ == "__main__":
    import json
    print(json.dumps(Batteries("This computer").get(), ensure_ascii=False, indent=1))
