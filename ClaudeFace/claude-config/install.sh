#!/bin/sh
# install.sh — installs claude-bridge's hooks + statusline into Claude Code's
# settings.json. Safe to re-run (idempotent). Never touches anything but the
# settings.json chosen below.
#
# Location choice (documented): scripts are COPIED to ~/.claude/claude-bridge/
# rather than referenced in place, so a later `git pull`/move of this repo
# checkout doesn't silently break an installed hook path. Re-running install.sh
# re-copies (overwrites) them, so edits here always propagate on the next install.
#
# Env overrides:
#   SETTINGS   - path to settings.json (default: ~/.claude/settings.json).
#                Used by the test suite to operate on a temp copy — NEVER point
#                this at the real file during automated testing.
#   INSTALL_DIR - where to copy scripts (default: ~/.claude/claude-bridge)
#   CLAUDE_BRIDGE_CONFIG_DIR - where the saved-original-statusline file lives
#                (default: ~/.config/claude-bridge)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SETTINGS="${SETTINGS:-$HOME/.claude/settings.json}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.claude/claude-bridge}"
CONFIG_DIR="${CLAUDE_BRIDGE_CONFIG_DIR:-$HOME/.config/claude-bridge}"

mkdir -p "$INSTALL_DIR" "$CONFIG_DIR"
cp "$SCRIPT_DIR/hooks/claude-bridge-hook.sh" "$INSTALL_DIR/claude-bridge-hook.sh"
cp "$SCRIPT_DIR/statusline/claude-bridge-statusline.sh" "$INSTALL_DIR/claude-bridge-statusline.sh"
chmod +x "$INSTALL_DIR/claude-bridge-hook.sh" "$INSTALL_DIR/claude-bridge-statusline.sh"

HOOK_SCRIPT="$INSTALL_DIR/claude-bridge-hook.sh" \
STATUSLINE_SCRIPT="$INSTALL_DIR/claude-bridge-statusline.sh" \
ORIGINAL_STATUSLINE_FILE="$CONFIG_DIR/statusline-original" \
SETTINGS="$SETTINGS" \
  node "$SCRIPT_DIR/lib/do-install.cjs"

echo "claude-bridge: installed hooks + statusline into $SETTINGS"
