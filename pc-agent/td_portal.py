#!/usr/bin/env python3
"""
The agent's settings page: http://127.0.0.1:4213 (python touchdeck_agent.py settings).

A FastAPI app run by the agent in a thread. It serves the React front end built
in portal/dist (cd portal && npm install && npm run build) and a small API:

    GET  /api/state    config without tokens, the agent's status
    PUT  /api/config   saves ~/.touchdeck/work.json and reconfigures the worker
    POST /api/test     polls GitLab / Jira with the form, saves nothing
    GET  /api/logs     the end of the agent's log (macOS service, windows/touchdeck.ps1)
    POST /api/bambu/signin, /code, /resend, /restart, /signout, GET /devices,
    PUT  /api/bambu/printers   Bambu Cloud sign-in and printers (td_bambu.py)

It listens on 127.0.0.1 only, so the tokens never leave this machine and no
firewall prompt shows. A token is never sent back: the page only learns whether
one is saved. Other local pages can't drive it: the Host header must be this
address (DNS rebinding) and every API call carries a per-run session value, in
a header, that only the served page knows (cross-site requests). The session is
also written to ~/.touchdeck/portal.session (mode 600) for the Vite dev server.
"""
import hmac
import os
import re
import secrets
import socket
import sys
import threading
import time
import urllib.parse
from typing import List, Optional

import td_bambu
import td_work

PORT = 4213
HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, "portal", "dist")
SESSION_FILE = os.path.join(os.path.dirname(td_work.CONFIG_FILE), "portal.session")
# Where the agent's output goes when it runs in the background: the macOS
# LaunchAgent, or windows/touchdeck.ps1 (pythonw, no console, --log).
if os.name == "nt":
    LOG_FILE = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "TouchDeck", "agent.log")
else:
    LOG_FILE = os.path.join(os.path.expanduser("~"), "Library", "Logs", "TouchDeck", "agent.log")
COLOR = re.compile(r"^#[0-9A-Fa-f]{6}$")


class BadInput(ValueError):
    pass


# ---------------------------------------------------------------------------
# Config <-> form (plain dicts, so the rules don't depend on the web layer)
# ---------------------------------------------------------------------------
def public_config(cfg):
    """The config without its tokens, plus whether each one is saved."""
    cfg = cfg or {}
    g = dict(cfg.get("gitlab") or {})
    g["token_set"] = bool(g.pop("token", ""))
    g.setdefault("enabled", bool(cfg.get("gitlab")))
    site = td_work.atlassian_site(cfg)
    j = {k: v for k, v in (cfg.get("jira") or {}).items() if k not in ("url", "email", "token", "active_jql")}
    j.setdefault("enabled", bool(site.get("token")))
    c = dict(cfg.get("confluence") or {})
    c.setdefault("enabled", False)
    return {"gitlab": g,
            "atlassian": {"url": site.get("url", ""), "email": site.get("email", ""),
                          "token_set": bool(site.get("token"))},
            "jira": j, "confluence": c}


def _text(form, key, max_len=300):
    v = form.get(key)
    v = "" if v is None else str(v).strip()
    if len(v) > max_len:
        raise BadInput("%s is too long" % key)
    return v


def _url(form, label):
    url = _text(form, "url").rstrip("/")
    if not url:
        return ""
    p = urllib.parse.urlsplit(url)
    if p.scheme != "https" or not p.netloc:  # the token goes with every request
        raise BadInput("%s: the URL must start with https://" % label)
    return url


def _every(form, low, default):
    v = form.get("every_s")
    if v in (None, ""):
        return default
    try:
        v = int(v)
    except (TypeError, ValueError):
        raise BadInput("every_s must be a number of seconds")
    return max(low, min(3600, v))


def _lines(form, key, max_items):
    raw = form.get(key) or []
    if isinstance(raw, str):
        raw = re.split(r"[\n,]", raw)
    items = [str(x).strip() for x in raw if str(x).strip()]
    if len(items) > max_items:
        raise BadInput("%s: %d at most" % (key, max_items))
    return items


def _token(old, form, url):
    """The token to keep: a new one typed, none if cleared, else the saved one,
    but never the saved one for another server (it would go to that server)."""
    new = _text(form, "token", 2000)
    if new:
        return new
    if form.get("clear_token"):
        return ""
    if old.get("token") and (old.get("url") or "").rstrip("/") == url:
        return old["token"]
    return ""


def merge(old_cfg, form):
    """The config to save from the page's form and the current config."""
    old_cfg = old_cfg or {}
    keep = ("gitlab", "atlassian", "jira", "confluence")
    cfg = {k: v for k, v in old_cfg.items() if k not in keep}

    g, og = form.get("gitlab") or {}, old_cfg.get("gitlab") or {}
    url = _url(g, "GitLab")
    gl = {"enabled": bool(g.get("enabled")), "url": url, "token": _token(og, g, url),
          "projects": _lines(g, "projects", 8),
          "branches": _lines(g, "branches", 10) or ["main", "master", "develop"],
          "every_s": _every(g, 30, 60)}
    if gl["url"] or gl["token"]:
        cfg["gitlab"] = gl

    # One Atlassian account for Jira and Confluence.
    a = form.get("atlassian") or {}
    url = _url(a, "Atlassian")
    site = {"url": url, "token": _token(td_work.atlassian_site(old_cfg), a, url)}
    email = _text(a, "email")
    if email:
        site["email"] = email
    if site["url"] or site["token"]:
        cfg["atlassian"] = site

    j = form.get("jira") or {}
    ji = {"enabled": bool(j.get("enabled")), "every_s": _every(j, 60, 120)}
    cols = []
    for c in (j.get("columns") or [])[:td_work.MAX_COLS + 1]:
        label, jql, color = _text(c, "label", 40), _text(c, "jql", 1000), _text(c, "color", 7)
        if not label and not jql:
            continue
        if not label or not jql:
            raise BadInput("Jira: each filter needs a label and a JQL")
        cols.append({"label": label, "jql": jql, "color": color.upper() if COLOR.match(color) else "#E6E9EE"})
    if len(cols) > td_work.MAX_COLS:
        raise BadInput("Jira: %d filters at most" % td_work.MAX_COLS)
    if cols:
        ji["columns"] = cols
    cfg["jira"] = ji

    cfg["confluence"] = {"enabled": bool((form.get("confluence") or {}).get("enabled"))}
    return cfg


def summary(results):
    """Test results: counts and labels, nothing else."""
    out = {}
    for name, data in results.items():
        if data.get("err"):
            out[name] = {"err": data["err"]}
        elif name == "gitlab":
            sec = data.get("sec") or [0] * len(td_work.SECTIONS)
            out[name] = {"active": data.get("active", 0), "review": sec[td_work.SEC["review"]],
                         "mine": sec[td_work.SEC["mine"]], "todos": data.get("todos", 0),
                         "pipelines": len(data.get("pipelines") or []), "ko": data.get("ko", 0)}
        elif name == "jira":
            out[name] = {"cols": [[c[0], c[1]] for c in data.get("cols") or []],
                         "tickets": sum(len(c[3]) for c in data.get("cols") or [] if len(c) > 3),
                         "gateway": bool(data.get("gateway"))}
        elif name == "confluence":
            out[name] = {"user": data.get("user", ""), "gateway": bool(data.get("gateway"))}
    return out


def tail(path, lines):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 64 * 1024))
            return f.read().decode("utf-8", "replace").splitlines()[-lines:]
    except OSError:
        return None


def port_in_use(port=PORT):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


# ---------------------------------------------------------------------------
# Web app
# ---------------------------------------------------------------------------
def create_app(portal):
    from fastapi import FastAPI, HTTPException, Request
    from fastapi.exceptions import RequestValidationError
    from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
    from fastapi.staticfiles import StaticFiles
    from pydantic import BaseModel, Field

    class Column(BaseModel):
        label: str = Field("", max_length=40)
        jql: str = Field("", max_length=1000)
        color: str = Field("#E6E9EE", max_length=7)

    class GitLabForm(BaseModel):
        enabled: bool = False
        url: str = Field("", max_length=300)
        token: str = Field("", max_length=2000)
        clear_token: bool = False
        projects: List[str] = Field(default_factory=list, max_length=8)
        branches: List[str] = Field(default_factory=list, max_length=10)
        every_s: Optional[int] = Field(None, ge=1, le=3600)

    class AtlassianForm(BaseModel):
        url: str = Field("", max_length=300)
        email: str = Field("", max_length=300)
        token: str = Field("", max_length=2000)
        clear_token: bool = False

    class JiraForm(BaseModel):
        enabled: bool = False
        columns: List[Column] = Field(default_factory=list, max_length=td_work.MAX_COLS)
        every_s: Optional[int] = Field(None, ge=1, le=3600)

    class ConfluenceForm(BaseModel):
        enabled: bool = False

    class ConfigForm(BaseModel):
        gitlab: GitLabForm = Field(default_factory=GitLabForm)
        atlassian: AtlassianForm = Field(default_factory=AtlassianForm)
        jira: JiraForm = Field(default_factory=JiraForm)
        confluence: ConfluenceForm = Field(default_factory=ConfluenceForm)

    app = FastAPI(title="CompaDeck agent", docs_url=None, redoc_url=None, openapi_url=None)

    @app.middleware("http")
    async def guard(request: Request, call_next):
        if request.headers.get("host", "") not in portal.hosts:
            return JSONResponse({"detail": "wrong host"}, status_code=403)
        if request.url.path.startswith("/api/") and not hmac.compare_digest(
                request.headers.get("x-td-session", ""), portal.session):
            return JSONResponse({"detail": "Reload the page"}, status_code=403)
        response = await call_next(request)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
        return response

    @app.exception_handler(BadInput)
    async def bad_input(request, exc):
        return JSONResponse({"detail": str(exc)}, status_code=400)

    @app.exception_handler(RequestValidationError)
    async def invalid(request, exc):
        # Pydantic echoes the rejected input: never send a token back.
        errors = [{"loc": list(e.get("loc", ())), "msg": e.get("msg", "")} for e in exc.errors()]
        return JSONResponse({"detail": errors}, status_code=422)

    @app.get("/api/state")
    def state():
        return portal.state()

    @app.put("/api/config")
    def save(form: ConfigForm):
        return portal.save(form.model_dump())

    @app.post("/api/test")
    def test(form: ConfigForm):
        return portal.test(form.model_dump())

    class AwakeForm(BaseModel):
        on: Optional[bool] = None
        jiggle: Optional[bool] = None

    @app.put("/api/awake")
    def awake(form: AwakeForm):
        if not portal.awake:
            raise HTTPException(404, "Keep awake is not available on this system")
        return portal.awake.set(form.on, form.jiggle)

    @app.post("/api/awake/permission")
    def awake_permission():
        if not portal.awake:
            raise HTTPException(404, "Keep awake is not available on this system")
        return {"granted": portal.awake.ask_permission(), "awake": portal.awake.status()}

    class SignInForm(BaseModel):
        region: str = Field("global", max_length=10)
        account: str = Field(..., min_length=3, max_length=200)
        password: str = Field(..., min_length=1, max_length=200)

    class CodeForm(BaseModel):
        code: str = Field(..., min_length=4, max_length=12)

    class PickedPrinter(BaseModel):
        serial: str = Field(..., min_length=4, max_length=40, pattern=r"^[A-Za-z0-9]+$")
        name: str = Field("", max_length=60)
        model: str = Field("", max_length=40)

    class PrintersForm(BaseModel):
        printers: List[PickedPrinter] = Field(default_factory=list, max_length=td_bambu.PRN_MAX)

    def bambu():
        if not portal.bambu:
            raise HTTPException(404, "Bambu printers are off: see the agent's log (is paho-mqtt installed?)")
        return portal

    @app.post("/api/bambu/signin")
    def bambu_signin(form: SignInForm):
        return bambu().bambu_signin(form.region, form.account, form.password)

    @app.post("/api/bambu/code")
    def bambu_code(form: CodeForm):
        return bambu().bambu_code(form.code)

    @app.post("/api/bambu/resend")
    def bambu_resend():
        return bambu().bambu_resend()

    @app.post("/api/bambu/restart")
    def bambu_restart():
        return bambu().bambu_restart()

    @app.post("/api/bambu/signout")
    def bambu_signout():
        return bambu().bambu_signout()

    @app.get("/api/bambu/devices")
    def bambu_devices():
        return bambu().bambu_devices()

    @app.put("/api/bambu/printers")
    def bambu_printers(form: PrintersForm):
        return bambu().bambu_pick([p.model_dump() for p in form.printers])

    @app.get("/api/logs")
    def logs(lines: int = 200):
        got = tail(portal.log_file, max(1, min(1000, lines)))
        return {"path": portal.log_file, "lines": got or [], "exists": got is not None}

    @app.api_route("/api/{rest:path}", methods=["GET", "POST", "PUT", "DELETE"])
    def api_404(rest: str):
        raise HTTPException(404, "Not found")

    index = os.path.join(DIST, "index.html")
    if os.path.isdir(os.path.join(DIST, "assets")):
        app.mount("/assets", StaticFiles(directory=os.path.join(DIST, "assets")), name="assets")

    @app.get("/{rest:path}", include_in_schema=False)
    def page(rest: str):
        # Only files under dist/: os.path.join drops DIST for an absolute `rest` ("//etc/hosts").
        root = os.path.realpath(DIST)
        path = os.path.realpath(os.path.join(root, rest)) if rest else ""
        if path.startswith(root + os.sep) and os.path.isfile(path):
            return FileResponse(path)
        try:
            with open(index, encoding="utf-8") as f:
                html = f.read()
        except OSError:
            return HTMLResponse(
                "<h1>CompaDeck agent</h1><p>The settings page is not built yet:</p>"
                "<pre>cd pc-agent/portal &amp;&amp; npm install &amp;&amp; npm run build</pre>", status_code=503)
        return HTMLResponse(html.replace("__TD_SESSION__", portal.session))

    return app


class Portal:
    def __init__(self, worker, deck_host, deck_addr, pc_name, port=PORT, path=td_work.CONFIG_FILE,
                 log_file=LOG_FILE, awake=None, bambu=None, bambu_path=td_bambu.CONFIG_FILE):
        self.worker, self.deck_host, self.deck_addr, self.pc_name = worker, deck_host, deck_addr, pc_name
        self.awake = awake
        self.bambu, self.bambu_path = bambu, bambu_path
        self.pending = None  # a Bambu sign-in waiting for its code: {region, account, kind, tfa_key, at}
        self.port, self.path, self.log_file = port, path, log_file
        self.session = secrets.token_urlsafe(24)
        self.hosts = {"127.0.0.1:%d" % port, "localhost:%d" % port}
        self.save_lock = threading.Lock()
        self.started = time.time()

    @property
    def url(self):
        return "http://127.0.0.1:%d/" % self.port

    def state(self):
        cfg = td_work.load_config(self.path) or {}
        return {"config": public_config(cfg), "path": self.path, "pc_name": self.pc_name,
                "deck": {"host": self.deck_host, "addr": self.deck_addr()},
                "status": self.worker.status() if self.worker else None,
                "awake": self.awake.status() if self.awake else None,
                "bambu": self.bambu_state(),
                "started": self.started, "now": time.time()}

    # -- Bambu Cloud (td_bambu.py) ---------------------------------------------
    PENDING_S = 900  # a code request older than this starts over

    def bambu_state(self):
        if not self.bambu:
            return None
        cfg = td_bambu.load_config(self.bambu_path)
        p = self.pending if self.pending and time.time() - self.pending["at"] < self.PENDING_S else None
        return {"region": td_bambu.region_of(cfg), "account": cfg.get("account", ""),
                "signed_in": bool(cfg.get("token")),
                "printers": [{k: x.get(k, "") for k in ("serial", "name", "model")}
                             for x in cfg.get("printers") or [] if isinstance(x, dict)],
                "pending": {"kind": p["kind"], "account": p["account"]} if p else None,
                "status": self.bambu.status()}

    def _bambu_signed_in(self, region, account, got):
        """Saves the sign-in; with no printer picked yet and only one on the account, picks it."""
        with self.save_lock:
            cfg = td_bambu.load_config(self.bambu_path)
            if cfg.get("account", "").lower() != account.lower() or td_bambu.region_of(cfg) != region:
                cfg["printers"] = []  # another account: its printers aren't these
            cfg.update(region=region, account=account, token=got["token"])
            if got.get("refresh"):
                cfg["refresh"] = got["refresh"]
            else:
                cfg.pop("refresh", None)
            found = None
            try:
                found = td_bambu.devices(region, got["token"])
            except td_bambu.CloudError as e:
                print("bambu: printer list after sign-in: %s" % e, flush=True)
            if not cfg.get("printers") and found and len(found) == 1:
                cfg["printers"] = [{k: found[0][k] for k in ("serial", "name", "model")}]
            td_bambu.save_config(cfg, self.bambu_path)
            self.bambu.configure(cfg)
        self.pending = None
        return {"devices": found, "bambu": self.bambu_state()}

    def bambu_signin(self, region, account, password):
        region = "cn" if region == "cn" else "global"
        account = account.strip()
        try:
            got = td_bambu.login(region, account, password)
        except td_bambu.CloudError as e:
            raise BadInput(str(e))
        if got.get("need"):
            self.pending = {"region": region, "account": account, "kind": got["need"],
                            "tfa_key": got.get("tfa_key", ""), "at": time.time()}
            return {"need": got["need"], "bambu": self.bambu_state()}
        return self._bambu_signed_in(region, account, got)

    def bambu_code(self, code):
        p = self.pending
        if not p or time.time() - p["at"] >= self.PENDING_S:
            self.pending = None
            raise BadInput("The code request expired: sign in again")
        try:
            got = td_bambu.verify_code(p["region"], p["account"], code, p["kind"], p["tfa_key"])
        except td_bambu.CloudError as e:
            raise BadInput(str(e))
        return self._bambu_signed_in(p["region"], p["account"], got)

    def bambu_resend(self):
        p = self.pending
        if not p or p["kind"] != "email":
            raise BadInput("No emailed code is pending")
        try:
            td_bambu.send_email_code(p["region"], p["account"])
        except td_bambu.CloudError as e:
            raise BadInput(str(e))
        p["at"] = time.time()
        return {"bambu": self.bambu_state()}

    def bambu_restart(self):
        self.pending = None
        return {"bambu": self.bambu_state()}

    def bambu_signout(self):
        """Forgets the token (Bambu has no sign-out call for it); keeps the account and printers."""
        with self.save_lock:
            cfg = td_bambu.load_config(self.bambu_path)
            cfg.pop("token", None)
            cfg.pop("refresh", None)
            td_bambu.save_config(cfg, self.bambu_path)
            self.bambu.configure(cfg)
        self.pending = None
        return {"bambu": self.bambu_state()}

    def bambu_devices(self):
        cfg = td_bambu.load_config(self.bambu_path)
        if not cfg.get("token"):
            raise BadInput("Sign in first")
        try:
            return {"devices": td_bambu.devices(td_bambu.region_of(cfg), cfg["token"])}
        except td_bambu.CloudError as e:
            raise BadInput(str(e))

    def bambu_pick(self, printers):
        with self.save_lock:
            cfg = td_bambu.load_config(self.bambu_path)
            cfg["printers"] = [{"serial": p["serial"], "name": p["name"].strip() or p["serial"],
                                "model": p["model"].strip()} for p in printers]
            td_bambu.save_config(cfg, self.bambu_path)
            self.bambu.configure(cfg)
        return {"bambu": self.bambu_state()}

    def save(self, form):
        with self.save_lock:
            cfg = merge(td_work.load_config(self.path), form)
            td_work.save_config(cfg, self.path)
            if self.worker:
                self.worker.configure(cfg)
        return self.state()

    def test(self, form):
        cfg = merge(td_work.load_config(self.path), form)
        return {"results": summary(td_work.poll_once(cfg))}

    def write_session(self):
        try:
            os.makedirs(os.path.dirname(SESSION_FILE), mode=0o700, exist_ok=True)
            fd = os.open(SESSION_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(self.session)
        except OSError:
            pass

    def serve(self):
        """Starts uvicorn in a thread; False if the port is taken (another agent runs)."""
        if port_in_use(self.port):
            return False
        import uvicorn
        # Under pythonw (Windows, no console) there is no stdout/stderr and
        # uvicorn's log setup fails; leave logging alone then.
        quiet = {"log_config": None} if sys.stdout is None or sys.stderr is None else {}
        config = uvicorn.Config(create_app(self), host="127.0.0.1", port=self.port,
                                log_level="warning", access_log=False, server_header=False, **quiet)
        server = uvicorn.Server(config)
        threading.Thread(target=server.run, daemon=True, name="td_portal").start()
        self.write_session()
        return True
