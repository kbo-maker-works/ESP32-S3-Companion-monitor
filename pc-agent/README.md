# CompaDeck PC agent

`touchdeck_agent.py` feeds the deck from this PC over WiFi (UDP port 4210):

- **PC metrics** for the metric tiles, one packet a second. The ids are stable,
  so a tile keeps its metric:

  | id | name     | unit | source |
  |----|----------|------|--------|
  | 1  | CPU      | %    | psutil |
  | 2  | RAM      | %    | psutil |
  | 3  | RAM USED | GB   | psutil |
  | 4  | GPU      | %    | nvidia-smi (NVIDIA only) |
  | 5  | GPUT     | C    | nvidia-smi |
  | 6  | VRAM     | %    | nvidia-smi |
  | 7  | GPUW     | W    | nvidia-smi |
  | 8  | DISK     | %    | system drive (macOS: the Data volume, as the Mac counts it) |
  | 9  | DOWN     | KB/s | all network cards |
  | 10 | UP       | KB/s | all network cards |
  | 11 | BATT     | %    | battery charge (laptops only) |
  | 12 | FPS      | FPS  | RTSS: the 3D app last in the foreground, 0 once it stops drawing |
  | 13 | FRAMETIME | ms  | RTSS |
  | 14 | GPU FAN  | %    | nvidia-smi |
  | 15 | GPU CLOCK | MHz | nvidia-smi |
  | 16 | CPUT     | C    | MSI Afterburner |
  | 17 | CPUW     | W    | MSI Afterburner |
  | 18 | VRAM USED | GB  | nvidia-smi |

  12, 13, 16 and 17 come from **RivaTuner Statistics Server** and **MSI
  Afterburner** (Windows, td_hwmon.py) while they run: both publish their
  readings in shared memory any process can read, so no admin and no extra
  package. Afterburner is the only way to the CPU's temperature here (Windows
  doesn't give it to a normal process); its sensor must be on in Afterburner's
  Monitoring tab. A metric whose source isn't running isn't sent.

  With these the packet can outgrow PACKET_MAX (a song playing, the deck
  paired): Claude's state then goes in a packet of its own, sent first, rather
  than the session list being cut.

  The **PC stats** widget shows the first four of CPU, RAM, GPU, DISK and
  BATT as rings (like macOS's Batteries widget) or as a list, from the tile's
  PC or else the first one online. Its *Batteries* variants show this PC's
  battery and its wireless accessories' instead (below).

- **Batteries** (td_batteries.py), every 30 s in `pcstate.batt`: this PC's
  battery and, on macOS, those of its connected accessories (keyboards, mice,
  trackpads, AirPods and their case, headsets, game controllers...), what a
  Mac's Batteries widget shows. macOS publishes them as power sources (IOKit,
  as `pmset -g accps` lists them): no permission needed. Only devices that
  report a battery to macOS appear, and only while connected (a sleeping
  mouse drops out). Windows: the PC's own battery only, for now.

- **Bambu printers through Bambu Cloud** (td_bambu.py, below): for the deck's
  printer pages and tiles, with the printer left in cloud mode.

- **Stock quotes** (td_stocks.py, below) for the deck's Stocks app: the symbols
  pinned on the settings page, from Yahoo Finance.

- **Claude Code's state** for the "Mascota Claude" widget: thinking, coding
  (with the tool it runs), several sessions at once, waiting for you (yellow),
  turn done (green), a failed tool (red), and asleep after 15 min of quiet.

## Setup

Any Python 3.9+ works. This PC has none installed system-wide, so the agent
uses its own environment built on PlatformIO's Python:

```bash
cd pc-agent
~/.platformio/penv/Scripts/python.exe -m venv .venv
.venv/Scripts/python.exe -m pip install psutil
.venv/Scripts/python.exe touchdeck_agent.py --once     # one packet, printed, not sent
.venv/Scripts/python.exe touchdeck_agent.py            # run (deck at companion.local)
```

`--host 192.168.1.225` (or `TOUCHDECK_HOST`) sets the deck's address. Without
it the agent resolves `companion.local` and falls back to a broadcast.

### Start and stop it on demand (Windows)

`windows\touchdeck.cmd` runs the agent in the background with no window
(pythonw) and logs to `%LOCALAPPDATA%\TouchDeck\agent.log`, which the settings
page shows under Logs. A double click starts it.

```bat
windows\touchdeck.cmd start --host 192.168.1.225
```

- `start`: starts it; nothing if it already runs.
- `stop`: stops it, however it was started (this script, a console, a shortcut).
- `restart`: after pulling changes.
- `status`: running or not, the settings page and the log's path.
- `settings`: opens the settings page, starting the agent first if needed.
- `shortcuts`: one shortcut per action (start, stop, restart, settings) in
  `windows\shortcuts\`, for launchers that pass no arguments, such as Stream
  Deck's Open: paste the `.lnk` path in its field (choosing it in the file
  dialog may swap it for the shortcut's target). No window shows when they run.
- `log`: follows the log; Ctrl+C to leave.
- `run`: runs it in this console instead (Ctrl+C stops), to see an error the
  log missed, such as a package that is not installed.

No action is `start`. Anything after the action goes to `touchdeck_agent.py`. Each start keeps the
previous log as `agent.log.1`. To start it at login too, put a shortcut to
`touchdeck.cmd` in the Startup folder (`shell:startup`), with Run set to
Minimized.

## Claude Code hooks (the mascot)

Claude Code runs the same script as `touchdeck_agent.py hook` on its hook
events; each call writes a small state file under `~/.claude/touchdeck/` and
exits at once (all but `PermissionRequest`, below). In `~/.claude/settings.json`, one entry per event
(`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`,
`Notification`, `Stop`, `SessionEnd`), all with the same command:

```json
{ "type": "command",
  "command": "C:/git-proyects/desktop-companion/pc-agent/.venv/Scripts/pythonw.exe C:/git-proyects/desktop-companion/pc-agent/touchdeck_agent.py hook" }
```

`pythonw.exe` keeps a console window from flashing on every hook.

### Permission prompts and questions on the deck (td_ask.py)

With one more entry, `PermissionRequest`, Claude Code's permission prompts
(Allow / Always / Deny) and its questions (AskUserQuestion, one at a time,
several options when it takes more than one) show up as a card on the deck. The
terminal shows its own dialog meanwhile, and whichever answers first wins. A
deny from the deck stops Claude, like the terminal's No.

The hook waits for the deck's answer, so it can't be `async` and it needs a long
timeout (the prompt stays on the deck for up to an hour):

```json
"PermissionRequest": [
  { "hooks": [
    { "type": "command",
      "command": "C:/git-proyects/desktop-companion/pc-agent/.venv/Scripts/pythonw.exe C:/git-proyects/desktop-companion/pc-agent/touchdeck_agent.py hook",
      "timeout": 3600 } ] } ]
```

It only waits while the agent runs and this PC is paired (`touchdeck_agent.py
pair`), since the deck answers with a signed command. Otherwise it steps aside
at once and only the terminal asks. When the terminal answers first, the hook
sees the tool's result in the transcript, or the session move on, and exits
without a decision. A prompt waiting for a tool to finish (one you allowed in the
terminal) can stay on the deck until the tool ends; a tap on it then does
nothing. The x on the card hides it until the next prompt.

## Bench app: sheets for the workbench (td_bench.py)

The soldering order, which wire goes where, a pinout: send it to the deck and
read it at the soldering station instead of on the phone or the PC. The deck
opens the Bench app by itself, and keeps the screen on while it is open.

```
python td_bench.py steps.txt           # text: one step per line
python td_bench.py pinout.png          # a picture
python td_bench.py --clip              # whatever is copied: a picture or text
python td_bench.py --text "Red -> 5V"  # text from the command line
some_command | python td_bench.py -    # text from stdin
python td_bench.py --sendto            # Windows: "CompaDeck Bench" in Explorer's Send to menu
```

- **Text**: one step per line. `# Title` lines are section headings, list
  markers (`- `, `1. `, `[ ] `) are dropped, colour words (red, black, rojo,
  negro...) get a swatch and `->` becomes an arrow. A Markdown table becomes
  one step per row (`Red -> 3V3`) under its header row. On the deck: tap the
  right half for the next step, the left half to go back. The current step
  survives a reboot.
- **Picture**: resized to at most 800 x 600 and sent as a JPEG (Pillow; a
  `.jpg` goes as it is without it). On the deck: tap to zoom 1:1 at that
  point, drag to pan, tap again to fit.
- The title is the file name, `--title`, or the text's first heading.

Like the Work app's data, the deck takes a sheet only from a PC whose agent
(`touchdeck_agent.py`) is running: same PC id, same address. Endpoints:
`POST /api/pc/bench` `{"pc_id", "title", "text"}` (up to 8 KB),
`POST /api/pc/bench/image?pc_id=&title=` (multipart `image`, a baseline JPEG up
to 400 KB) and `GET /api/pc/bench` (the current sheet).

### A plan on the bench, followed live (td_plan.py)

The checklist of a plan under a repo's `PLANS/` (a `## Progreso` or
`## Progress` section of `- [ ]` / `- [~]` / `- [x]` items, grouped by `###`),
on the deck instead of in a terminal pane. Pin it from the repo:

```
python touchdeck_agent.py plan              # the newest plan in this repo's PLANS/
python touchdeck_agent.py plan funding      # the newest one whose file name has "funding"
python touchdeck_agent.py plan off          # stop following it
```

The running agent checks the pin (`~/.touchdeck/plan.json`) and the plan's
mtime every 3 s and sends the checklist to the Bench app when it changes:
pinning opens the app, later updates redraw it in place. The sheet is
read-only and opens on the whole checklist: its groups, done steps ticked and
struck through, the one in progress (`[~]`) in amber, the ones left with an
empty ring, scrolled to where the plan is, and how many are done. A tap
switches to the current step alone (the first `[~]`, else the first `[ ]`,
large, with the one before and the next ones) and back; nothing moves a step. Whoever
implements the plan (any session, any LLM) ticks the file and the deck follows.
`POST /api/pc/bench` takes `"plan": true` and `"quiet": true` for this.

## Work app: GitLab and Jira (td_work.py)

The Work app on the deck has two tabs.

- **GitLab** works like GitLab's merge request dashboard: one foldable section
  per state, with its count. First the *active* ones: **Returned to you** (a
  reviewer sent your MR back), **Review requested** and **Your merge
  requests** (no reviewer yet). Then, not counted as active: **Waiting for
  author** (you reviewed), **Waiting for approvals**, **Approved by you** and
  **Approved by others**. Each MR shows its state as a chip (Requested, Ready,
  Conflict, CI, Draft...), its title, `project!iid`, lines added and removed,
  its pipeline, resolved threads and age. Last, the **Pipelines** of your MRs'
  branches and of `main` / `develop`...; failing ones also show as "1 pipeline
  KO" in the deck's top bar. The data comes from one GraphQL query (works on
  GitLab CE).
- **Jira**: one foldable section per filter (set up on the settings page, each
  a JQL with a label and a colour): its count and first 5 tickets.

**A tap on a row** (an MR, a pipeline, a ticket) opens its page in the
browser of the PC the data came from, like a Stream Deck key. The deck sends
a signed command `open:<id>` (the same channel as PC control, so the PC must
be paired once: `python3 touchdeck_agent.py pair`, then Allow on the deck);
the id names a page the agent fetched itself in its last two polls, so the
deck can never make the PC open an arbitrary URL. Works on macOS, Windows and
Linux (the agent listens on UDP 4212 for commands).

Two widgets put a section on a home tile, each with a variant per section:
**GitLab MRs** (Active, Review requested, Returned to you, Your merge
requests, Waiting for author / approvals, Approved by you / others,
Pipelines) and **Jira filter** (Filter 1 to 6, in the settings page's order).
The tile shows the count and, if it is big enough, the first titles; a tap
opens the Work app on that section.

The agent polls with your own tokens and **pushes** a summary to the deck
(`POST /api/pc/work`) when it changes, and once a minute anyway. Nothing
listens on the agent's machine, so no firewall prompt. The deck takes it only
from a PC that already sends it metrics, from that same address.

Set it up on the agent's **settings page**: while the agent runs, open
<http://127.0.0.1:4213> (or `python3 touchdeck_agent.py settings`). It has an
overview (deck, GitLab and Jira at a glance), a page per source (URL, token,
projects and branches, Jira columns and JQL) with a **Test connection** button
that polls with what is on the form without saving, and the agent's log. Save
applies at once, no restart.

- The page listens on 127.0.0.1 only: the tokens stay on this machine and no
  firewall prompt shows. `--portal-port 0` turns it off.
- A saved token is never shown again; leave the field empty to keep it.
  **Forget** removes it. Changing a source's URL drops its saved token, so it
  is never sent to a server it wasn't typed for.
- It only answers requests for `127.0.0.1:4213` / `localhost:4213` that carry
  the page's per-run session value, so other web pages can't read or change it.

### Branches tab: local branches, on demand

When `~/.claude/briefing/branches.py` exists (or `work.json` names another
script in `"branches": {"script": ...}`; `"enabled": false` turns it off), the
Work app gets a third tab with the local branches of the repos under `~/code`
as that script classifies them: Keep, Review and Delete, each branch with its
state (active, parked, blocked, stale, merged), repo, MR and reason. A tap on a
branch opens its MR here.

A scan runs git in every repo and asks GitLab for the MRs (~5-15 s), so it never
runs on a timer: the deck asks for one (signed `branches_scan`, so the PC must
be paired) when the tab opens with no scan or one older than 10 minutes, and on
its **Scan** button. The last scan keeps being pushed with the rest of the Work
data until the next one.

### The settings page (td_portal.py, portal/)

`td_portal.py` is a FastAPI app the agent runs in a thread (uvicorn). It serves
the React front end in `portal/` (Vite + TypeScript), built to `portal/dist`
(not in the repo):

```bash
cd pc-agent/portal
npm install
npm run build      # -> portal/dist, served by the agent
npm run dev        # http://127.0.0.1:5173 with hot reload, API from the running agent
```

API: `GET /api/state`, `PUT /api/config`, `POST /api/test`, `GET /api/logs`.
In dev mode the Vite proxy reads the session from `~/.touchdeck/portal.session`
(mode 600, written by the agent).

The page writes `~/.touchdeck/work.json` (mode 600, never in the repo), which
can also be edited by hand. Any section can be left out:

```json
{
  "gitlab": {
    "url": "https://gitlab.example.com",
    "token": "glpat-...",
    "projects": ["group/shop-api", "group/shop-web"],
    "branches": ["main", "master", "develop"]
  },
  "atlassian": {
    "url": "https://yourcompany.atlassian.net",
    "email": "you@company.com",
    "token": "..."
  },
  "jira": {
    "columns": [
      {"label": "TO DO",   "jql": "assignee = currentUser() AND status = \"To Do\"",       "color": "#E6E9EE"},
      {"label": "IN PROG", "jql": "assignee = currentUser() AND status = \"In Progress\"", "color": "#58A6FF"},
      {"label": "DONE",    "jql": "assignee = currentUser() AND status = Done AND updated >= -14d", "color": "#3FB950"}
    ],
    "active_jql": "assignee = currentUser() AND statusCategory = \"In Progress\" ORDER BY updated DESC"
  },
  "confluence": {"enabled": true}
}
```

- **GitLab** (gitlab.com or your own server): a personal access token with the
  `read_api` scope. `projects` is optional; the projects of your open MRs are
  always added. Polled every 60 s (`every_s`).
- **Atlassian**: one account (`atlassian`: site URL, email, token) for Jira
  and Confluence. **Cloud** (`*.atlassian.net`): `email` plus an API token from
  id.atlassian.com; a token without scopes covers both apps. A scoped token
  works too: after a 401 on the site the agent looks up the site's cloudId and
  retries through `api.atlassian.com/ex/<app>/<cloudId>`. **Server / Data
  Center**: leave `email` out and use a personal access token. Older configs
  with `url` / `email` / `token` inside `jira` still load; the settings page
  moves them to `atlassian` on the next save.
- **Jira**: up to 6 `columns`, each a JQL count with its label and
  color; without them: To Do / In Progress / Done (last 14 days) by status
  category. `active_jql` picks the tickets listed (up to 8). Polled every 120 s.
- **Confluence**: nothing on the deck uses it yet; with `enabled`, the settings
  page's Test also checks the account can read Confluence.
- Check the tokens without the deck: `python3 td_work.py --once` prints what it
  would send (or the HTTP error per source). `python3 td_work.py --sample --host
  <deck>` shows sample data on the deck.

## Bambu printers through Bambu Cloud (td_bambu.py)

For a printer kept in its normal cloud mode (a P2S, or any one you don't want in
LAN-only mode: Bambu Handy's notifications, the camera and remote printing keep
working). The agent signs in to Bambu Cloud, listens to the printers you pick on
Bambu's MQTT broker and pushes their state, already parsed, to the deck
(`POST /api/pc/printers`) when it changes and once a minute. The deck only shows
it; nothing is sent to the printer but status requests.

1. Agent settings page → **Printers**: region, email, password, then the code
   Bambu emails (or your authenticator app's). The password goes to Bambu only
   and is never stored; `~/.touchdeck/bambu.json` (mode 600) keeps the sign-in,
   about three months. Pick the printers (two at most).
2. Deck portal → Advanced → **Bambu Lab printers** on; Printers → Add printer →
   Connection **Bambu Cloud (through a PC agent)** → **Use this printer** →
   Create printer page → Save to device.

Several PCs can serve one printer (sign in on each): the deck keeps the one
already showing it until it goes quiet for 90 s or the other has a fresher
report. With every serving PC off, the printer shows "No news from <PC>".

Bambu's rules, kept here: its own User-Agent (Bambu's firewall blocks software
posing as Bambu Studio or OrcaSlicer since November 2026), a status request
("pushall") on connecting and then only while the printer is idle, every 5 min,
at most one every 2 min (a printing printer pushes by itself; requests every
few seconds get the broker session refused), and no control commands over the
cloud. When Bambu refuses the token, the agent stops retrying and both the
settings page and the deck say to sign in again. Needs `paho-mqtt`
(requirements.txt). `python td_bambu.py --sample --host <deck>` shows a sample
printer on the deck's portal picker.

## Stocks app (td_stocks.py)

Agent settings page → **Stocks**: search by name or symbol and **Pin** (twelve
at most, in the order shown, which the arrows change). The agent polls Yahoo
Finance's public API, no account or key, with only the symbols: the prices of
all of them in one request every 15 s while a market is open (`every_s` in
`~/.touchdeck/stocks.json`, 10 to 300) and every 2 minutes while all are
closed; each symbol's name, currency and trading hours every 30 minutes. It
pushes the rows (price, previous close, the day's line, the market's state) to
the deck (`POST /api/pc/stocks`) when they change. Yahoo gives some exchanges'
quotes with a delay, by its own rules. The deck's **Stocks** widget (deck
portal → a page → + Stocks) shows the watchlist or one pinned stock. On the deck, a tap on a row sends the signed
command `stock:<sym>:<range>`; the agent fetches that chart (1D, 5D, 1M, 6M, 1Y
or 5Y) and pushes it back (`POST /api/pc/stocks/chart`). An open market's 1D
chart is asked again every minute while it is on screen. The pins live in
`~/.touchdeck/stocks.json`. The PC must be paired for the charts (the rows come
without it).

Symbols are Yahoo's: `AAPL`, `SAN.MC` (Madrid), `^IBEX`, `BTC-USD`,
`EURUSD=X`. Without the deck: `python td_stocks.py --once SAN.MC AAPL`,
`python td_stocks.py --chart SAN.MC 1mo`, `python td_stocks.py --search iberdrola`.

## macOS

The agent runs on a Mac too: metrics (psutil, no GPU), Claude Code (if you add
the hooks there), the Work app, Now playing with its cover and buttons, and
the deck's commands once paired, Keep awake, and the volume (output level and mute, through
`osascript`; an output with no volume of its own, as some HDMI ones, shows none). The Mac and
the deck must be on the same network.

**As a service** (starts at login, restarts if it stops):

```bash
pc-agent/macos/install.sh                        # deck at companion.local
pc-agent/macos/install.sh --host 192.168.1.225
```

It creates `pc-agent/.venv`, installs `requirements.txt`, builds the settings
page (needs Node; `--no-build` skips it), writes the LaunchAgent
`~/Library/LaunchAgents/com.touchdeck.agent.plist` and starts it. Run it again
after a pull to update and restart. Logs: `~/Library/Logs/TouchDeck/agent.log`
(also on the settings page, Logs). `pc-agent/macos/uninstall.sh` stops and
removes the service, keeping the settings and the logs.

```bash
launchctl print gui/$(id -u)/com.touchdeck.agent | head    # state
launchctl kickstart -k gui/$(id -u)/com.touchdeck.agent     # restart
```

**By hand**, to try things:

```bash
cd pc-agent
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python td_work.py --once            # check ~/.touchdeck/work.json
.venv/bin/python touchdeck_agent.py --verbose # run; Ctrl+C to stop
```

A machine without the Claude Code hooks sends no Claude state, so a Mac
running only the Work part does not cover the mascot fed from another PC. If
both PCs run the hooks, the last one to send wins for the mascot and the AI
app.

## Open sessions (AI app)

The hooks keep each session's state in `~/.claude/touchdeck/sessions/`. The
agent sends up to 6 of them as `claude.ss`: `[name, state, seconds, detail]`
rows, state `i` (waiting for you: a permission or a question, red), `w`
(working, blue), `d` (turn done, green) or `z` (idle, grey). Waiting ones come
first. A "working" session with no hook for 30 min counts as idle, and one
untouched for 12 h drops off (a terminal closed without SessionEnd). Two
sessions in the same folder are numbered ("desktop-companion 2"). If the
packet would pass 1900 bytes the details go first, then rows.

## Plan usage (AI app, "AI usage" widget)

The 5-hour and 7-day windows and each session's context come from Claude
Code's own status line: on every update it pipes the status line command a
JSON payload that already holds the account's usage windows
(`rate_limits.five_hour` / `seven_day`) and the session's `context_window`.
`td_statusline.py` writes them to `~/.claude/touchdeck/usage/` (`limits.json`,
merged across sessions, and one file per session, removed by the SessionEnd
hook) and prints a normal status line (`Opus 5.5 | repo | ctx:12% | 5h:45%
wk:21%`). No password, keychain or API call. In `~/.claude/settings.json`:

```json
"statusLine": {"type": "command",
               "command": "/path/to/pc-agent/.venv/bin/python -I /path/to/pc-agent/td_statusline.py"}
```

**Several accounts on one computer** (switched with `/login`): the windows
are an account's, so `limits.json` keeps them per account, and the logged-in
one is read from Claude Code's config (`~/.claude.json`, `oauthAccount`, or
`$CLAUDE_CONFIG_DIR/.claude.json`; a plain file, no password). The AI app shows
that account (email and organization) over its quotas; an account with no
response yet shows `--` rather than the previous account's numbers.

The agent sends the logged-in account's windows in the `claude` object (`q5`,
`q5r`, `q7`, `q7r`, `acct`, `org`); not a session's context, which says little
with several sessions open. The windows only exist on a Pro/Max subscription, after a
session's first response. (Files written by the older `touchdeck-usage` mod
in the same folder are read too.)

### Antigravity's quota (td_agy_statusline.py)

The Antigravity CLI (`agy`) has the same kind of status line: it pipes the
command a JSON payload whose `quota` holds the real windows, `gemini-5h` and
`gemini-weekly` for Gemini models and `3p-5h` / `3p-weekly` for third-party
models (Claude, GPT), each with `remaining_fraction` and `reset_time`.
`td_agy_statusline.py` writes them to `~/.touchdeck/agy_usage.json` and prints a
status line (`Gemini 4 Hydrogen (High) | ctx:3% | 5h:11% wk:3%`). In
`~/.gemini/antigravity-cli/settings.json`:

```json
"statusLine": {"type": "command",
               "command": "/path/to/pc-agent/.venv/bin/python -I /path/to/pc-agent/td_agy_statusline.py"}
```

The agent sends them in the `claude` object as `g5`/`g5r`, `g7`/`g7r` (Gemini)
and `t5`/`t5r`, `t7`/`t7r` (third-party). The quota is the account's, so the
Antigravity app spends the same windows, but they only move here while `agy`
runs; a window whose reset has passed since then is shown at 0 %.

**Headless agy** (`agy -p`) never runs the status line. An interactive `agy`
hands it the quota a second or two after it starts, before any message, so
`td_agy.py` (started by the agent, macOS / Linux) opens `agy` for a moment in a
pseudo-terminal whenever a conversation is newer than the last reading (an
`agy -p`, the CLI or the Antigravity app; it waits until the conversation has
been quiet for 20 s), lets the status line save the quota and closes it. No
message is sent and no conversation is created. `python td_agy.py` does one
refresh by hand. On Windows only the interactive status line updates it.

The deck's **AI usage** widget picks what to track: Claude (both windows, or
5 hours or the week alone), Antigravity (the same, or its other models), or
Claude + Antigravity (four rows).

## Now playing, volume and PC control (td_pc.py)

- **Now playing**: whatever Windows' media controls show (YouTube Music's PWA
  in Chrome, Spotify, a browser tab): the session that plays, else the current
  one. Title, artist, progress and the cover go in `pcstate.media`.
- **Now playing on macOS** (td_media_mac.py): what Control Centre's Now
  Playing shows (Music, Spotify, YouTube Music, a tab in Chrome or Safari),
  with the cover and play/pause, next and previous. Since macOS 15.4 only
  Apple-signed programs may read it (MediaRemote), so `td_nowplaying_mac.py`
  runs under Apple's own python3 from the Command Line Tools
  (`xcode-select --install` if missing) and streams it to the agent; no
  password, no permission prompt, no extra packages (sips, part of macOS,
  resizes the cover).
- **Cover**: served on TCP 4211, `GET /art?id=<crc32>&s=<px>`, raw RGB565.
- **Volume**: master volume and mute in `pcstate` (Windows: pycaw; macOS: `osascript`).
- **Commands** (the tile's buttons, PC volume): UDP 4212, signed with the key
  from pairing. Only media, volume, Keep awake and the Work app's links run;
  lock, sleep, restart, shutdown and hotkeys are refused.

- **Keep awake** (td_awake.py): a USB mouse jiggler in software. While on,
  the computer neither sleeps nor locks from idleness (macOS: `caffeinate -d
  -i`, tied to the agent; Windows: SetThreadExecutionState). With *Stay
  active* (on by default), after 60 s without mouse or keyboard the pointer
  moves 1 px and back, so Slack or Teams keep you active; never while you use
  the computer. Turn it on from the deck (a PC command tile, command *Keep
  awake*: the tile is lit while it is on) or the settings page, Overview. It
  survives restarts (`~/.touchdeck/awake.json`). On macOS *Stay active* needs
  the Accessibility permission for the agent's Python (System Settings >
  Privacy & Security > Accessibility; the settings page shows which binary
  and can ask macOS for it); it then covers any script that Python runs.
  Without it, only the no-sleep part works.

Pair once (tap *Allow* on the deck): `.venv/Scripts/python.exe touchdeck_agent.py pair`
(macOS: `.venv/bin/python touchdeck_agent.py pair`).
The key is kept in `~/.touchdeck/agent.json`.

The deck connects to the PC for the cover and the commands. On a Mac with the
firewall on, allow incoming connections for the agent's Python when macOS asks.
Windows' firewall needs two inbound rules (admin PowerShell):

```powershell
New-NetFirewallRule -DisplayName "TouchDeck agent: cover (TCP 4211)" -Direction Inbound -Protocol TCP -LocalPort 4211 -RemoteAddress LocalSubnet -Action Allow -Profile Any
New-NetFirewallRule -DisplayName "TouchDeck agent: commands (UDP 4212)" -Direction Inbound -Protocol UDP -LocalPort 4212 -RemoteAddress LocalSubnet -Action Allow -Profile Any
```

## Old files

`companion_agent.py` and `claude_statusline.py` belong to the ESP32-C3
companion (plain JSON over serial); the TouchDeck firmware ignores them.
