#!/bin/sh
# claude-bridge-hook.sh — forwards a Claude Code hook payload (stdin JSON) to the
# claude-bridge daemon. Must be fast, silent, and never break the hook pipeline.
#
# Contract (see ClaudeFace/docs/SPEC.md and bridge/README.md):
#   - reads the hook JSON from stdin
#   - POSTs it to http://127.0.0.1:${CLAUDE_BRIDGE_PORT:-7337}/hook with a <=1s timeout
#   - prints NOTHING to stdout or stderr
#   - always exits 0, even if the bridge is down or curl is missing
#   - client-side privacy: strips the `prompt` field (UserPromptSubmit) and the
#     `tool_response` field (PostToolUse) before the payload ever leaves this
#     process, so prompt text/tool output never touch the loopback socket at all.
#     (The bridge does its OWN privacy filtering server-side for everything else —
#     e.g. truncating Bash commands and stripping paths — this script only removes
#     the two fields that are pure free text with no protocol use.)
#
# jq decision (documented per SPEC instructions):
#   If `jq` is available, use it to surgically delete `.prompt` and `.tool_response`
#   and forward the rest of the JSON unchanged (all fields the bridge needs —
#   hook_event_name, session_id, tool_name, tool_input, notification_type, error,
#   workspace, etc. — survive).
#   If `jq` is NOT available, we do NOT attempt to hand-roll JSON field deletion in
#   POSIX sh/sed (too fragile/unsafe on arbitrary JSON string content). Instead we
#   fall back to sending a minimal, always-safe payload containing only
#   hook_event_name and session_id (extracted with sed against the raw stdin).
#   This loses tool-name/current-tool detail on a jq-less box, but never risks
#   leaking prompt text or shipping malformed JSON. Document this as an assumption.

port="${CLAUDE_BRIDGE_PORT:-7337}"
url="http://127.0.0.1:${port}/hook"

body="$(cat)"

if command -v jq >/dev/null 2>&1; then
  payload="$(printf '%s' "$body" | jq -c 'del(.prompt, .tool_response)' 2>/dev/null)"
  if [ -z "$payload" ]; then
    # jq failed to parse (unexpected input shape) — do nothing rather than guess.
    exit 0
  fi
else
  event="$(printf '%s' "$body" | sed -n 's/.*"hook_event_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
  session="$(printf '%s' "$body" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
  if [ -z "$event" ] || [ -z "$session" ]; then
    exit 0
  fi
  payload="{\"hook_event_name\":\"${event}\",\"session_id\":\"${session}\"}"
fi

if command -v curl >/dev/null 2>&1; then
  curl -s -m 1 -o /dev/null --data-binary "$payload" -H 'Content-Type: application/json' "$url" >/dev/null 2>&1
fi

exit 0
