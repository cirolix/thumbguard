#!/bin/bash
# Removes thumbguard completely. The log file is left in place.

set -uo pipefail

LABEL="local.thumbguard"
BIN="$HOME/.local/bin/thumbguard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

echo "==> Stopping the service"
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true

echo "==> Removing files"
rm -f "$PLIST" "$BIN"

echo
echo "Removed. The log file is still at:"
echo "  $HOME/Library/Logs/thumbguard.log"
