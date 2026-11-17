"""Game and sensor stats from RivaTuner Statistics Server and MSI Afterburner (Windows).

Both publish what they measure in named shared memory that any process may
read: no admin, no driver, no extra package (ctypes only). Nothing to set up
beyond having them running, as anyone who watches their FPS already does.

- RTSS ("RTSSSharedMemoryV2"): the frame rate and frame time of the 3D app it
  last saw in the foreground, the one its OSD shows.
- Afterburner ("MAHMSharedMemory"): every sensor its Monitoring tab lists. The
  agent takes the CPU's temperature and power, which Windows doesn't give a
  normal process (psutil has no sensors_temperatures() there).

Layouts: RTSSSharedMemory.h and MAHMSharedMemory.h from the RTSS SDK.
"""
import ctypes
import ctypes.wintypes as wt
import struct

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenFileMappingW.restype = wt.HANDLE
_k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
_k32.MapViewOfFile.restype = ctypes.c_void_p
_k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
_k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_k32.CloseHandle.argtypes = [wt.HANDLE]
_k32.GetTickCount.restype = wt.DWORD

FILE_MAP_READ = 0x0004
RTSS_SIGNATURE = 0x52545353  # "RTSS"
MAHM_SIGNATURE = 0x4D41484D  # "MAHM"; 0xDEAD once Afterburner has quit
STALE_MS = 2500              # RTSS refreshes an app's counters about once a second
MAX_PATH = 260

# Afterburner source (its English name, whatever the UI language) -> our key.
AFTERBURNER = {"CPU temperature": "cpu_temp", "CPU power": "cpu_power"}


def _snapshot(name, head_len, size_of, parse):
    """parse(head, ptr) over a read-only view of `name`, None when it isn't there.

    The header is mapped first and the view sized from it: a header that
    claims more than the mapping holds makes MapViewOfFile fail instead of
    the read running off the end."""
    h = _k32.OpenFileMappingW(FILE_MAP_READ, False, name)
    if not h:
        return None  # the program isn't running
    try:
        p = _k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, head_len)
        if not p:
            return None
        try:
            head = ctypes.string_at(p, head_len)
        finally:
            _k32.UnmapViewOfFile(p)
        size = size_of(head)
        if not size:
            return None
        p = _k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, size)
        if not p:
            return None
        try:
            return parse(head, p)
        finally:
            _k32.UnmapViewOfFile(p)
    finally:
        _k32.CloseHandle(h)


# --- RTSS ------------------------------------------------------------------
# Header: signature, version, app entry size, app array offset, app count, ...
# v2.15+ adds the last foreground app's index and process id at 64 and 68.
# App entry: pid, name[260], flags, time0, time1, frames, frame time (us).
_APP = struct.Struct("<5I")  # flags, time0, time1, frames, frametime at +264


def _rtss_size(head):
    sig, ver, esize, off, count = struct.unpack_from("<5I", head)
    if sig != RTSS_SIGNATURE or ver < 0x00020000 or esize < 284 or count > 4096:
        return 0
    return off + esize * count


def _rtss_parse(head, p):
    _, ver, esize, off, count = struct.unpack_from("<5I", head)
    fg_pid = struct.unpack_from("<I", head, 68)[0] if ver >= 0x0002000F else 0
    best = None  # (time1, frames, time0, frametime)
    for i in range(count):
        o = p + off + i * esize
        pid = struct.unpack("<I", ctypes.string_at(o, 4))[0]
        if not pid or (fg_pid and pid != fg_pid):
            continue
        _, t0, t1, frames, ft = _APP.unpack(ctypes.string_at(o + 4 + MAX_PATH, _APP.size))
        if fg_pid:
            best = (t1, frames, t0, ft)
            break
        if t1 > t0 and (best is None or t1 > best[0]):  # old RTSS: the freshest app
            best = (t1, frames, t0, ft)
    out = {"fps": 0.0, "frametime": 0.0}
    if best:
        t1, frames, t0, ft = best
        age = (_k32.GetTickCount() - t1) & 0xFFFFFFFF
        if t1 > t0 and age < STALE_MS:  # closed or minimized: it stops drawing
            out = {"fps": 1000.0 * frames / (t1 - t0), "frametime": ft / 1000.0}
    return out


# --- MSI Afterburner -------------------------------------------------------
# Header: signature, version, header size, entry count, entry size, time.
# Entry: name, units, localized name, localized units, format (260 each), then
# data, min, max (floats), flags. A source with no reading holds FLT_MAX.
_DATA_AT = 5 * MAX_PATH


def _mahm_size(head):
    sig, ver, hsize, count, esize = struct.unpack_from("<5I", head)
    if sig != MAHM_SIGNATURE or esize < _DATA_AT + 4 or count > 1024:
        return 0
    return hsize + esize * count


def _mahm_parse(head, p):
    _, _, hsize, count, esize = struct.unpack_from("<5I", head)
    out = {}
    for i in range(count):
        o = p + hsize + i * esize
        name = ctypes.string_at(o, 32).split(b"\0", 1)[0].decode("ascii", "replace")
        key = AFTERBURNER.get(name)
        if key:
            v = struct.unpack("<f", ctypes.string_at(o + _DATA_AT, 4))[0]
            if abs(v) < 1e6:
                out[key] = v
    return out


def read():
    """{"fps", "frametime"} while RTSS runs, {"cpu_temp", "cpu_power"} while
    Afterburner does (each only if it monitors that sensor)."""
    out = {}
    for args in (("RTSSSharedMemoryV2", 72, _rtss_size, _rtss_parse),
                 ("MAHMSharedMemory", 24, _mahm_size, _mahm_parse)):
        try:
            out.update(_snapshot(*args) or {})
        except (OSError, struct.error, ValueError):
            pass
    return out
