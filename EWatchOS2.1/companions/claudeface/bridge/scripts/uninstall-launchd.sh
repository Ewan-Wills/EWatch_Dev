#!/bin/sh
# uninstall-launchd.sh — unloads and removes the claude-bridge launchd agent.
# NOT run as part of building claude-bridge; run it yourself when you want to
# stop autostarting the bridge:  sh bridge/scripts/uninstall-launchd.sh

set -eu

PLIST="$HOME/Library/LaunchAgents/com.ewills.claude-bridge.plist"

if [ -f "$PLIST" ]; then
  launchctl unload "$PLIST" 2>/dev/null || true
  rm -f "$PLIST"
  echo "claude-bridge: unloaded and removed $PLIST"
else
  echo "claude-bridge: $PLIST not present, nothing to do"
fi
