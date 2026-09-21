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

# Values end up inside the plist, so XML special characters must be escaped.
xml_escape() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

echo "==> Building"
cc -O2 -Wall -Wextra -o "$SRC_DIR/thumbguard" "$SRC_DIR/thumbguard.c"

# Reject typos now - otherwise the service would keep restarting in vain.
"$SRC_DIR/thumbguard" ${EXTRA_ARGS+"${EXTRA_ARGS[@]}"} --version >/dev/null

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
        <string>$(xml_escape "$BIN")</string>
        <string>$(xml_escape "--log=$LOG")</string>
EOF
    for a in ${EXTRA_ARGS+"${EXTRA_ARGS[@]}"}; do
        printf '        <string>%s</string>\n' "$(xml_escape "$a")"
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
# "launchctl print" alone also succeeds for a job that keeps crashing, so look
# at its state line. Captured first: grep -q in a pipe would trip pipefail.
STATE="$(launchctl print "gui/$UID/$LABEL" 2>/dev/null || true)"
if grep -q $'^\tstate = running' <<<"$STATE"; then
    echo "Running. The service will now start automatically at every login."
else
    echo "The service failed to start - please check '$LOG'." >&2
    exit 1
fi

echo
echo "  Show state:  $BIN --status"
echo "  Log:         tail -f $LOG"
echo "  Remove:      $SRC_DIR/uninstall.sh"
