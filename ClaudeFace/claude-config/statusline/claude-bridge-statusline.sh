#!/bin/sh
# claude-bridge-statusline.sh — forwards the statusline JSON to claude-bridge
# (fire-and-forget) and prints a compact terminal statusline. Target: <50ms.
#
# If the user already had a statusline command installed, install.sh saves it to
# ~/.config/claude-bridge/statusline-original and this script wraps it: forward to
# the bridge, then exec the ORIGINAL command with the same stdin and print ITS
# output instead of ours, so we never clobber what they had.

CONFIG_DIR="${CLAUDE_BRIDGE_CONFIG_DIR:-$HOME/.config/claude-bridge}"
ORIGINAL_FILE="$CONFIG_DIR/statusline-original"
port="${CLAUDE_BRIDGE_PORT:-7337}"
url="http://127.0.0.1:${port}/statusline"

body="$(cat)"

# Fire-and-forget to the bridge: fully detached background curl, 1s hard timeout,
# never lets a slow/dead bridge add latency to the statusline itself.
if command -v curl >/dev/null 2>&1; then
  ( printf '%s' "$body" | curl -s -m 1 -o /dev/null --data-binary @- -H 'Content-Type: application/json' "$url" >/dev/null 2>&1 & ) >/dev/null 2>&1
fi

# Wrap an existing statusline command, if install.sh recorded one.
if [ -s "$ORIGINAL_FILE" ]; then
  original_cmd="$(cat "$ORIGINAL_FILE")"
  if [ -n "$original_cmd" ]; then
    printf '%s' "$body" | eval "$original_cmd"
    exit $?
  fi
fi

# No wrapped command: print our own compact line.
if command -v jq >/dev/null 2>&1; then
  model="$(printf '%s' "$body" | jq -r '.model.display_name // "?"')"
  ctx="$(printf '%s' "$body" | jq -r 'if .context_window.used_percentage == null then "—" else (.context_window.used_percentage | floor | tostring) end')"
  five_used="$(printf '%s' "$body" | jq -r 'if .rate_limits.five_hour.used_percentage == null then null else .rate_limits.five_hour.used_percentage end')"
  five_resets="$(printf '%s' "$body" | jq -r '.rate_limits.five_hour.resets_at // empty')"
  seven_used="$(printf '%s' "$body" | jq -r 'if .rate_limits.seven_day.used_percentage == null then null else .rate_limits.seven_day.used_percentage end')"

  five_str="—"
  if [ -n "$five_used" ] && [ "$five_used" != "null" ]; then
    five_pct="$(printf '%s' "$five_used" | awk '{printf "%d", $1}')"
    remain=""
    if [ -n "$five_resets" ]; then
      now="$(date +%s)"
      secs=$((five_resets - now))
      if [ "$secs" -gt 0 ]; then
        h=$((secs / 3600))
        m=$(((secs % 3600) / 60))
        remain=" (${h}h${m})"
      fi
    fi
    five_str="${five_pct}%${remain}"
  fi

  seven_str="—"
  if [ -n "$seven_used" ] && [ "$seven_used" != "null" ]; then
    seven_str="$(printf '%s' "$seven_used" | awk '{printf "%d%%", $1}')"
  fi

  printf '%s · ctx %s%% · 5h %s · 7d %s\n' "$model" "$ctx" "$five_str" "$seven_str"
else
  # jq missing: degrade gracefully rather than fail. sed-based best-effort model name.
  model="$(printf '%s' "$body" | sed -n 's/.*"display_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
  if [ -n "$model" ]; then
    printf '%s\n' "$model"
  else
    printf 'claude-bridge\n'
  fi
fi
