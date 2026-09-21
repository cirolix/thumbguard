#!/bin/bash
# Builds thumbguard, installs it as a background service and starts it.
#
#   ./install.sh              normal operation (thumbnails keep working)
#   ./install.sh --block      block mode (no HTML/Office thumbnails at all)
#
# Any other thumbguard option is passed through unchanged, e.g.
#   ./install.sh --cpu=70 --strikes=5

set -euo pipefail

LABEL="local.thumbguard"
BIN_DIR="$HOME/.local/bin"
BIN="$BIN_DIR/thumbguard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/thumbguard.log"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

EXTRA_ARGS=("$@")

echo "==> Building"
cc -O2 -Wall -Wextra -o "$SRC_DIR/thumbguard" "$SRC_DIR/thumbguard.c"

echo "==> Installing to $BIN"
mkdir -p "$BIN_DIR" "$(dirname "$LOG")"
# Unload a running copy first, otherwise the file cannot be replaced.
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
install -m 755 "$SRC_DIR/thumbguard" "$BIN"

echo "==> Setting up the service"
{
    cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>--log=$LOG</string>
EOF
    for a in ${EXTRA_ARGS+"${EXTRA_ARGS[@]}"}; do
        printf '        <string>%s</string>\n' "$a"
    done
    cat <<EOF
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>LowPriorityIO</key>
    <true/>
</dict>
</plist>
EOF
} > "$PLIST"

launchctl bootstrap "gui/$UID" "$PLIST"

sleep 1
echo
if launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1; then
    echo "Running. The service will now start automatically at every login."
else
    echo "The service failed to load - please check '$LOG'." >&2
    exit 1
fi

echo
echo "  Show state:  $BIN --status"
echo "  Log:         tail -f $LOG"
echo "  Remove:      $SRC_DIR/uninstall.sh"
