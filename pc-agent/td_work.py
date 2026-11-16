#!/usr/bin/env python3
"""
Work data for the deck's Work app: GitLab (your merge requests, reviews,
to-dos, pipelines) and Jira (column counts, tickets in progress), polled with
your own tokens and pushed to the deck when something changes:

    POST http://<deck>/api/pc/work   {"pc_id", "pc_name", "gitlab": {...}, "jira": {...}}

Standard library only, so it runs the same on macOS, Linux and Windows.
touchdeck_agent.py starts it when ~/.touchdeck/work.json exists.

~/.touchdeck/work.json (keep it out of the repo, it holds your tokens):

{
  "gitlab": {
    "url": "https://gitlab.example.com",
    "token": "glpat-...",                      # a personal token with read_api
    "projects": ["group/shop-api"],            # optional: the projects of your open MRs are added
    "branches": ["main", "master", "develop"], # their latest pipeline is always shown
    "every_s": 60
  },
  "atlassian": {                               # one account for Jira and Confluence
    "url": "https://yourcompany.atlassian.net",
    "email": "you@company.com",                # Cloud: email + API token (scoped tokens work too)
    "token": "..."                             # Server / Data Center: no email, a personal access token
  },
  "jira": {
    "columns": [                               # the filters (optional, see DEFAULT_COLUMNS): each one
      {"label": "TO DO", "jql": "assignee = currentUser() AND statusCategory = \\"To Do\\"", "color": "#E6E9EE"}
    ],                                         # is a section on the deck with its count and tickets
    "every_s": 120
  },
  "confluence": {"enabled": true}              # only checked for now (Test on the settings page)
}

Test your tokens without the deck:   python td_work.py --once
Show sample data on the deck:        python td_work.py --sample --host 192.168.1.225
"""
import argparse
import base64
import datetime
import json
import os
import platform
import hashlib
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "work.json")
PUSH_EVERY_S = 60     # also when nothing changed, so the deck knows the data is current
TIMEOUT_S = 20

# Deck field sizes in bytes, terminator included (src/services/work_data.h).
MAX_NAME, MAX_REF, MAX_INFO, MAX_TITLE, MAX_TEXT, MAX_KEY, MAX_LABEL, MAX_ERR = 19, 39, 15, 63, 15, 15, 11, 39
MAX_MR_REF, MAX_CHIP, MAX_AGE = 31, 11, 5
MAX_PIPELINES, MAX_MRS, MAX_SECTION_MRS, MAX_COLS, MAX_TICKETS, MAX_COL_TICKETS = 8, 16, 6, 6, 24, 5

DEFAULT_COLUMNS = [
    {"label": "TO DO", "jql": 'assignee = currentUser() AND statusCategory = "To Do" AND resolution = Unresolved',
     "color": "#E6E9EE"},
    {"label": "IN PROG", "jql": 'assignee = currentUser() AND statusCategory = "In Progress"', "color": "#58A6FF"},
    {"label": "DONE", "jql": "assignee = currentUser() AND statusCategory = Done AND updated >= -14d",
     "color": "#3FB950"},
]


def clip(text, max_bytes):
    b = str(text or "").encode("utf-8")[:max_bytes]
    return b.decode("utf-8", "ignore")


def ago(iso):
    """'2026-11-15T09:02:11.123Z' -> '6m', '3h', '2d'."""
    try:
        t = datetime.datetime.fromisoformat((iso or "").replace("Z", "+00:00")).timestamp()
    except ValueError:
        return ""
    s = max(0, int(time.time() - t))
    return "now" if s < 60 else "%dm" % (s // 60) if s < 3600 else "%dh" % (s // 3600) if s < 86400 else "%dd" % (s // 86400)


def http_error(e):
    if isinstance(e, urllib.error.HTTPError):
        if e.code in (401, 403):
            return "HTTP 403: check email, token, scopes" if e.code == 403 else "HTTP 401: check email and token"
        return "HTTP %d" % e.code
    if isinstance(e, urllib.error.URLError):
        return clip("no connection: %s" % e.reason, MAX_ERR)
    return clip(str(e) or e.__class__.__name__, MAX_ERR)


def link(links, url):
    """A short id for a row's web page, kept in links: the deck sends "open:<id>"
    back and the agent opens only pages it fetched itself (never a URL from the deck)."""
    if not url or not str(url).startswith(("https://", "http://")):
        return ""
    lid = hashlib.sha1(str(url).encode()).hexdigest()[:8]
    links[lid] = str(url)
    return lid


def open_url(url):
    """Opens a page in the default browser of the user's session."""
    if sys.platform == "darwin":
        subprocess.Popen(["open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif os.name == "nt":
        os.startfile(url)  # noqa: the only way that never shows a console
    else:
        subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class _KeepTokensHome(urllib.request.HTTPRedirectHandler):
    """urllib copies every header to a redirect's target, the token too: to
    another host, or from https to http. There the tokens stay behind."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is None:
            return None
        a, b = urllib.parse.urlsplit(req.full_url), urllib.parse.urlsplit(newurl)
        if (a.scheme, a.netloc.lower()) != (b.scheme, b.netloc.lower()):
            for h in list(new.headers):
                if h.lower() in ("authorization", "private-token"):
                    del new.headers[h]
        return new


_OPENER = urllib.request.build_opener(_KeepTokensHome)


class Api:
    def __init__(self, base, headers):
        self.base, self.headers = base.rstrip("/"), headers

    def call(self, path, params=None, body=None):
        url = self.base + path + ("?" + urllib.parse.urlencode(params, doseq=True) if params else "")
        data = json.dumps(body).encode() if body is not None else None
        headers = dict(self.headers, Accept="application/json")
        if data is not None:
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method="POST" if data else "GET")
        with _OPENER.open(req, timeout=TIMEOUT_S) as r:
            return json.loads(r.read().decode() or "null"), r.headers


# ---------------------------------------------------------------------------
# GitLab
# ---------------------------------------------------------------------------
PIPE_STATUS = {"success": "s", "failed": "f", "running": "r", "pending": "p", "created": "p",
               "waiting_for_resource": "p", "preparing": "p", "scheduled": "p", "canceled": "c",
               "skipped": "c", "manual": "m"}
PIPE_ORDER = {"f": 0, "r": 1, "p": 2, "m": 3, "s": 4, "c": 5}


# GitLab's merge request dashboard, section by section (the deck's Work app and
# its tiles keep this order: deck work_data.h WORK_SEC_*). The first three are
# the "active" ones; the rest wait on someone else.
SECTIONS = ("returned", "review", "mine", "author", "approvals", "by_me", "by_others")
SEC = {name: i for i, name in enumerate(SECTIONS)}
ACTIVE_SECTIONS = 3

MR_QUERY = """
fragment mr on MergeRequest {
  iid title draft updatedAt detailedMergeStatus sourceBranch webUrl
  project { path fullPath }
  diffStatsSummary { additions deletions }
  headPipeline { status }
  resolvableDiscussionsCount resolvedDiscussionsCount
  reviewers { nodes { username mergeRequestInteraction { reviewState approved } } }
  approvedBy { nodes { username } }
}
query {
  currentUser {
    username
    authored: authoredMergeRequests(state: opened, first: 20, sort: UPDATED_DESC) { nodes { ...mr } }
    assigned: assignedMergeRequests(state: opened, first: 20, sort: UPDATED_DESC) { nodes { ...mr } }
    review: reviewRequestedMergeRequests(state: opened, first: 20, sort: UPDATED_DESC) { nodes { ...mr } }
  }
}"""

GQL_PIPE = {"SUCCESS": "s", "FAILED": "f", "RUNNING": "r", "PENDING": "p", "CREATED": "p",
            "WAITING_FOR_RESOURCE": "p", "PREPARING": "p", "SCHEDULED": "p", "WAITING_FOR_CALLBACK": "p",
            "CANCELED": "c", "CANCELING": "c", "SKIPPED": "c", "MANUAL": "m"}

# detailedMergeStatus -> the row's chip
MERGE_CHIP = {"MERGEABLE": ("Ready", "o"), "CONFLICT": ("Conflict", "b"), "NEED_REBASE": ("Rebase", "b"),
              "CI_MUST_PASS": ("CI", "i"), "CI_STILL_RUNNING": ("CI", "i"),
              "DISCUSSIONS_NOT_RESOLVED": ("Threads", "w"), "NOT_APPROVED": ("Approval", "d"),
              "APPROVALS_SYNCING": ("Approval", "d"), "BLOCKED_STATUS": ("Blocked", "w"),
              "EXTERNAL_STATUS_CHECKS": ("Checks", "w"), "JIRA_ASSOCIATION_MISSING": ("Jira key", "w"),
              "NOT_OPEN": ("Closed", "d"), "DRAFT_STATUS": ("Draft", "d")}


class GitLab:
    def __init__(self, cfg):
        url = cfg["url"].rstrip("/")
        self.api = Api(url + "/api/v4", {"PRIVATE-TOKEN": cfg["token"]})
        self.gql = Api(url + "/api", {"PRIVATE-TOKEN": cfg["token"]})
        self.projects = list(cfg.get("projects") or [])
        self.branches = set(cfg.get("branches") or ["main", "master", "develop"])
        self.every = max(30, int(cfg.get("every_s") or 60))

    def total(self, path, **params):
        body, headers = self.api.call(path, dict(params, per_page=1))
        total = headers.get("X-Total")
        return int(total) if total and total.isdigit() else len(body or [])

    def merge_requests(self):
        body = self.gql.call("/graphql", body={"query": MR_QUERY})[0] or {}
        if body.get("errors"):
            raise ValueError(clip("GraphQL: %s" % body["errors"][0].get("message"), MAX_ERR))
        me = (body.get("data") or {}).get("currentUser")
        if not me:
            raise ValueError("GraphQL: no current user")
        return me

    @staticmethod
    def review_state(m, user):
        for r in (m.get("reviewers") or {}).get("nodes") or []:
            if r.get("username") == user:
                i = r.get("mergeRequestInteraction") or {}
                return "APPROVED" if i.get("approved") else (i.get("reviewState") or "UNREVIEWED")
        return "UNREVIEWED"

    @staticmethod
    def section_of_mine(m):
        """One of your MRs (authored or assigned): where it stands with its reviewers."""
        states = [((r.get("mergeRequestInteraction") or {}).get("reviewState") or "UNREVIEWED",
                   bool((r.get("mergeRequestInteraction") or {}).get("approved")))
                  for r in (m.get("reviewers") or {}).get("nodes") or []]
        if any(st in ("REQUESTED_CHANGES", "REVIEWED") and not ok for st, ok in states):
            return SEC["returned"]
        if any(ok for _, ok in states) or (m.get("approvedBy") or {}).get("nodes"):
            return SEC["by_others"]
        if states:
            return SEC["approvals"]
        return SEC["mine"]

    @staticmethod
    def chip(m, sec, my_state):
        if m.get("draft"):
            return "Draft", "d"
        if sec == SEC["review"] and my_state in ("UNREVIEWED", "REVIEW_STARTED"):
            return "Requested", "w"
        if sec == SEC["returned"]:
            return "Changes", "w"
        status = m.get("detailedMergeStatus") or ""
        if status in ("UNCHECKED", "CHECKING", "PREPARING", ""):  # GitLab has not worked it out yet
            return ("Approval", "d") if sec in (SEC["approvals"], SEC["by_others"]) else ("Open", "d")
        return MERGE_CHIP.get(status, ("Open", "d"))

    def mr_row(self, m, sec, links, my_state=None):
        text, tone = self.chip(m, sec, my_state)
        ds = m.get("diffStatsSummary") or {}
        pipe = GQL_PIPE.get(((m.get("headPipeline") or {}).get("status") or "").upper(), "")
        return [sec, clip("%s!%s" % ((m.get("project") or {}).get("path") or "?", m.get("iid")), MAX_MR_REF),
                clip(m.get("title"), MAX_TITLE), clip(text, MAX_CHIP), tone,
                int(ds.get("additions") or 0), int(ds.get("deletions") or 0), pipe,
                int(m.get("resolvedDiscussionsCount") or 0), int(m.get("resolvableDiscussionsCount") or 0),
                clip(ago(m.get("updatedAt")), MAX_AGE), link(links, m.get("webUrl")), m.get("updatedAt") or ""]

    def poll(self):
        me = self.merge_requests()
        user = me.get("username")
        rows, mine, links = [], {}, {}
        for m in ((me.get("authored") or {}).get("nodes") or []) + ((me.get("assigned") or {}).get("nodes") or []):
            mine.setdefault("%s!%s" % ((m.get("project") or {}).get("fullPath"), m.get("iid")), m)
        for m in mine.values():
            rows.append(self.mr_row(m, self.section_of_mine(m), links))
        for m in (me.get("review") or {}).get("nodes") or []:
            st = self.review_state(m, user)
            sec = SEC["by_me"] if st == "APPROVED" else SEC["author"] if st in ("REQUESTED_CHANGES", "REVIEWED") \
                else SEC["review"]
            rows.append(self.mr_row(m, sec, links, st))
        counts = [0] * len(SECTIONS)
        for r in rows:
            counts[r[0]] += 1
        # Newest first within each section; at most MAX_SECTION_MRS per section and
        # MAX_MRS in all, the active sections first (the counts stay exact).
        rows.sort(key=lambda r: (r[0], -self.stamp(r[12])))
        sent, per = [], [0] * len(SECTIONS)
        for sec in list(range(ACTIVE_SECTIONS)) + list(range(ACTIVE_SECTIONS, len(SECTIONS))):
            for r in rows:
                if r[0] == sec and per[sec] < MAX_SECTION_MRS and len(sent) < MAX_MRS:
                    per[sec] += 1
                    sent.append(r[:12])
        sent.sort(key=lambda r: r[0])
        out = {"sec": counts, "active": sum(counts[:ACTIVE_SECTIONS]), "mrs": sent,
               "todos": self.total("/todos", state="pending")}
        out["pipelines"] = self.pipelines(list(mine.values()), links)
        out["_links"] = links
        out["ko"] = sum(1 for p in out["pipelines"] if p[2] == "f")
        return out

    def pipelines(self, mine, links):
        """Latest pipeline per interesting ref: your MRs' branches and the main ones."""
        projects = {}  # full path -> (short name, refs wanted)
        for p in self.projects:
            projects.setdefault(p, [p.rsplit("/", 1)[-1], set()])
        for m in mine:
            pr = m.get("project") or {}
            if pr.get("fullPath"):
                projects.setdefault(pr["fullPath"], [pr.get("path") or pr["fullPath"], set()])[1].add(m.get("sourceBranch"))
        rows = []
        for key, (name, refs) in list(projects.items())[:8]:
            pid = urllib.parse.quote(str(key), safe="")
            try:
                pls = self.api.call("/projects/%s/pipelines" % pid,
                                    {"per_page": 30, "order_by": "updated_at", "sort": "desc"})[0]
            except urllib.error.HTTPError:
                continue
            seen = set()
            for pl in pls:
                ref = pl.get("ref") or ""
                if ref in seen or (ref not in refs and ref not in self.branches):
                    continue
                seen.add(ref)
                st = PIPE_STATUS.get(pl.get("status"), "c")
                rows.append([clip(name, MAX_NAME), clip(ref, MAX_REF), st, self.pipe_info(pid, pl, st),
                             link(links, pl.get("web_url")), pl.get("updated_at") or ""])
        rows.sort(key=lambda r: (PIPE_ORDER[r[2]], -self.stamp(r[5])))
        return [r[:5] for r in rows[:MAX_PIPELINES]]

    @staticmethod
    def stamp(iso):
        try:
            return datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()
        except ValueError:
            return 0

    def pipe_info(self, pid, pl, st):
        try:
            if st == "f":
                jobs = self.api.call("/projects/%s/pipelines/%d/jobs" % (pid, pl["id"]),
                                     {"scope[]": "failed", "per_page": 1})[0]
                return clip(jobs[0]["name"] if jobs else "failed", MAX_INFO)
            if st in ("r", "p"):
                jobs = self.api.call("/projects/%s/pipelines/%d/jobs" % (pid, pl["id"]), {"per_page": 100})[0]
                done = sum(1 for j in jobs if j.get("status") in ("success", "failed", "skipped", "canceled"))
                return "%d/%d" % (done, len(jobs))
        except (urllib.error.URLError, OSError, ValueError, KeyError, IndexError):
            pass
        return ago(pl.get("updated_at")) if st == "s" else {"m": "manual", "c": "canceled"}.get(st, "")


# ---------------------------------------------------------------------------
# Jira
# ---------------------------------------------------------------------------
TONE_BY_CATEGORY = {"new": "dim", "indeterminate": "info", "done": "ok"}


def atlassian_site(cfg):
    """The Atlassian account shared by Jira and Confluence: {"url", "email", "token"}.
    Older configs kept them in the jira section."""
    cfg = cfg or {}
    site = dict(cfg.get("atlassian") or {})
    old = cfg.get("jira") or {}
    for k in ("url", "email", "token"):
        if not site.get(k) and old.get(k):
            site[k] = old[k]
    return site


class AtlassianApi(Api):
    """Api for an Atlassian Cloud site that also takes scoped API tokens: those
    only work through api.atlassian.com/ex/<product>/<cloudId>, so after a 401
    on the site the cloudId is looked up (public) and the call is retried there."""

    def __init__(self, site, product):
        url, token = (site.get("url") or "").rstrip("/"), site["token"]
        self.site, self.product, self.cloud = url, product, bool(site.get("email"))
        if self.cloud:
            auth = "Basic " + base64.b64encode(("%s:%s" % (site["email"], token)).encode()).decode()
        else:  # Server / Data Center: a personal access token
            auth = "Bearer " + token
        super().__init__(url, {"Authorization": auth})
        self.via_gateway = False

    def cloud_id(self):
        info = urllib.request.urlopen(self.site + "/_edge/tenant_info", timeout=TIMEOUT_S).read()
        return json.loads(info.decode())["cloudId"]

    def call(self, path, params=None, body=None):
        try:
            return super().call(path, params, body)
        except urllib.error.HTTPError as e:
            if e.code != 401 or self.via_gateway or not self.cloud or "atlassian.net" not in self.site:
                raise
            try:
                self.base = "https://api.atlassian.com/ex/%s/%s" % (self.product, self.cloud_id())
            except (urllib.error.URLError, OSError, ValueError, KeyError):
                raise e
            self.via_gateway = True
            return super().call(path, params, body)


class Jira:
    def __init__(self, cfg, site):
        self.api = AtlassianApi(site, "jira")
        self.cloud = self.api.cloud
        self.columns = cfg.get("columns") or DEFAULT_COLUMNS
        self.every = max(60, int(cfg.get("every_s") or 120))
        self.me = None

    def count(self, jql):
        if self.cloud:
            return int(self.api.call("/rest/api/3/search/approximate-count", body={"jql": jql})[0].get("count") or 0)
        return int(self.api.call("/rest/api/2/search", {"jql": jql, "maxResults": 0})[0].get("total") or 0)

    def search(self, jql, n):
        params = {"jql": jql, "maxResults": n, "fields": "summary,status"}
        path = "/rest/api/3/search/jql" if self.cloud else "/rest/api/2/search"
        return self.api.call(path, params)[0].get("issues") or []

    def poll(self):
        # Jira answers searches with bad credentials as an anonymous user: the
        # counts come back 0 with no error. Asking who we are fails loudly (401).
        if not self.me:
            self.me = self.api.call("/rest/api/3/myself" if self.cloud else "/rest/api/2/myself")[0]
        # One section per filter: its count, its colour and its first tickets.
        cols, sent, links = [], 0, {}
        for c in self.columns[:MAX_COLS]:
            tickets = []
            room = min(MAX_COL_TICKETS, MAX_TICKETS - sent)
            for issue in self.search(c["jql"], room) if room > 0 else []:
                f = issue.get("fields") or {}
                status = f.get("status") or {}
                tone = TONE_BY_CATEGORY.get((status.get("statusCategory") or {}).get("key"), "dim")
                url = "%s/browse/%s" % (self.api.site, issue.get("key")) if self.api.site else ""
                tickets.append([clip(issue.get("key"), MAX_KEY), clip(f.get("summary"), MAX_TITLE),
                                clip((status.get("name") or "").upper(), MAX_TEXT), tone, link(links, url)])
            sent += len(tickets)
            cols.append([clip(c.get("label"), MAX_LABEL), self.count(c["jql"]), c.get("color") or "#E6E9EE", tickets])
        return {"cols": cols, "_links": links}


# ---------------------------------------------------------------------------
# Polling and pushing
# ---------------------------------------------------------------------------
def load_config(path=CONFIG_FILE):
    """The settings, None when missing or unreadable: a broken file must not
    take the settings page (which can write a good one) down with it."""
    try:
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else None
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        global _load_error
        if str(e) != _load_error:  # the settings page reads it every few seconds
            print("work: can't read %s: %s" % (path, e), flush=True)
            _load_error = str(e)
        return None


_load_error = None


def save_config(cfg, path=CONFIG_FILE):
    """Writes work.json readable by this user only, replacing it in one step."""
    folder = os.path.dirname(path)
    os.makedirs(folder, mode=0o700, exist_ok=True)
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)
    try:
        os.chmod(path, 0o600)
    except OSError:  # Windows: the user's profile folder already keeps it private
        pass


def check_confluence(cfg):
    """Confluence is only checked (nothing on the deck uses it yet): who the token is."""
    api = AtlassianApi(atlassian_site(cfg), "confluence")
    me = api.call("/wiki/rest/api/user/current")[0] or {}
    if me.get("type") == "anonymous":
        raise ValueError("anonymous: check email and token")
    return {"user": clip(me.get("displayName") or me.get("publicName") or "?", MAX_TITLE),
            "gateway": api.via_gateway}


def make_sources(cfg):
    """The sources a config enables: GitLab needs its url and token, Jira the
    Atlassian account's url and token."""
    cfg, sources = cfg or {}, {}
    g = cfg.get("gitlab") or {}
    if g.get("enabled", True) and g.get("url") and g.get("token"):
        sources["gitlab"] = GitLab(g)
    j, site = cfg.get("jira") or {}, atlassian_site(cfg)
    if j.get("enabled", True) and site.get("url") and site.get("token"):
        sources["jira"] = Jira(j, site)
    return sources


def poll_once(cfg):
    """{source: data or {"err"}} for each enabled source, polled now (the portal's Test).
    Confluence, when enabled with a token, is checked too."""
    out = {}
    for name, src in make_sources(cfg).items():
        try:
            out[name] = src.poll()
            out[name].pop("_links", None)
            if name == "jira":
                out[name]["gateway"] = src.api.via_gateway
        except Exception as e:
            out[name] = {"err": http_error(e)}
    c = (cfg or {}).get("confluence") or {}
    site = atlassian_site(cfg)
    if c.get("enabled", False) and site.get("url") and site.get("token"):
        try:
            out["confluence"] = check_confluence(cfg)
        except Exception as e:
            out["confluence"] = {"err": http_error(e)}
    return out


class Worker(threading.Thread):
    """Polls each source on its own period and pushes to the deck on a change.
    configure() swaps the sources while it runs (the portal saves a new config)."""

    def __init__(self, cfg, deck_addr, pc_id, pc_name, verbose=False):
        super().__init__(daemon=True, name="td_work")
        self.deck_addr, self.pc_id, self.pc_name, self.verbose = deck_addr, pc_id, pc_name, verbose
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.pushed, self.pushed_at = None, 0.0
        self.last_push = {"ok": None, "at": 0.0, "addr": ""}
        self.had_sources = False
        self.links = {}  # source -> (links of the last poll, of the one before)
        self.configure(cfg)

    def configure(self, cfg):
        sources = make_sources(cfg)
        with self.lock:
            self.sources = sources
            self.state = {name: {"err": "connecting..."} for name in sources}
            self.due = {name: 0.0 for name in sources}
            self.polled = {}  # name -> wall time of the last good poll
            self.pushed = None
            self.had_sources = self.had_sources or bool(sources)
        self.wake.set()

    def run(self):
        failing = None
        while True:
            try:
                self._tick()
            except Exception as e:  # one bad poll or push must not stop the Work updates for good
                if str(e) != failing:
                    print("work: %s (logged once until it changes)" % e, flush=True)
                    failing = str(e)
            else:
                failing = None
            self.wake.wait(5)
            self.wake.clear()

    def _tick(self):
        with self.lock:
            todo = [(n, s) for n, s in self.sources.items() if time.monotonic() >= self.due[n]]
            for name, src in todo:
                self.due[name] = time.monotonic() + src.every
        for name, src in todo:
            try:
                data, err = src.poll(), None
            except Exception as e:  # keep the last data, say what failed
                data, err = None, e
                if self.verbose:
                    print("work: %s: %s" % (name, e), flush=True)
            with self.lock:
                if self.sources.get(name) is not src:  # reconfigured meanwhile
                    continue
                if err is None:
                    new = data.pop("_links", {})
                    self.links[name] = (new, self.links.get(name, ({}, {}))[0])  # a tap may race a poll
                    self.state[name] = data
                    self.polled[name] = time.time()
                else:
                    self.state[name] = dict(self.state[name], err=http_error(err))
        self.push()

    def open_link(self, lid):
        """The deck's "open:<id>": opens that MR / pipeline / ticket page here."""
        if not re.fullmatch(r"[0-9a-f]{8}", lid or ""):
            return False, "bad link"
        with self.lock:
            url = next((d[lid] for pair in self.links.values() for d in pair if lid in d), None)
        if not url:
            return False, "link expired"
        try:
            open_url(url)
        except OSError as e:
            return False, clip(str(e), 60)
        return True, ""

    def payload(self):
        with self.lock:
            return dict({"pc_id": self.pc_id, "pc_name": self.pc_name}, **self.state)

    def status(self):
        """For the portal: per source when it last answered and its error, and the last push."""
        with self.lock:
            src = {name: {"polled": self.polled.get(name, 0.0), "err": st.get("err", "")}
                   for name, st in self.state.items()}
            return {"sources": src, "push": dict(self.last_push)}

    def push(self):
        # No sources ever: nothing to say. Sources removed: one push clears the deck.
        if not self.had_sources:
            return
        body = json.dumps(self.payload(), separators=(",", ":"), ensure_ascii=False)
        if body == self.pushed and time.monotonic() - self.pushed_at < PUSH_EVERY_S:
            return
        addr = self.deck_addr()
        if addr == "255.255.255.255":  # the deck's name doesn't resolve yet: nowhere to push
            return
        ok = push(addr, body, self.verbose)
        with self.lock:
            self.last_push = {"ok": ok, "at": time.time(), "addr": addr}
            if ok:
                self.pushed, self.pushed_at = body, time.monotonic()


def push(addr, body, verbose=False):
    req = urllib.request.Request("http://%s/api/pc/work" % addr, data=body.encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
        if verbose:
            print("work: pushed %d B to %s" % (len(body), addr), flush=True)
        return True
    except (urllib.error.URLError, OSError) as e:
        if verbose:
            print("work: push to %s failed: %s" % (addr, e), flush=True)
        return False


SAMPLE = {
    "gitlab": {
        "sec": [1, 2, 3, 0, 1, 1, 1], "active": 6, "todos": 1, "ko": 1,
        "mrs": [
            [0, "shop-web!17", "Checkout page redesign", "Changes", "w", 412, 96, "s", 2, 5, "3h"],
            [1, "shop-core!56", "Retry failed payments", "Requested", "w", 1815, 122, "f", 4, 4, "3d"],
            [1, "shop-api!21", "Order confirmation email", "Ready", "o", 23, 0, "s", 1, 1, "3d"],
            [2, "shop-api!60", "Rate limit on the login endpoint", "Ready", "o", 375, 3, "s", 0, 0, "2d"],
            [2, "shop-api!30", "Funding report notifications", "Conflict", "b", 237, 10, "s", 0, 0, "2d"],
            [2, "shop-lib!29", "Shared config: new fields", "CI", "i", 14, 2, "r", 0, 0, "2d"],
            [4, "shop-web!8", "Dark mode for the account page", "Approval", "d", 88, 40, "s", 0, 0, "5d"],
            [5, "shop-lib!27", "Bump the HTTP client", "Ready", "o", 2, 0, "s", 0, 0, "1d"],
            [6, "shop-core!5", "Applied conditions persistence fix", "Approval", "d", 232, 48, "f", 0, 1, "2d"]],
        "pipelines": [["shop-core", "feature/PROJ-101-login-limit", "f", "unit-tests"],
                      ["shop-web", "develop", "r", "2/5"],
                      ["shop-api", "main", "s", "6m"]],
    },
    "jira": {
        "cols": [["TO DO", 7, "#E6E9EE", [["PROJ-120", "Export invoices as CSV", "TO DO", "dim"],
                                          ["PROJ-118", "Search by order number", "TO DO", "dim"]]],
                 ["IN PROG", 2, "#58A6FF", [["PROJ-101", "Rate limit on the login endpoint", "IN PROGRESS", "info"],
                                            ["PROJ-96", "Checkout page redesign", "IN PROGRESS", "info"]]],
                 ["REVIEW", 1, "#FF8A84", [["PROJ-110", "Retry failed payments", "CODE REVIEW", "info"]]],
                 ["DONE", 4, "#3FB950", [["PROJ-90", "Order confirmation email", "DONE", "ok"]]]],
    },
}


def main():
    ap = argparse.ArgumentParser(description="TouchDeck work data (GitLab, Jira)")
    ap.add_argument("--once", action="store_true", help="poll once with ~/.touchdeck/work.json and print it")
    ap.add_argument("--sample", action="store_true", help="push sample data to the deck")
    ap.add_argument("--host", default=os.environ.get("TOUCHDECK_HOST", "companion.local"))
    args = ap.parse_args()
    host = platform.node() or "PC"
    pc_id, pc_name = hashlib.sha1(host.encode()).hexdigest()[:12], clip(host, 23)
    if args.sample:
        body = json.dumps(dict({"pc_id": pc_id, "pc_name": pc_name}, **SAMPLE), separators=(",", ":"))
        print("pushed" if push(args.host, body, True) else "push failed")
        return
    cfg = load_config()
    if not cfg:
        raise SystemExit("no %s (see the top of td_work.py)" % CONFIG_FILE)
    print(json.dumps(dict({"pc_id": pc_id, "pc_name": pc_name}, **poll_once(cfg)), indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
