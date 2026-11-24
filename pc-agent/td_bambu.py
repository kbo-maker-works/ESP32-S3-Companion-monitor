#!/usr/bin/env python3
"""
Bambu Lab printers through Bambu Cloud, for the deck's printer pages and tiles.

The printer stays in its normal cloud mode, so Bambu Handy's notifications, the
camera and remote printing keep working. This agent signs in to Bambu Cloud
with your account (on the settings page), listens to the printers you pick on
Bambu's MQTT broker and pushes what the deck shows, already interpreted, when
it changes and once a minute:

    POST http://<deck>/api/pc/printers   {"pc_id", "pc_name", "printers": [...]}

Read only: the only thing ever sent to a printer is a status request. Bambu
asks third-party software to name itself and to keep its traffic low, so this
agent sends its own User-Agent and asks for a printer's full status
("pushall") on connecting and then only while the printer is idle, every
5 min: a printing printer pushes its changes by itself; one that is off gets
one every 30 min. Requests on the cloud
broker's request topic every few seconds get the session refused.

~/.touchdeck/bambu.json (mode 600, written by the settings page):

    {"region": "global" | "cn", "account": "you@example.com", "token": "...",
     "refresh": "...", "printers": [{"serial": "22E...", "name": "P2S", "model": "P2S"}]}

The password is never stored. The token lasts about three months; once Bambu
refuses it the agent stops trying, and the settings page and the deck ask you
to sign in again.

The report parsing is the deck's own (src/printers/deck_bambu.cpp, report()),
ported field for field, so a printer reads the same over the LAN and here.

Show sample printers on the deck:   python td_bambu.py --sample --host 192.168.1.225
"""
import argparse
import hashlib
import json
import os
import platform
import secrets
import threading
import time
import urllib.error
import urllib.request

import td_work  # save_config (mode 600, atomic), clip

CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "bambu.json")
USER_AGENT = "TouchDeck-Agent/1.0"
API = {"global": "https://api.bambulab.com", "cn": "https://api.bambulab.cn"}
BROKER = {"global": "us.mqtt.bambulab.com", "cn": "cn.mqtt.bambulab.com"}  # EU accounts use the US broker
WEB = "https://bambulab.com"  # the authenticator-app step goes through the web sign-in, both regions
TIMEOUT_S = 20

PUSHALL_IDLE_S = 300   # full status while idle; a printing printer pushes by itself
PUSHALL_GAP_S = 120    # never two status requests closer than this (but the one on connecting)
STALE_JOB_S = 120      # printing and silent this long: one status request to catch up
SILENT_S = 330         # no report this long: the printer is off or not reporting
PUSHALL_OFF_S = 1800   # off: one status request this often, in case it is back (it reports by itself on waking)
PUSH_GAP_S = 3         # to the deck: changes at most this often...
PUSH_EVERY_S = 60      # ...and this often anyway, so it knows the data is current
RETRY_S = (30, 60, 120, 300, 600)  # broker reconnects

# Deck sizes (src/printers/deck_bambu.h), terminator included.
PRN_MAX, AMS_UNITS, AMS_SLOTS, MAP_MAX, HMS_MAX = 2, 6, 4, 16, 32
MAX_JOB, MAX_TRAY, MAX_ERR, MAX_NAME = 64, 24, 48, 32


class CloudError(Exception):
    """Something Bambu Cloud said or did, worded for the settings page."""


class SignInNeeded(CloudError):
    pass


def load_config(path=CONFIG_FILE):
    try:
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        print("bambu: can't read %s: %s" % (path, e), flush=True)
        return {}


def save_config(cfg, path=CONFIG_FILE):
    td_work.save_config(cfg, path)


def region_of(cfg):
    return "cn" if (cfg or {}).get("region") == "cn" else "global"


# ---------------------------------------------------------------------------
# Bambu Cloud's HTTP API: sign-in, profile, printer list
# ---------------------------------------------------------------------------
def _cookies(headers):
    """Set-Cookie values by name. A value may hold "=": split on the first one only."""
    out = {}
    for raw in headers.get_all("Set-Cookie") or []:
        name, _, value = raw.split(";", 1)[0].partition("=")
        out[name.strip()] = value.strip()
    return out


def _http(method, url, body=None, token=None, headers=None):
    """(status, JSON body or None, cookies). A 403 page in HTML is Cloudflare, not a bad token."""
    h = {"User-Agent": USER_AGENT, "Accept": "application/json", "Content-Type": "application/json"}
    h.update(headers or {})
    if token:
        h["Authorization"] = "Bearer " + token
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        resp = urllib.request.urlopen(req, timeout=TIMEOUT_S)
    except urllib.error.HTTPError as e:
        resp = e
    except (urllib.error.URLError, OSError) as e:
        raise CloudError("Can't reach Bambu Cloud: %s" % getattr(e, "reason", e))
    with resp:
        status = resp.status if hasattr(resp, "status") else resp.code
        raw, ctype, cookies = resp.read(), resp.headers.get("Content-Type") or "", _cookies(resp.headers)
    if status == 403 and "text/html" in ctype:
        raise CloudError("Bambu Cloud blocked the request (Cloudflare, 403). Try again later.")
    try:
        payload = json.loads(raw.decode("utf-8")) if raw else {}
    except ValueError:
        payload = None
    return status, payload, cookies


def _keys(what, status, data):
    """Logs the names of a response's fields, never their values: what Bambu sends
    (a refresh token? its lifetime?) is not documented."""
    if isinstance(data, dict):
        inner = data.get("data") if isinstance(data.get("data"), dict) else {}
        print("bambu: %s -> HTTP %s, fields %s%s" % (what, status, sorted(data),
              " data.%s" % sorted(inner) if inner else ""), flush=True)


def _message(data):
    if isinstance(data, dict):
        for k in ("message", "error", "msg"):
            if isinstance(data.get(k), str) and data[k].strip():
                return data[k].strip()
    return ""


def _token_of(data):
    if not isinstance(data, dict):
        return None, None
    inner = data.get("data") if isinstance(data.get("data"), dict) else {}
    token = data.get("accessToken") or inner.get("accessToken")
    refresh = data.get("refreshToken") or inner.get("refreshToken")
    return (token if isinstance(token, str) and token else None,
            refresh if isinstance(refresh, str) and refresh else None)


def login(region, account, password):
    """{"token", "refresh"} when signed in, or {"need": "email" | "app", "tfa_key"}
    when Bambu wants a code first (sent by email, or from an authenticator app)."""
    status, data, _ = _http("POST", API[region] + "/v1/user-service/user/login",
                            {"account": account.strip(), "password": password})
    _keys("sign-in", status, data)
    if data is None:
        raise CloudError("Bambu Cloud answered something else than JSON (HTTP %d)" % status)
    token, refresh = _token_of(data)
    if token:
        return {"token": token, "refresh": refresh}
    kind = data.get("loginType")
    if kind == "verifyCode":
        return {"need": "email", "tfa_key": ""}
    if kind == "tfa":
        return {"need": "app", "tfa_key": str(data.get("tfaKey") or "")}
    raise CloudError(_message(data) or ("Wrong email or password" if status in (400, 401)
                                        else "Sign-in failed (HTTP %d)" % status))


def send_email_code(region, account):
    """Asks Bambu to email a sign-in code again (the first one comes with the sign-in)."""
    status, data, _ = _http("POST", API[region] + "/v1/user-service/user/sendemail/code",
                            {"email": account.strip(), "type": "codeLogin"})
    _keys("send code", status, data)
    if status >= 300:
        raise CloudError(_message(data) or "Could not send the code (HTTP %d)" % status)


def verify_code(region, account, code, kind, tfa_key=""):
    """The second step: the emailed code, or the authenticator app's with the tfa_key."""
    code = code.strip()
    if kind == "email":
        status, data, _ = _http("POST", API[region] + "/v1/user-service/user/login",
                                {"account": account.strip(), "code": code})
        _keys("email code", status, data)
        token, refresh = _token_of(data)
    else:
        token, refresh = _verify_app(code, tfa_key)
    if not token:
        raise CloudError("Wrong or expired code")
    return {"token": token, "refresh": refresh}


def _verify_app(code, tfa_key):
    """Bambu's web sign-in: a CSRF cookie, then the code. The token comes back as
    the "token" cookie. A stale CSRF value gets one retry with a fresh one."""
    browser = {"Accept": "application/json, text/plain, */*", "Accept-Language": "en-US,en;q=0.9",
               "Origin": WEB, "Referer": WEB + "/"}
    for _ in range(2):
        _, _, cookies = _http("GET", WEB + "/api/csrf", headers=browser)
        csrf = cookies.get("bbl_csrf_token")
        if not csrf:
            raise CloudError("Bambu's sign-in page sent no CSRF cookie")
        status, data, cookies = _http(
            "POST", WEB + "/api/sign-in/tfa", {"tfaKey": tfa_key, "tfaCode": code},
            headers=dict(browser, Cookie="bbl_csrf_token=" + csrf, **{"x-bbl-csrf-token": csrf}))
        _keys("app code", status, data)
        if status == 403 and "csrf" in json.dumps(data or {}).lower():
            continue
        token, refresh = cookies.get("token"), None
        if not token and isinstance(data, dict):
            token = data.get("accessToken") or data.get("token")
            refresh = data.get("refreshToken")
        return (token if isinstance(token, str) and token else None, refresh)
    raise CloudError("Bambu's sign-in refused the CSRF check")


def refresh_token(region, refresh):
    """A new token from the refresh token, if Bambu gave one. Undocumented: None when it fails."""
    try:
        status, data, _ = _http("POST", API[region] + "/v1/user-service/user/refreshtoken",
                                {"refreshToken": refresh})
    except CloudError:
        return None
    _keys("refresh", status, data)
    token, new_refresh = _token_of(data)
    return {"token": token, "refresh": new_refresh or refresh} if token else None


def user_id(region, token):
    """The broker's user name, "u_<uid>": tokens are opaque, the profile has the uid."""
    status, data, _ = _http("GET", API[region] + "/v1/user-service/my/profile", token=token)
    if status in (401, 403):
        raise SignInNeeded("Bambu refused the saved sign-in: sign in again")
    if status != 200 or not isinstance(data, dict):
        raise CloudError(_message(data) or "Profile request failed (HTTP %d)" % status)
    uid = data.get("uidStr") or data.get("uid")
    if uid in (None, ""):
        raise CloudError("Bambu's profile has no user id")
    return "u_%s" % uid


def devices(region, token):
    """The printers bound to the account: [{"serial", "name", "model", "online"}]."""
    status, data, _ = _http("GET", API[region] + "/v1/iot-service/api/user/bind", token=token)
    if status in (401, 403):
        raise SignInNeeded("Bambu refused the saved sign-in: sign in again")
    if status != 200 or not isinstance(data, dict):
        raise CloudError(_message(data) or "Printer list request failed (HTTP %d)" % status)
    raw = data.get("devices") if isinstance(data.get("devices"), list) else data.get("data")
    out = []
    for d in raw if isinstance(raw, list) else []:
        serial = isinstance(d, dict) and (d.get("dev_id") or d.get("dev_id_str"))
        if serial:
            out.append({"serial": str(serial), "name": str(d.get("name") or d.get("dev_product_name") or serial),
                        "model": str(d.get("dev_product_name") or ""), "online": bool(d.get("online", True))})
    return out


# ---------------------------------------------------------------------------
# Reports -> the deck's printer state (deck_bambu.cpp report(), field for field)
# ---------------------------------------------------------------------------
def num(v):
    """A number from a report value: Bambu sends many as strings ("tray_now": "255")."""
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return v
    if isinstance(v, str):
        try:
            return float(v)
        except ValueError:
            return None
    return None


def uint32(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFFFFFF


def find_obj(node, key):
    """The first object named `key` anywhere in the message, in document order
    (the deck's readObject: "extruder" and "ctc" live under print.device on H2 / P2S)."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == key and isinstance(v, dict):
                return v
            found = find_obj(v, key)
            if found is not None:
                return found
    elif isinstance(node, list):
        for v in node:
            found = find_obj(v, key)
            if found is not None:
                return found
    return None


def _tray():
    return {"present": False, "rgb": 0, "alpha": 255, "type": "", "remain": -1}


def parse_tray(obj, tray, names):
    t, b = obj.get("tray_type"), obj.get("tray_sub_brands")
    t = t if isinstance(t, str) else None
    b = b if isinstance(b, str) else None
    if t is not None and not t and not b:  # an empty slot
        tray.clear()
        tray.update(_tray())
        names[:] = ["", ""]
        return
    if t is not None:
        names[0] = td_work.clip(t, MAX_TRAY - 1)
    if b is not None:
        names[1] = td_work.clip(b, MAX_TRAY - 1)
    if t is not None or b is not None:
        name = names[1] or names[0]
        tray["present"] = bool(name)
        tray["type"] = name
    color = obj.get("tray_color")
    if not isinstance(color, str) or len(color) < 6:
        cols = obj.get("cols")
        color = cols[0] if isinstance(cols, list) and cols else None
    if isinstance(color, str) and len(color) in (6, 8):
        try:
            rgba = int(color, 16)
        except ValueError:
            rgba = None
        if rgba is not None:
            tray["rgb"] = rgba >> 8 if len(color) == 8 else rgba
            tray["alpha"] = rgba & 255 if len(color) == 8 else 255
    remain = num(obj.get("remain"))
    if remain is not None:
        tray["remain"] = int(remain) if 0 < remain <= 100 else -1


class Printer:
    """One printer's state, from its reports (full or incremental)."""

    def __init__(self, serial, name="", model=""):
        self.serial, self.name, self.model = serial, name or serial, model
        self.rx = 0.0          # time.time() of the last report, 0 = none yet
        self.st = ""           # gcode_state
        self.job = ""
        self.pct = self.left = self.layer = self.layers = self.spd = 0
        self.stg = -1
        self.dual, self.an = False, 0
        self.noz = [[None, None], [None, None]]  # [current, target] per nozzle; [0] the only / right one
        self.bed = [None, None]
        self.ch = None
        self.door = -1
        self.light = -1        # chamber light: -1 unknown, 0 off, 1 on (shown, never switched from here)
        self.fan = [-1, -1, -1]  # part cooling, auxiliary, chamber fan in %, -1 = not reported
        self.duct = -1         # air duct mode (P2S): 0 cooling (outside air in), 1 heating (recirculates)
        self.perr = None       # print_error, None until reported
        self.hms = None        # [[attr, code]], None until reported
        self.ams = []          # [{"id", "slots", "hum", "temp", "dry", "tray": [tray x slots]}]
        self.au = self.as_ = 255
        self.ext = _tray()
        self.map = []
        # Report bookkeeping, as in the deck's Session
        self.snow, self.snow_seen = [0xFFFF, 0xFFFF], [False, False]
        self.tray_now = 255
        self.names = {}        # (unit id, slot) -> [type, brand]
        self.ext_names = ["", ""]
        self.task_id = ""
        self.ctc_seen = False

    def apply(self, msg):
        p = msg.get("print") if isinstance(msg.get("print"), dict) else {}
        snow_updated = [False, False]
        ext = find_obj(msg, "extruder")
        if ext is not None:
            info = [e for e in ext.get("info") or [] if isinstance(e, dict)]
            if len(info) >= 2 or any(num(e.get("id")) == 1 for e in info):
                self.dual = True
            state = num(ext.get("state"))
            if self.dual and state is not None:
                an = (int(state) >> 4) & 15
                self.an = an if an <= 1 else 0
            for e in info:
                i = num(e.get("id"))
                if i not in (0, 1):
                    continue
                i = int(i)
                temp = num(e.get("temp"))
                if temp is not None:
                    packed = int(temp) & 0xFFFFFFFF
                    self.noz[i] = [packed & 0xFFFF, packed >> 16]
                snow = num(e.get("snow"))
                if snow is not None:
                    self.snow[i], self.snow_seen[i], snow_updated[i] = int(snow) & 0xFFFF, True, True
        ctc = find_obj(msg, "ctc")
        if ctc is not None and isinstance(ctc.get("info"), dict) and num(ctc["info"].get("temp")) is not None:
            self.ch = int(num(ctc["info"]["temp"])) & 0xFFFF
            self.ctc_seen = True
        ams = find_obj(msg, "ams")
        if ams is not None:
            units = ams.get("ams")
            if isinstance(units, list) and not units:
                self.ams, self.names = [], {}
            for obj in units if isinstance(units, list) else []:
                uid = num(obj.get("id")) if isinstance(obj, dict) else None
                if uid is None or not 0 <= uid < 254:
                    continue
                uid = int(uid)
                unit = next((u for u in self.ams if u["id"] == uid), None)
                if unit is None:
                    if len(self.ams) == AMS_UNITS:
                        continue
                    slots = 1 if uid >= 128 else 4
                    unit = {"id": uid, "slots": slots, "hum": 255, "temp": None, "dry": 0,
                            "tray": [_tray() for _ in range(slots)]}
                    self.ams.append(unit)
                hum = num(obj.get("humidity_raw"))
                if hum is not None:
                    unit["hum"] = int(hum) if 0 <= hum <= 100 else 255
                temp = num(obj.get("temp"))
                if temp is not None:
                    unit["temp"] = float(temp)
                dry = num(obj.get("dry_time"))
                if dry is not None:
                    unit["dry"] = max(0, min(65535, int(dry)))
                for tr in obj.get("tray") or []:
                    tid = num(tr.get("id")) if isinstance(tr, dict) else None
                    if tid is not None and 0 <= tid < unit["slots"]:
                        tid = int(tid)
                        parse_tray(tr, unit["tray"][tid], self.names.setdefault((uid, tid), ["", ""]))
            tray_now = num(ams.get("tray_now"))
            if tray_now is not None:
                self.tray_now = int(tray_now) & 0xFFFF
                if not self.dual and not snow_updated[0]:
                    self.snow_seen[0] = False
        vt = find_obj(msg, "vt_tray")
        if vt is not None:
            parse_tray(vt, self.ext, self.ext_names)
        self._active_tray()

        if isinstance(p.get("gcode_state"), str):
            self.st = p["gcode_state"][:15]
        if isinstance(p.get("subtask_name"), str):
            name = td_work.clip(p["subtask_name"], MAX_JOB - 1)
            if name != self.job:
                self.map = []
            self.job = name
        if p.get("task_id") is not None:
            tid = str(p["task_id"])[:31]
            if self.task_id and tid != self.task_id:
                self.map = []
            self.task_id = tid
        if isinstance(p.get("mapping"), list):
            self.map = [int(v) for v in p["mapping"] if num(v) is not None and 0 <= num(v) < 65535][:MAP_MAX]
        for key, attr, limit in (("mc_percent", "pct", 100), ("mc_remaining_time", "left", 65535),
                                 ("layer_num", "layer", 65535), ("total_layer_num", "layers", 65535),
                                 ("spd_lvl", "spd", 4)):
            v = num(p.get(key))
            if v is not None:
                setattr(self, attr, max(0, min(limit, int(v))))
        stage = num(p.get("stg_cur"))
        if stage is not None:
            self.stg = -1 if stage < 0 or stage >= 255 else int(stage)
        if not self.dual:
            for key, k in (("nozzle_temper", 0), ("nozzle_target_temper", 1)):
                v = num(p.get(key))
                if v is not None:
                    self.noz[0][k] = float(v)
        for key, k in (("bed_temper", 0), ("bed_target_temper", 1)):
            v = num(p.get(key))
            if v is not None:
                self.bed[k] = float(v)
        # Fans as 0..15 gears; Bambu Studio shows floor(gear / 1.5) * 10 %.
        for i, key in enumerate(("cooling_fan_speed", "big_fan1_speed", "big_fan2_speed")):
            v = num(p.get(key))
            if v is not None:
                self.fan[i] = min(100, int(max(0, v) // 1.5) * 10)
        airduct = (p.get("device") or {}).get("airduct") if isinstance(p.get("device"), dict) else None
        if isinstance(airduct, dict) and num(airduct.get("modeCur")) is not None:
            self.duct = int(num(airduct["modeCur"]))
        for light in p.get("lights_report") or []:
            if isinstance(light, dict) and light.get("node") == "chamber_light" and isinstance(light.get("mode"), str):
                self.light = 0 if light["mode"] == "off" else 1
        x1 = self.serial.startswith(("00M", "03W"))  # X1C / X1E
        if not self.ctc_seen and x1 and num(p.get("chamber_temper")) is not None:
            self.ch = float(num(p["chamber_temper"]))  # P1 reports a bogus one
        # Door: X1 in home_flag; H2 (dual nozzle) and P2S ("22E") in stat; P1 / A1 have none.
        if x1 and num(p.get("home_flag")) is not None:
            self.door = (int(num(p["home_flag"])) >> 23) & 1
        elif not x1 and (self.dual or self.serial.startswith("22E")) and isinstance(p.get("stat"), str):
            try:
                self.door = 1 if int(p["stat"], 16) & 0x00800000 else 0
            except ValueError:
                pass
        if uint32(p.get("print_error")):
            self.perr = p["print_error"]
        if isinstance(p.get("hms"), list):
            self.hms = [[e["attr"], e["code"]] for e in p["hms"]
                        if isinstance(e, dict) and uint32(e.get("attr")) and uint32(e.get("code"))][:HMS_MAX]
        self.rx = time.time()

    def _active_tray(self):
        n = self.an
        if self.snow_seen[n]:
            unit, slot = self.snow[n] >> 8, self.snow[n] & 3
        else:
            unit = self.tray_now if self.tray_now >= 254 else self.tray_now // 4
            slot = self.tray_now % 4
        self.au, self.as_ = (254 if unit == 254 else 255), 255
        if unit >= 254:
            return
        for i, u in enumerate(self.ams):
            if u["id"] == unit and slot < u["slots"]:
                self.au, self.as_ = i, slot
                break

    def printing(self):
        return self.st in ("RUNNING", "PREPARE", "SLICING", "PAUSE")

    def deck(self, link, err=""):
        """This printer for the deck (POST /api/pc/printers), without the report's age."""
        tray = lambda t: [int(t["present"]), t["rgb"], t["alpha"], t["type"], t["remain"]]
        out = {"serial": self.serial, "name": td_work.clip(self.name, MAX_NAME - 1),
               "model": td_work.clip(self.model, 23), "link": link}
        if err:
            out["err"] = td_work.clip(err, MAX_ERR - 1)
        if not self.rx:
            return out
        out.update({
            "st": self.st, "job": self.job, "pct": self.pct, "left": self.left,
            "layer": self.layer, "layers": self.layers, "spd": self.spd, "stg": self.stg,
            "dual": int(self.dual), "an": self.an, "noz": self.noz if self.dual else self.noz[:1],
            "bed": self.bed, "ch": self.ch, "door": self.door, "light": self.light, "fan": self.fan, "duct": self.duct,
            "ams": [{"id": u["id"], "n": u["slots"], "h": u["hum"], "t": u["temp"], "dry": u["dry"],
                     "tr": [tray(t) for t in u["tray"]]} for u in self.ams],
            "au": self.au, "as": self.as_, "ext": tray(self.ext), "map": self.map})
        if self.perr is not None:
            out["perr"] = self.perr
        if self.hms is not None:
            out["hms"] = self.hms
        return out


# ---------------------------------------------------------------------------
# The cloud session and the pushes to the deck
# ---------------------------------------------------------------------------
class Cloud(threading.Thread):
    """One broker session for the account, every picked printer on it.
    configure() takes a new config while it runs (the settings page signs in,
    picks printers or signs out)."""

    def __init__(self, deck_addr, pc_id, pc_name, verbose=False, path=CONFIG_FILE):
        super().__init__(daemon=True, name="td_bambu")
        self.deck_addr, self.pc_id, self.pc_name, self.verbose, self.path = deck_addr, pc_id, pc_name, verbose, path
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.client = None
        self.cfg, self.printers = {}, {}
        self.state, self.err = "off", ""
        self.retry_at, self.retries = 0.0, 0
        self.connected_at, self.pushall_at = 0.0, {}
        self.pushed, self.pushed_at = None, 0.0
        self.last_push = {"ok": None, "at": 0.0, "addr": ""}
        self.had_printers = False  # printers removed: one push with none clears the deck
        self.refreshed_at = -3600.0
        self.restart = True
        self.configure(load_config(path))

    # -- settings page -------------------------------------------------------
    def configure(self, cfg):
        with self.lock:
            self.cfg = dict(cfg or {})
            picked = [p for p in self.cfg.get("printers") or [] if isinstance(p, dict) and p.get("serial")][:PRN_MAX]
            old = self.printers
            self.printers = {}
            for p in picked:
                pr = old.get(p["serial"]) or Printer(p["serial"])
                pr.name, pr.model = p.get("name") or p["serial"], p.get("model") or ""
                self.printers[pr.serial] = pr
            self.restart, self.retry_at, self.retries = True, 0.0, 0
            self.pushed = None
        self.wake.set()

    def status(self):
        with self.lock:
            now = time.time()
            return {"state": self.state, "err": self.err,
                    "printers": [{"serial": p.serial, "name": p.name, "model": p.model,
                                  "link": self._link(p, now)[0], "rx": p.rx, "st": p.st, "pct": p.pct}
                                 for p in self.printers.values()],
                    "push": dict(self.last_push)}

    # -- broker --------------------------------------------------------------
    def _set(self, state, err=""):
        with self.lock:
            changed = (self.state, self.err) != (state, err)
            self.state, self.err = state, err
        if changed and (err or self.verbose):
            print("bambu: %s%s" % (state, ": " + err if err else ""), flush=True)

    def _stop_client(self):
        if self.client:
            try:
                self.client.disconnect()
            except Exception:
                pass
            self.client = None

    def _connect(self):
        import paho.mqtt.client as mqtt
        with self.lock:
            cfg, serials = dict(self.cfg), list(self.printers)
            self.restart = False
        region, token = region_of(cfg), cfg.get("token") or ""
        if not token:
            self._set("signin" if serials else "off", "Sign in to Bambu Cloud" if serials else "")
            return
        if not serials:
            self._set("off")
            return
        self._set("connecting")
        try:
            user = user_id(region, token)
        except SignInNeeded as e:
            if not self._refresh(cfg):
                self._set("signin", str(e))
            return
        except CloudError as e:
            self._fail(str(e))
            return
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="touchdeck_" + secrets.token_hex(4),
                        protocol=mqtt.MQTTv311)
        c.username_pw_set(user, token)
        c.tls_set()  # the system's CAs: Bambu's broker has a public certificate
        c.on_connect, c.on_message, c.on_disconnect = self._on_connect, self._on_message, self._on_disconnect
        c.user_data_set({"serials": serials, "region": region})
        try:
            c.connect(BROKER[region], 8883, keepalive=30)
        except (OSError, ValueError) as e:
            self._fail("Can't reach Bambu's broker: %s" % e)
            return
        self.client, self.connected_at = c, time.time()

    def _refresh(self, cfg):
        """One try with the refresh token, when Bambu gave one at sign-in, and
        not again within the hour: a renewed token that is refused too means
        signing in again."""
        if not cfg.get("refresh") or time.monotonic() - self.refreshed_at < 3600:
            return False
        self.refreshed_at = time.monotonic()
        got = refresh_token(region_of(cfg), cfg["refresh"])
        if not got:
            return False
        cfg.update(token=got["token"], refresh=got["refresh"])
        save_config(cfg, self.path)
        self.configure(cfg)
        print("bambu: sign-in renewed", flush=True)
        return True

    def _fail(self, err):
        self._stop_client()
        delay = RETRY_S[min(self.retries, len(RETRY_S) - 1)]
        self.retries += 1
        self.retry_at = time.monotonic() + delay
        self._set("error", "%s (retry in %d s)" % (err, delay))

    def _on_connect(self, client, userdata, flags, reason, properties=None):
        if reason.is_failure:
            # CONNACK 4 / 5: the token is no longer good. Retrying would only hammer the broker.
            if str(reason) in ("Bad user name or password", "Not authorized") or reason.value in (4, 5, 134, 135):
                self._stop_client()
                if not self._refresh(dict(self.cfg)):
                    self._set("signin", "Bambu refused the saved sign-in: sign in again")
                    self.retry_at = float("inf")
            else:
                self._fail("Broker refused the session: %s" % reason)
            return
        for serial in userdata["serials"]:
            client.subscribe("device/%s/report" % serial, qos=0)
            self._pushall(serial)  # the state now: an idle printer may say nothing for long
        self.retries = 0
        self._set("online")

    def _on_disconnect(self, client, userdata, flags, reason, properties=None):
        if self.client is client:  # ours, not one stopped on purpose: wait before the next try
            self.client = None
            self._fail("Disconnected from Bambu's broker (%s)" % reason)

    def _on_message(self, client, userdata, msg):
        serial = msg.topic.split("/")[1] if msg.topic.count("/") == 2 else ""
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except ValueError:
            return
        with self.lock:
            pr = self.printers.get(serial)
            if pr and isinstance(data, dict):
                pr.apply(data)

    def _pushall(self, serial):
        c = self.client
        if not c:
            return
        body = json.dumps({"pushing": {"sequence_id": str(int(time.time()) % 100000), "command": "pushall",
                                       "version": 1, "push_target": 1}})
        c.publish("device/%s/request" % serial, body, qos=0)
        self.pushall_at[serial] = time.time()
        if self.verbose:
            print("bambu: status request to %s" % serial[-4:], flush=True)

    def _requests(self):
        """Status requests, sparingly: idle printers every PUSHALL_IDLE_S once they
        have answered (an offline one gets only the request on connecting), a
        printing one only if it went quiet, and never two within PUSHALL_GAP_S.
        One silent for SILENT_S is off (or was switched off mid-print, its last
        state still "printing"): only one request every PUSHALL_OFF_S; its
        first report on waking brings back the normal pace."""
        now = time.time()
        with self.lock:
            due = []
            for p in self.printers.values():
                last = self.pushall_at.get(p.serial, 0.0)
                if now - last < PUSHALL_GAP_S or not p.rx or p.rx < self.connected_at:
                    continue
                quiet = now - p.rx
                if quiet > SILENT_S:
                    if now - last >= PUSHALL_OFF_S:
                        due.append(p.serial)
                    continue
                if (not p.printing() and now - last >= PUSHALL_IDLE_S) or (p.printing() and quiet >= STALE_JOB_S):
                    due.append(p.serial)
        for serial in due:
            self._pushall(serial)

    def _link(self, p, now):
        if self.state == "signin":
            return "error", "Sign in again on %s's agent" % self.pc_name
        if self.state == "error":
            return "error", self.err
        if self.state != "online" or not p.rx:
            return "connecting", ""
        if now - p.rx > SILENT_S:
            return "offline", "Printer off or not reporting"
        return "online", ""

    # -- deck ------------------------------------------------------------------
    def _push(self):
        with self.lock:
            if self.printers:
                self.had_printers = True
            elif not self.had_printers:
                return
            now = time.time()
            printers = []
            for p in self.printers.values():
                link, err = self._link(p, now)
                printers.append(p.deck(link, err))
            ages = [int(now - p.rx) if p.rx else -1 for p in self.printers.values()]
        body = json.dumps({"pc_id": self.pc_id, "pc_name": self.pc_name, "printers": printers},
                          separators=(",", ":"), ensure_ascii=False)
        mono = time.monotonic()
        if body == self.pushed and mono - self.pushed_at < PUSH_EVERY_S:
            return
        if body != self.pushed and mono - self.pushed_at < PUSH_GAP_S:
            return
        addr = self.deck_addr()
        if addr == "255.255.255.255":
            return
        # Each printer's report age: with two PCs on one printer, the deck keeps the freshest.
        full = json.loads(body)
        for pr, age in zip(full["printers"], ages):
            pr["age"] = age
        ok = push(addr, json.dumps(full, separators=(",", ":"), ensure_ascii=False), self.verbose)
        with self.lock:
            self.last_push = {"ok": ok, "at": time.time(), "addr": addr}
            if ok:
                self.pushed, self.pushed_at = body, mono
                if not self.printers:
                    self.had_printers = False

    def run(self):
        failing = None
        while True:
            try:
                if self.restart:
                    self._stop_client()
                    if time.monotonic() >= self.retry_at:
                        self._connect()
                elif not self.client and self.state in ("error", "connecting") and time.monotonic() >= self.retry_at:
                    self.restart = True
                    continue
                if self.client:
                    self.client.loop(timeout=1.0)
                    self._requests()
                else:
                    self.wake.wait(1.0)
                    self.wake.clear()
                self._push()
            except Exception as e:  # one bad report or push must not end the printer updates
                if str(e) != failing:
                    print("bambu: %s (logged once until it changes)" % e, flush=True)
                    failing = str(e)
                time.sleep(1)


def push(addr, body, verbose=False):
    req = urllib.request.Request("http://%s/api/pc/printers" % addr, data=body.encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
        if verbose:
            print("bambu: pushed %d B to %s" % (len(body), addr), flush=True)
        return True
    except (urllib.error.URLError, OSError) as e:
        if verbose:
            print("bambu: push to %s failed: %s" % (addr, e), flush=True)
        return False


SAMPLE = [{
    "serial": "22E00SAMPLE0001", "name": "P2S", "model": "P2S", "link": "online", "age": 2,
    "st": "RUNNING", "job": "Benchy_PLA_0.2mm", "pct": 42, "left": 83, "layer": 96, "layers": 230,
    "spd": 2, "stg": 0, "dual": 0, "an": 0, "noz": [[219, 220]], "bed": [55, 55], "ch": None, "door": 0,
    "ams": [{"id": 0, "n": 4, "h": 21, "t": 26.5, "dry": 0,
             "tr": [[1, 0xF5F5F5, 255, "PLA Basic", 80], [1, 0x101010, 255, "PLA Matte", 35],
                    [1, 0xE83A2F, 255, "PETG HF", -1], [0, 0, 255, "", -1]]}],
    "au": 0, "as": 1, "ext": [0, 0, 255, "", -1], "map": [1], "perr": 0, "hms": []}]


def main():
    ap = argparse.ArgumentParser(description="CompaDeck: Bambu printers through Bambu Cloud")
    ap.add_argument("--sample", action="store_true", help="push a sample printer to the deck")
    ap.add_argument("--host", default=os.environ.get("TOUCHDECK_HOST", "companion.local"))
    args = ap.parse_args()
    host = platform.node() or "PC"
    pc_id, pc_name = hashlib.sha1(host.encode()).hexdigest()[:12], td_work.clip(host, 23)
    if args.sample:
        body = json.dumps({"pc_id": pc_id, "pc_name": pc_name, "printers": SAMPLE}, separators=(",", ":"))
        print("pushed" if push(args.host, body, True) else "push failed")
        return
    ap.print_help()


if __name__ == "__main__":
    main()
