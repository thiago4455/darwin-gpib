#!/bin/bash
# Registers gpibd as a per-user LaunchAgent.
#
# A LaunchAgent with a MachServices key is what makes the service reachable by
# ANY process in the session. An XPC service bundle inside the app
# (Contents/XPCServices/) would be private to that app and unreachable from
# python — that distinction is the whole design.
#
# No root: the driver's user client opens fine as a normal user, so this is a
# user agent, not a system daemon.
#
# Development-time installer. Ship path is SMAppService.agent from Open GPIB.
set -euo pipefail
cd "$(dirname "$0")"

LABEL="app.saturno.darwin-gpib.gpibd"
BIN="$(pwd)/gpibd"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

[ -x "$BIN" ] || { echo "build first: ./build.sh" >&2; exit 1; }

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
    </array>
    <!-- No RunAtLoad: MachServices alone gives on-demand launch. launchd holds
         the service name and starts gpibd on the first message. -->
    <key>MachServices</key>
    <dict>
        <key>$LABEL</key>
        <true/>
    </dict>
    <key>ProcessType</key>
    <string>Adaptive</string>
</dict>
</plist>
PLISTEOF

# Replace any previous registration.
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$PLIST"

echo "registered $LABEL"
echo "  plist:  $PLIST"
echo "  binary: $BIN"
launchctl print "gui/$UID/$LABEL" 2>/dev/null | grep -E "state|program|path" | head -5 || true
