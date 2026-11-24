"""
CompaDeck agent: Claude Code's permission prompts and questions on the deck.

Claude Code runs `touchdeck_agent.py hook` on PermissionRequest, a hook that
blocks (README.md), while the terminal shows its own dialog. The hook writes the
prompt to ~/.claude/touchdeck/prompts/<id>.json and waits. The running agent
sends it to the deck, and the deck's tap comes back as the signed command
"claude:<id>:<qi>:<choice>". The agent writes it to <id>.answer.json, and the
hook turns that into Claude Code's decision.

Whichever answers first wins. When the terminal does, the tool's result shows up
in the transcript (or the session moves on), and the hook steps aside without a
decision. AskUserQuestion goes through PermissionRequest too, so its questions
come the same way, one at a time.
"""
import hashlib
import json
import os
import re
import secrets
import sys
import threading
import time

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "touchdeck")
PROMPTS_DIR = os.path.join(STATE_DIR, "prompts")
ALIVE_FILE = os.path.join(PROMPTS_DIR, "agent.alive")

WAIT_MAX_S = 3500   # under the hook's timeout (3600 in settings.json)
POLL_S = 0.25
BEAT_S = 3          # the hook touches its prompt file this often...
LIVE_S = 12         # ...so a prompt untouched this long has lost its hook
ALIVE_EVERY_S = 5   # the agent's heartbeat, while the deck can answer
ALIVE_S = 15        # older: no agent, the hook steps aside at once
TAIL = 256 * 1024   # transcript bytes read when the hook starts

# The deck's field sizes (bytes incl. the terminator): ClaudeAsk in pc_metrics.h.
MAX_SESSION, MAX_TITLE, MAX_TEXT, MAX_DESC, MAX_HINT, MAX_OPT, MAX_OPT_DESC = 23, 31, 239, 99, 63, 47, 79
MAX_OPTS = 4

DENY_MESSAGE = "The user denied this on the CompaDeck."


def clip(text, max_bytes):
    b = (text or "").encode("utf-8")[:max_bytes]
    return b.decode("utf-8", "ignore")


def one_line(text):
    return re.sub(r"\s+", " ", text or "").strip()


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def remove_quietly(path):
    try:
        os.remove(path)
    except OSError:
        pass


def prompt_path(pid):
    return os.path.join(PROMPTS_DIR, "%s.json" % pid)


def answer_path(pid):
    return os.path.join(PROMPTS_DIR, "%s.answer.json" % pid)


# ---------------------------------------------------------------------------
# What the deck shows
# ---------------------------------------------------------------------------
def tool_title(name):
    """'Bash'; 'mcp__github__create_issue' -> 'create_issue (github)'."""
    if name.startswith("mcp__"):
        parts = name.split("__")
        return "%s (%s)" % (parts[-1], parts[1]) if len(parts) >= 3 else name[5:]
    return name


def short_path(path):
    parts = [p for p in re.split(r"[\\/]+", path or "") if p]
    return "/".join(parts[-3:])


def tool_text(name, ti):
    """(what it runs or touches, Claude's own description of it)."""
    if name in ("Bash", "PowerShell"):
        return ti.get("command") or "", ti.get("description") or ""
    if name in ("Edit", "Write", "Read", "NotebookEdit", "MultiEdit"):
        return short_path(ti.get("file_path") or ti.get("notebook_path")), ""
    if name == "WebFetch":
        return ti.get("url") or "", ""
    if name == "WebSearch":
        return ti.get("query") or "", ""
    if name in ("Agent", "Task"):
        return ti.get("description") or ti.get("prompt") or "", ti.get("subagent_type") or ""
    if name == "Skill":
        return ti.get("skill") or ti.get("name") or "", ""
    return json.dumps(ti, ensure_ascii=False, separators=(",", ":")), ""


def suggestions_for(payload):
    """What "Always" saves. Claude Code sends no suggestions for MCP tools, yet the
    terminal offers "Always allow" for them, which allows that one tool in the
    project's local settings: the same rule here. Other tools without suggestions
    get no "Always" (a whole-tool rule for Bash would allow every command)."""
    s = payload.get("permission_suggestions")
    if isinstance(s, list) and s:
        return s
    name = payload.get("tool_name") or ""
    if name.startswith("mcp__"):
        return [{"type": "addRules", "rules": [{"toolName": name}], "behavior": "allow",
                 "destination": "localSettings"}]
    return []


def always_hint(suggestions):
    """What 'Always' does, as the terminal words its option."""
    where = {"session": "this session", "localSettings": "this project", "projectSettings": "this project",
             "userSettings": "every project"}
    for s in suggestions:
        if not isinstance(s, dict):
            continue
        kind, dest = s.get("type"), where.get(s.get("destination"), "")
        if kind == "setMode":
            mode = {"acceptEdits": "accept all edits", "bypassPermissions": "skip all prompts",
                    "plan": "plan mode"}.get(s.get("mode"), "mode %s" % s.get("mode"))
            return "Always: %s%s" % (mode, " in " + dest if dest else "")
        if kind in ("addRules", "replaceRules"):
            rules = []
            for r in s.get("rules") or []:
                if isinstance(r, dict):
                    tool, content = r.get("toolName") or "", r.get("ruleContent") or ""
                    if tool.startswith("mcp__") and not content:  # "addOrEditJiraIssueComment (atlassian)"
                        tool = tool_title(tool)
                    rules.append("%s(%s)" % (tool, content) if content else tool)
            if rules:
                return "Always: %s%s" % (", ".join(rules), " in " + dest if dest else "")
        if kind == "addDirectories":
            dirs = [short_path(d) for d in s.get("directories") or []]
            if dirs:
                return "Always: access %s" % ", ".join(dirs)
    return "Always: don't ask again for this"


def call_sig(name, tool_input):
    """The tool call, for PostToolUse to find its prompt (a Write's whole file
    stays out of the prompt file)."""
    blob = json.dumps([name, tool_input], sort_keys=True, ensure_ascii=False)
    return hashlib.sha1(blob.encode("utf-8")).hexdigest()[:16]


def build_prompt(payload, session):
    """The prompt file's content, None for what the deck can't answer."""
    name = payload.get("tool_name") or ""
    ti = payload.get("tool_input") if isinstance(payload.get("tool_input"), dict) else {}
    base = {"sid": payload.get("session_id") or "", "t": time.time(), "sig": call_sig(name, ti),
            "session": clip(session or "Claude Code", MAX_SESSION)}
    if name == "AskUserQuestion":
        questions = []
        for q in ti.get("questions") or []:
            if not isinstance(q, dict) or not q.get("question"):
                continue
            opts = [o for o in q.get("options") or [] if isinstance(o, dict) and o.get("label")]
            if not opts or len(opts) > MAX_OPTS:
                return None  # more options than the deck shows: the terminal only
            questions.append({"q": q["question"], "h": q.get("header") or "", "m": bool(q.get("multiSelect")),
                              "o": [o["label"] for o in opts], "d": [o.get("description") or "" for o in opts]})
        if not questions:
            return None
        return dict(base, kind="q", questions=questions)
    suggestions = suggestions_for(payload)
    text, desc = tool_text(name, ti)
    # Shown left to right; the last one is the deck's accent button.
    actions = ["deny", "always", "allow"] if suggestions else ["deny", "allow"]
    labels = {"deny": "Deny", "always": "Always", "allow": "Allow"}
    return dict(base, kind="p", title=clip(tool_title(name), MAX_TITLE), text=clip(text, MAX_TEXT),
                desc=clip(one_line(desc), MAX_DESC), hint=clip(always_hint(suggestions), MAX_HINT) if suggestions else "",
                actions=actions, labels=[labels[a] for a in actions])


def deck_ask(pid, p, qi):
    """The "ask" object for the deck (pc_metrics.h)."""
    out = {"id": pid, "s": p.get("session") or "", "k": p.get("kind")}
    if p.get("kind") == "p":
        out.update(t=p.get("title") or "", x=p.get("text") or "", o=p.get("labels") or [])
        if p.get("desc"):
            out["d"] = p["desc"]
        if p.get("hint"):
            out["h"] = p["hint"]
        return out
    qs = p.get("questions") or []
    q = qs[qi]
    out.update(t=clip(one_line(q["h"]), MAX_TITLE), x=clip(q["q"], MAX_TEXT),
               o=[clip(one_line(o), MAX_OPT) for o in q["o"]], qi=qi, qn=len(qs))
    descs = [clip(one_line(d), MAX_OPT_DESC) for d in q.get("d") or []]
    if any(descs):
        out["od"] = descs
    if q.get("m"):
        out["m"] = True
    return out


# ---------------------------------------------------------------------------
# The hook: wait for the deck, or step aside when the terminal answers first
# ---------------------------------------------------------------------------
class TranscriptWatch:
    """Sees the tool's result appear in the transcript: the terminal answered
    (allowed and the tool ran, denied, or Esc). The tool call is found by its
    name and input; earlier identical calls already have their result."""

    def __init__(self, path, name, tool_input):
        self.path, self.name, self.input = path, name, tool_input
        self.pending, self.done = set(), set()
        self.pos, self.rest = 0, b""
        try:
            size = os.path.getsize(path)
        except (OSError, TypeError):
            self.path = None
            return
        self.pos = max(0, size - TAIL)
        self._read(skip_first=self.pos > 0)
        self.pending -= self.done

    def _read(self, skip_first=False):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return False
        self.pos += len(data)
        lines = (self.rest + data).split(b"\n")
        self.rest = lines.pop()  # a line still being written
        if skip_first and lines:
            lines = lines[1:]  # cut in the middle of a line
        hit = False
        for line in lines:
            if b'"tool_use"' in line and self.name.encode() in line:
                for block in self._blocks(line):
                    if (block.get("type") == "tool_use" and block.get("name") == self.name
                            and block.get("input") == self.input and block.get("id")):
                        self.pending.add(block["id"])
            if b'"tool_result"' in line:
                for block in self._blocks(line):
                    if block.get("type") == "tool_result":
                        tid = block.get("tool_use_id")
                        self.done.add(tid)
                        hit = hit or tid in self.pending
        return hit

    @staticmethod
    def _blocks(line):
        try:
            entry = json.loads(line)
        except ValueError:
            return []
        content = ((entry.get("message") or {}) if isinstance(entry, dict) else {}).get("content")
        return [b for b in content if isinstance(b, dict)] if isinstance(content, list) else []

    def resolved(self):
        return bool(self.path) and self._read()


def decision_for(answer, payload):
    if isinstance(answer.get("answers"), dict):
        ti = dict(payload.get("tool_input") or {})
        ti["answers"] = answer["answers"]
        return {"behavior": "allow", "updatedInput": ti}
    action = answer.get("action")
    if action == "allow":
        return {"behavior": "allow"}
    if action == "always":
        return {"behavior": "allow", "updatedPermissions": suggestions_for(payload)}
    if action == "deny":
        # As the terminal's No: Claude stops and waits for you.
        return {"behavior": "deny", "message": DENY_MESSAGE, "interrupt": True}
    return None


def agent_alive():
    try:
        return time.time() - os.path.getmtime(ALIVE_FILE) < ALIVE_S
    except OSError:
        return False


def run_permission_hook(payload, session):
    """Blocks until the deck answers (prints Claude Code's decision) or the
    terminal does (prints nothing)."""
    decision = wait_decision(payload, session)
    if decision:
        out = json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}})
        if sys.stdout is not None:
            sys.stdout.write(out)
            sys.stdout.flush()
        else:  # pythonw without a stdout object: the pipe is still there
            os.write(1, out.encode())


def wait_decision(payload, session):
    """The deck's answer as Claude Code's decision; None when the terminal
    answered first, or nothing can show the prompt."""
    if not agent_alive():
        return None  # no agent sending to a deck: the terminal only
    p = build_prompt(payload, session)
    if not p:
        return None
    pid = secrets.token_hex(4)
    path, ans_path = prompt_path(pid), answer_path(pid)
    # The subagent's own transcript, when Claude Code names it.
    transcript = payload.get("agent_transcript_path") or payload.get("transcript_path")
    watch = TranscriptWatch(transcript, payload.get("tool_name") or "", payload.get("tool_input"))
    write_json_atomic(path, p)
    decision = None
    try:
        start = beat = time.time()
        while time.time() - start < WAIT_MAX_S:
            answer = read_json(ans_path)
            if isinstance(answer, dict):
                decision = decision_for(answer, payload)
                break
            if not os.path.exists(path):  # the session moved on (another hook removed it)
                break
            if watch.resolved():
                break
            if time.time() - beat >= BEAT_S:
                beat = time.time()
                try:
                    os.utime(path)
                except OSError:
                    break
            time.sleep(POLL_S)
    finally:
        remove_quietly(path)
        remove_quietly(ans_path)
    return decision


def dismiss(sid, tool_name=None, tool_input=None):
    """Another hook of the session says the prompt is over: its tool ran (that
    tool only), or the turn ended or a new one started (all of the session's)."""
    try:
        names = os.listdir(PROMPTS_DIR)
    except OSError:
        return
    for name in names:
        if not re.fullmatch(r"[0-9a-f]{8}\.json", name):
            continue
        path = os.path.join(PROMPTS_DIR, name)
        p = read_json(path)
        if not isinstance(p, dict) or p.get("sid") != sid:
            continue
        if tool_name is not None and p.get("sig") != call_sig(tool_name, tool_input):
            continue
        remove_quietly(path)


# ---------------------------------------------------------------------------
# The agent: send the oldest prompt, take the deck's answers
# ---------------------------------------------------------------------------
class Asks:
    def __init__(self):
        self.lock = threading.Lock()
        self.partial = {}  # prompt id -> the answers so far, one per question
        self.beat_at = 0.0
        self.swept_at = 0.0

    def beat(self):
        """The main loop, while the deck can answer: hooks may wait for it."""
        now = time.time()
        if now - self.beat_at < ALIVE_EVERY_S:
            return
        self.beat_at = now
        try:
            os.makedirs(PROMPTS_DIR, exist_ok=True)
            with open(ALIVE_FILE, "w") as f:
                f.write("%d" % now)
        except OSError:
            pass

    def _live(self):
        """[(t, id, prompt)] of the prompts whose hook still waits, oldest first."""
        out = []
        now = time.time()
        try:
            names = os.listdir(PROMPTS_DIR)
        except OSError:
            return out
        sweep = now - self.swept_at > 60
        if sweep:
            self.swept_at = now
        for name in names:
            path = os.path.join(PROMPTS_DIR, name)
            m = re.fullmatch(r"([0-9a-f]{8})\.json", name)
            try:
                age = now - os.path.getmtime(path)
            except OSError:
                continue
            if not m:
                if sweep and age > 600 and (name.endswith(".answer.json") or name.endswith(".tmp")):
                    remove_quietly(path)  # left by a hook that was killed
                continue
            if age > LIVE_S:
                if sweep and age > 600:
                    remove_quietly(path)
                continue
            p = read_json(path)
            if isinstance(p, dict) and p.get("kind") in ("p", "q"):
                out.append((float(p.get("t") or 0), m.group(1), p))
        out.sort(key=lambda x: x[0])
        with self.lock:
            ids = {pid for _, pid, _ in out}
            for pid in list(self.partial):
                if pid not in ids:
                    del self.partial[pid]
        return out

    def current(self):
        """The "ask" object for the deck, None when no session waits."""
        live = self._live()
        if not live:
            return None
        _, pid, p = live[0]
        with self.lock:
            qi = len(self.partial.get(pid, []))
        if p["kind"] == "q" and qi >= len(p.get("questions") or []):
            return None  # all answered: the hook takes it now
        return deck_ask(pid, p, qi)

    def command(self, rest):
        """Deck command "claude:<id>:<qi>:<choice>": an option's index, or
        several joined by "," for a question that takes more than one."""
        m = re.fullmatch(r"([0-9a-f]{8}):(\d{1,2}):(\d(?:,\d){0,%d})" % (MAX_OPTS - 1), rest)
        if not m:
            return False, "bad answer"
        pid, qi, picks = m.group(1), int(m.group(2)), [int(x) for x in m.group(3).split(",")]
        path = prompt_path(pid)
        p = read_json(path)
        try:
            stale = time.time() - os.path.getmtime(path) > LIVE_S
        except OSError:
            stale = True
        if not isinstance(p, dict) or stale:
            return False, "Claude no longer asks"
        if len(set(picks)) != len(picks):
            return False, "bad answer"
        if p.get("kind") == "p":
            actions = p.get("actions") or []
            if qi or len(picks) != 1 or picks[0] >= len(actions):
                return False, "bad answer"
            write_json_atomic(answer_path(pid), {"action": actions[picks[0]]})
            return True, ""
        qs = p.get("questions") or []
        with self.lock:
            done = self.partial.setdefault(pid, [])
            if qi != len(done) or qi >= len(qs):
                return False, "already answered"
            q = qs[qi]
            if any(i >= len(q["o"]) for i in picks) or (len(picks) > 1 and not q.get("m")):
                return False, "bad answer"
            done.append(", ".join(q["o"][i] for i in sorted(picks)))
            if len(done) < len(qs):
                return True, ""
            answers = {qs[i]["q"]: done[i] for i in range(len(qs))}
        write_json_atomic(answer_path(pid), {"answers": answers})
        return True, ""
