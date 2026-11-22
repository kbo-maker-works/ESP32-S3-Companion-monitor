#!/bin/bash
# Installs the CompaDeck agent as a macOS service (LaunchAgent): it starts at
# login, restarts if it stops, and logs to ~/Library/Logs/TouchDeck/agent.log.
# Run it again after pulling changes: it rebuilds and restarts the agent.
#
#   macos/install.sh                         # deck at companion.local
#   macos/install.sh --host 192.168.1.225
#   macos/install.sh --no-build              # skip the settings page build (no Node)
set -euo pipefail

LABEL="com.touchdeck.agent"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs/TouchDeck"
HOST="${TOUCHDECK_HOST:-companion.local}"
BUILD=1

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

PY="$(command -v python3 || true)"
[ -n "$PY" ] || { echo "python3 not found (install it from python.org or Homebrew)" >&2; exit 1; }
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 9))' || { echo "python 3.9 or newer needed" >&2; exit 1; }

say "Python environment in $HERE/.venv"
[ -x "$HERE/.venv/bin/python" ] || "$PY" -m venv "$HERE/.venv"
"$HERE/.venv/bin/python" -m pip install -q --upgrade pip
"$HERE/.venv/bin/python" -m pip install -q -r "$HERE/requirements.txt"

if [ "$BUILD" = 1 ]; then
  if command -v npm >/dev/null; then
    say "Building the settings page"
    (cd "$HERE/portal" && npm install --no-audit --no-fund --silent && npm run -s build >/dev/null)
  else
    echo "npm not found: the settings page is not built (install Node, or use --no-build)" >&2
  fi
fi

say "LaunchAgent $PLIST (deck: $HOST)"
mkdir -p "$LOG_DIR" "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$HERE/.venv/bin/python</string>
    <string>$HERE/touchdeck_agent.py</string>
    <string>--host</string><string>$HOST</string>
  </array>
  <key>WorkingDirectory</key><string>$HERE</string>
  <key>EnvironmentVariables</key>
  <dict><key>PYTHONUNBUFFERED</key><string>1</string></dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$LOG_DIR/agent.log</string>
  <key>StandardErrorPath</key><string>$LOG_DIR/agent.log</string>
</dict>
</plist>
PLIST
plutil -lint "$PLIST" >/dev/null

# A fresh log per install, the previous one kept beside it.
[ -f "$LOG_DIR/agent.log" ] && mv "$LOG_DIR/agent.log" "$LOG_DIR/agent.log.1"

say "Starting the agent"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"

for _ in $(seq 1 20); do
  curl -s -o /dev/null -m 1 http://127.0.0.1:4213/ && break
  sleep 0.5
done
if curl -s -o /dev/null -m 1 http://127.0.0.1:4213/; then
  say "Running. Settings: http://127.0.0.1:4213"
else
  echo "The agent did not answer yet; see $LOG_DIR/agent.log" >&2
  tail -n 20 "$LOG_DIR/agent.log" 2>/dev/null >&2 || true
  exit 1
fi
