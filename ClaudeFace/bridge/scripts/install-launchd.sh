#!/bin/sh
# install-launchd.sh — writes ~/Library/LaunchAgents/com.ewills.claude-bridge.plist
# from the template, with the absolute node + cli.js paths filled in, then loads
# it with launchctl.
#
# IMPORTANT: this script is written for use but is NOT run as part of building
# claude-bridge. Run it yourself, deliberately, when you're ready to autostart
# the bridge:  sh bridge/scripts/install-launchd.sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEMPLATE="$BRIDGE_DIR/launchd/com.ewills.claude-bridge.plist"
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST="$PLIST_DIR/com.ewills.claude-bridge.plist"

NODE_PATH="$(command -v node || true)"
if [ -z "$NODE_PATH" ]; then
  echo "install-launchd.sh: node not found on PATH" >&2
  exit 1
fi
CLI_PATH="$BRIDGE_DIR/dist/src/cli.js"
if [ ! -f "$CLI_PATH" ]; then
  echo "install-launchd.sh: $CLI_PATH not built yet — run 'npm run build' in bridge/ first" >&2
  exit 1
fi

mkdir -p "$PLIST_DIR" "$HOME/Library/Logs"

sed -e "s#@NODE_PATH@#$NODE_PATH#g" \
    -e "s#@CLI_PATH@#$CLI_PATH#g" \
    -e "s#@HOME_DIR@#$HOME#g" \
    "$TEMPLATE" > "$PLIST"

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"

echo "claude-bridge: installed and loaded $PLIST"
echo "  node: $NODE_PATH"
echo "  cli:  $CLI_PATH"
echo "  logs: $HOME/Library/Logs/claude-bridge.log"
