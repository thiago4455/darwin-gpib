#!/bin/bash
set -euo pipefail
LABEL="app.saturno.darwin-gpib.gpibd"
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$LABEL.plist"
echo "unregistered $LABEL"
