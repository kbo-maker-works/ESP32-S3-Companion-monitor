#!/bin/bash
# Stops the CompaDeck agent service and removes its LaunchAgent. Keeps the
# settings (~/.touchdeck), the logs (~/Library/Logs/TouchDeck) and .venv.
set -euo pipefail
LABEL="com.touchdeck.agent"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null && echo "stopped" || echo "was not running"
rm -f "$PLIST" && echo "removed $PLIST"
