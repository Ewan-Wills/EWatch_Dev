#!/bin/sh
# uninstall.sh — removes only claude-bridge's own hook entries and restores the
# original statusLine command (or removes the key if there wasn't one before).
# Does NOT touch any other hooks/settings the user has.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SETTINGS="${SETTINGS:-$HOME/.claude/settings.json}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.claude/claude-bridge}"
CONFIG_DIR="${CLAUDE_BRIDGE_CONFIG_DIR:-$HOME/.config/claude-bridge}"

SETTINGS="$SETTINGS" \
ORIGINAL_STATUSLINE_FILE="$CONFIG_DIR/statusline-original" \
  node "$SCRIPT_DIR/lib/do-uninstall.cjs"

rm -f "$CONFIG_DIR/statusline-original"
rm -rf "$INSTALL_DIR"

echo "claude-bridge: removed hooks + statusline from $SETTINGS"
