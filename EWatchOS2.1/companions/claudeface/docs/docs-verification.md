# Claude Code Hooks & Statusline — Docs Verification

Sources fetched:
- Hooks: https://code.claude.com/docs/en/hooks (loaded successfully; page also references a linked llms.txt index but no separate "hooks-guide" page was linked/needed — the single hooks page covers events, config, and exit-code semantics)
- Statusline: https://code.claude.com/docs/en/statusline (loaded successfully)
- Local install check: `claude --version` — the `claude` binary was not on PATH in this shell; `npx -y @anthropic-ai/claude-code --version` reported **2.1.280 (Claude Code)**, confirming this docs snapshot is current as of a recent 2.1.x release (the statusline doc's version-gated notes go up to "Requires Claude Code v2.1.260 or later" / "v2.1.267+", consistent with 2.1.280 being current).

---

## A. HOOKS

### A1. Exact list of documented hook events

Full list (33 events), quoted/paraphrased from the events section of the hooks page:

`SessionStart`, `Setup`, `UserPromptSubmit`, `UserPromptExpansion`, `PreToolUse`, `PermissionRequest`, `PermissionDenied`, `PostToolUse`, `PostToolUseFailure`, `PostToolBatch`, `Notification`, `MessageDisplay`, `SubagentStart`, `SubagentStop`, `TaskCreated`, `TaskCompleted`, `Stop`, `StopFailure`, `TeammateIdle`, `InstructionsLoaded`, `ConfigChange`, `CwdChanged`, `DirectoryAdded`, `FileChanged`, `WorktreeCreate`, `WorktreeRemove`, `PreCompact`, `PostCompact`, `PreModelSwitch`, `PostModelSwitch`, `Elicitation`, `ElicitationResult`, `SessionEnd`.

Checklist against the requested names:

| Requested name | Status |
|---|---|
| `SessionStart` | CONFIRMED — "When a session begins or resumes" |
| `UserPromptSubmit` | CONFIRMED — "When you submit a prompt, before Claude processes it" |
| `PreToolUse` | CONFIRMED — "Before a tool call executes" |
| `PostToolUse` | CONFIRMED — "After a tool call succeeds" |
| `PostToolUseFailure` | CONFIRMED — "After a tool call fails" |
| `PermissionRequest` | CONFIRMED — "When a tool call needs a permission decision" |
| `Notification` | CONFIRMED — "When Claude Code sends a notification" |
| `Stop` | CONFIRMED — "When Claude finishes responding" |
| `StopFailure` (API-error/rate-limit hook) | CONFIRMED — exact name is `StopFailure`, defined as "When the turn ends due to an API error." This is the hook that reports API errors/rate limits: its matcher ("error type") enumerates `rate_limit`, `overloaded`, `authentication_failed`, `oauth_org_not_allowed`, `account_on_hold`, `billing_error`, `invalid_request`, `model_not_found`, `server_error`, `max_output_tokens`, `cloud_credential_error` (v2.1.267+), `unknown`. |
| `SubagentStart` | CONFIRMED — "When a subagent is spawned" |
| `SubagentStop` | CONFIRMED — "When a subagent finishes" |
| `PreCompact` | CONFIRMED — "Before context compaction" |
| `PostCompact` | CONFIRMED — "After context compaction completes" |
| `SessionEnd` | CONFIRMED — "When a session terminates" |

Other documented events not in the requested list: `Setup`, `UserPromptExpansion`, `PermissionDenied`, `PostToolBatch`, `MessageDisplay`, `TaskCreated`, `TaskCompleted`, `TeammateIdle`, `InstructionsLoaded`, `ConfigChange`, `CwdChanged`, `DirectoryAdded`, `FileChanged`, `WorktreeCreate`, `WorktreeRemove`, `PreModelSwitch`, `PostModelSwitch`, `Elicitation`, `ElicitationResult`.

### A2. Stdin JSON fields

**Common fields on every hook invocation** (quoted example from the page):

```json
{
  "session_id": "abc123",
  "prompt_id": "550e8400-e29b-41d4-a716-446655440000",
  "transcript_path": "/home/user/.claude/projects/.../transcript.jsonl",
  "cwd": "/home/user/my-project",
  "scratchpad_dir": "/tmp/claude-1000/-home-user-my-project/abc123/scratchpad",
  "permission_mode": "default",
  "effort": { "level": "medium" },
  "hook_event_name": "PreToolUse",
  "agent_id": "subagent-id",
  "agent_type": "general-purpose"
}
```

- `permission_mode`: one of `"default"`, `"plan"`, `"acceptEdits"`, `"auto"`, `"dontAsk"`, `"bypassPermissions"` — more values than the 4 commonly assumed (`default`/`plan`/`acceptEdits`/`bypassPermissions`); `auto` and `dontAsk` are also documented.
- `agent_id` / `agent_type`: present as **common** fields "only when inside subagent," not exclusively SubagentStart/Stop-specific fields. `agent_type` examples given: `"Explore"`, `"security-reviewer"`, `"general-purpose"`.
- `scratchpad_dir`: absent if unavailable; requires v2.1.257+.
- `prompt_id`: absent until first user input; requires v2.1.196+.

**Tool events** (`PreToolUse`, `PostToolUse`, `PostToolUseFailure`, `PermissionRequest`, `PermissionDenied`):

```json
{ "tool_name": "Bash", "tool_input": { "command": "npm test" }, "tool_use_id": "toolu_01ABC123..." }
```

- `tool_name` CONFIRMED; example tool names given: `Bash`, `PowerShell`, `Edit`, `Write`, `Read`, plus MCP tools as `mcp__<server>__<tool>`.
- `tool_input` shape for **Bash**: CONFIRMED as `{ "command": "..." }`.
- `tool_input` shape for **Edit/Write/Read** (`file_path`): **NOT explicitly quoted** in the fetched page content — the page shows only the Bash example inline; it does not reproduce the Edit/Write/Read `tool_input` JSON in the section retrieved. Treat as PARTIALLY CONFIRMED / not independently verified from this fetch (consistent with prior knowledge that these carry `file_path`, but not confirmed by direct quote this pass).

**`Notification` event:**

```json
{ "notification_type": "permission_prompt", "tool_name": "Bash", "reason": "User action required" }
```

Fields: `notification_type`, `tool_name`, `reason` (no separate `message`/`title` fields were shown in the quoted schema — NOT FOUND in this fetch, though `reason` plays a similar role). `notification_type` enum (11 values found, more than the 4 requested): `permission_prompt`, `idle_prompt`, `auth_success`, `elicitation_dialog`, `elicitation_url_dialog`, `elicitation_complete`, `elicitation_response`, `agent_needs_input`, `agent_completed`, `quota_auto_resume_fired`, `quota_auto_resume_stale`, `quota_auto_resume_disabled`.

**`Stop` and `SubagentStop` events:**

```json
{ "last_assistant_message": "The task is complete...", "stop_reason": "end_turn" }
```

- `last_assistant_message`: CONFIRMED.
- `stop_hook_active`: **NOT FOUND** in the fetched schema — the page's example instead shows `stop_reason` (e.g. `"end_turn"`), not a `stop_hook_active` boolean. DIFFERENT from what was asked.

**`SessionStart`:**

```json
{ "source": "startup", "model": "claude-opus-5" }
```

`source` values: `"startup"`, `"resume"`, `"clear"`, `"compact"`, `"fork"` — CONFIRMED, 5 values (matches matcher table too).

**`SessionEnd`:**

```json
{ "reason": "clear" }
```

`reason` values: `"clear"`, `"resume"`, `"logout"`, `"prompt_input_exit"`, `"other"` — CONFIRMED, 5 values.

**`PreCompact`/`PostCompact`:** no separate input-field JSON example was shown; only the **matcher** ("trigger") values were documented: `"manual"`, `"auto"`. CONFIRMED for the trigger values; no additional per-event body fields found beyond the common fields.

**`SubagentStart`:** matcher values are agent type/name (`"general-purpose"`, `"Explore"`, `"Plan"`, custom names, plugin-scoped names like `^my-plugin:reviewer$`). No separate `agent_id`/`agent_type` fields beyond the common-fields table (those two are documented once, in the common fields, not duplicated per-event).

**`StopFailure`:** matcher = "error type", values listed above (includes `rate_limit`). This is the confirmed answer to "any hook reporting API errors/rate limits."

### A3. settings.json hooks config format

CONFIRMED format (quoted):

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash|Edit",
        "hooks": [
          { "type": "command", "if": "Bash(rm *)", "command": "...", "args": [], "timeout": 30, "statusMessage": "...", "async": false, "asyncRewake": false, "shell": "bash" },
          { "type": "http", "url": "http://localhost:8080/hooks/pre-tool-use", "timeout": 30, "headers": {"Authorization": "Bearer $MY_TOKEN"}, "allowedEnvVars": ["MY_TOKEN"] },
          { "type": "mcp_tool", "server": "my_server", "tool": "security_scan", "input": {"file_path": "${tool_input.command}"}, "timeout": 30 },
          { "type": "prompt", "prompt": "Is this command safe? $ARGUMENTS", "model": "claude-opus-5", "timeout": 30 },
          { "type": "agent", "prompt": "Validate this operation: $ARGUMENTS", "timeout": 60 }
        ]
      }
    ]
  }
}
```

- `matcher`: CONFIRMED — string, exact/list match for simple names, else treated as unanchored JS regex.
- `hooks[]`: CONFIRMED — array of handler objects.
- `type: "command"`: CONFIRMED.
- `timeout`: CONFIRMED, in seconds.
- **`async` option**: CONFIRMED — exists on `type: "command"` (`"async": false` example), plus a related `asyncRewake` field ("run in background, wake Claude on exit 2").
- **Default timeout**: CONFIRMED, but not a single number — it's type-dependent: 600s for `command`/`http`/`mcp_tool`; 30s for `prompt`; 60s for `agent`. Lower overrides apply to specific events: 30s for `UserPromptSubmit`, `PreModelSwitch`, `PostModelSwitch`; 10s for `MessageDisplay`.

### A4. Stdout / exit code semantics

- **Exit 0**: success. Stdout parsed as JSON only if it starts with `{` and ends with `}`; valid JSON matching schema → decision honored; valid JSON failing schema, or unparseable/plain text → non-blocking, action proceeds. **Plain-text stdout is only injected into Claude's context for `UserPromptSubmit`, `UserPromptExpansion`, `SessionStart`, and `PostModelSwitch`** — for all other events plain text goes to the debug log only. This directly confirms SessionStart and UserPromptSubmit inject stdout into context (and additionally UserPromptExpansion, PostModelSwitch, which the spec didn't list).
- **Exit 2**: blocking error, always blocks regardless of JSON `"allow"`. Blocking reason from JSON decision reason or stderr text. Effect is event-dependent (PreToolUse → tool call blocked; UserPromptSubmit → prompt rejected/erased; UserPromptExpansion → expansion blocked; PreModelSwitch → switch blocked; Elicitation/ElicitationResult → hookSpecificOutput ignored; others vary).
- **Other exit codes (1, 3, ...)**: generally non-blocking (decision honored only if valid JSON present); exceptions: `PreModelSwitch` timeout blocks the switch; `WorktreeCreate`/`WorktreeRemove` — any non-zero exit fails the operation.
- Non-zero exit does surface errors: exit 2's stderr text becomes the blocking reason shown to the user/Claude.

### A5. HTTP hook type

CONFIRMED — `"type": "http"` exists.

Fields: `url` (required), `headers` (optional, `$VAR`/`${VAR}` interpolation), `allowedEnvVars` (optional allowlist for header interpolation), `timeout` (optional, default 600s, shared with command/mcp_tool default), `statusMessage` (optional), `if` (optional, permission-rule filter).

Behavior: POSTs the hook's JSON input as the body with `Content-Type: application/json`; response uses the same JSON output format as command hooks. The page references a further "HTTP response handling" subsection for failure behavior specifics, but that subsection's body text was not returned in this fetch — failure-mode details beyond "same JSON output format as command hooks" are **NOT FOUND** in the retrieved excerpt.

---

## B. STATUSLINE

### B1. Full JSON input schema — field-by-field confirmation

| Path | Status | Notes |
|---|---|---|
| `rate_limits.five_hour.used_percentage` | CONFIRMED | number 0–100 |
| `rate_limits.five_hour.resets_at` | CONFIRMED | Unix epoch seconds |
| `rate_limits.seven_day.used_percentage` | CONFIRMED | number 0–100 |
| `rate_limits.seven_day.resets_at` | CONFIRMED | Unix epoch seconds |
| `context_window.used_percentage` | CONFIRMED | pre-calculated; may be `null` early in session |
| `context_window.context_window_size` | CONFIRMED | "200000 by default, or 1000000 for models with extended context" |
| `model.display_name` | CONFIRMED | |
| `model.id` | CONFIRMED | e.g. `"claude-opus-5-5"` |
| `effort.level` | CONFIRMED | values: `low`, `medium`, `high`, `xhigh`, `max`. Quote: "Ultracode is not a distinct level and reports as `xhigh`." Field absent when model doesn't support effort. |
| `fast_mode` | CONFIRMED | boolean |
| `thinking.enabled` | CONFIRMED | boolean |
| `cost.total_cost_usd` | CONFIRMED | resets to $0 on `/clear` (v2.1.211+; before that it carried over) |
| `cost.total_duration_ms` | CONFIRMED | wall-clock, accumulates across resumes |
| `cost.total_api_duration_ms` | CONFIRMED | time waiting on API |
| `cost.total_lines_added` | CONFIRMED | |
| `cost.total_lines_removed` | CONFIRMED | |
| `session_id` | CONFIRMED | |
| `session_name` | CONFIRMED, but conditionally absent | populated by `--name`/`/rename` or AI-generated title; the default display name (e.g. `my-app-3f`) does NOT populate it |
| `workspace.project_dir` | CONFIRMED | dir where Claude Code launched |
| `workspace.current_dir` | CONFIRMED | same value as top-level `cwd`; preferred over `cwd` for consistency |
| `workspace.repo.name` | CONFIRMED | also `.host`, `.owner`; absent outside git repo or without an `origin` remote; GitLab subgroup projects populate `owner` as full slash-path (v2.1.260+ for that case) |
| `prompt_cache.warm` | CONFIRMED | boolean; false if last response had no cache tokens even if caching was observed before |
| `prompt_cache.expires_at` | CONFIRMED | epoch seconds; `null` when last response had no cache tokens |
| `pr.number` | CONFIRMED | also `.url`; for GitLab this is the merge-request number (v2.1.234+) |
| `pr.review_state` | CONFIRMED | enum: `approved`, `pending`, `changes_requested`, `draft`. May be independently absent even when `pr` present. |
| `agent.name` | CONFIRMED | present only with `--agent` flag or agent settings configured |
| `version` | CONFIRMED | Claude Code version string, e.g. `"2.1.90"` |
| `output_style` | **DIFFERENT** | Actual path is `output_style.name` (an object with a `name` field), not a bare `output_style` string |
| `exceeds_200k_tokens` | CONFIRMED | boolean; fixed 200k threshold regardless of actual context window size |
| `transcript_path` | CONFIRMED | |

Additional fields present in the schema not on the requested list: `cwd`, `prompt_id`, `workspace.added_dirs`, `workspace.git_worktree`, `context_window.total_input_tokens`/`total_output_tokens`/`remaining_percentage`/`current_usage.{input_tokens,output_tokens,cache_creation_input_tokens,cache_read_input_tokens}`, `rate_limits.spend_limit.{used_percentage,resets_at}` (gateway-only, v2.1.251+, `used_percentage` can exceed 100), `prompt_cache.{caching_observed,ttl,requests,misses,expected_rebuilds,hit_ratio,cache_write_tokens,miss_recache_tokens,last_miss_at,last_miss_cause,miss_causes,recache_tokens_if_cold}`, `vim.mode`, `worktree.{name,path,branch,original_cwd,original_branch}`, `pr.kind` (GitLab MR marker).

### B2. Trigger / refreshInterval / padding / timeout

**When the statusline command runs** (quoted): runs once "when a session starts, including when you resume one." After that, re-runs on:
- A new assistant message arrives
- `/compact` finishes
- The permission mode changes
- Vim mode toggles
- The `command` in `statusLine` settings changes (skips debounce, runs immediately)
- A `refreshInterval` timer elapses, if set
- A rate-limit window's `resets_at` time passes
- A warm prompt cache's `expires_at` time passes

Debounce: 300ms (rapid changes batch; script runs once after changes stop). If a new update fires while the script is still running, the in-flight script is cancelled.

- **`refreshInterval`**: CONFIRMED field name. "The optional `refreshInterval` field re-runs your command every N seconds in addition to the event-driven updates. The minimum is `1`." Configured inside the `statusLine` settings object (same place as `type`/`command`). Units: seconds.
- **`padding`**: CONFIRMED. "The optional `padding` field adds extra horizontal spacing (in characters) to the status line content. Defaults to `0`." Also configured inside `statusLine`.
- **Timeout behaviour for the statusline command itself**: **NOT FOUND** — the fetched statusline page does not state a specific timeout value or behavior for the status line script (unlike the hooks page, which gives explicit per-type timeout defaults). Only debounce/cancellation-on-new-trigger behavior is documented.
- Also found: `hideVimModeIndicator` (boolean, suppresses built-in `-- INSERT --` text) — not requested but part of the same config object.

### B3. Caveats documented

- **`rate_limits` availability**: quoted — "appears only for claude.ai Pro and Max subscribers, or behind a Claude apps gateway that sets a spend limit for you, and only after the first API response in the session. Each window (`five_hour`, `seven_day`, `spend_limit`) may be independently absent, and Claude Code drops a window once its `resets_at` time passes." CONFIRMS: Pro/Max-only (or gateway spend-limit), absent before first response, and absent windows are possible/expected.
- **`resets_at` units**: Unix epoch seconds (stated for `rate_limits.*.resets_at` and reused as the same unit for `prompt_cache.expires_at`/`last_miss_at`).

---

## C. Differences vs our spec

Spec summary being checked: hooks map `SessionStart→IDLE`, `UserPromptSubmit→WORKING`, `PreToolUse→current tool`, `PostToolUse→count`, `PermissionRequest + Notification(permission prompt)→BLOCKED`, `Notification(idle prompt)→WAITING_INPUT`, `Stop→DONE`, `SubagentStart/Stop→count`, `PreCompact→COMPACTING`, `SessionEnd→remove`; plus an API-error hook→LIMITED. Statusline fields as in B1 with `refreshInterval=30`.

1. **All named hook events exist with the exact names assumed** — no renames needed for `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PermissionRequest`, `Notification`, `Stop`, `SubagentStart`, `SubagentStop`, `PreCompact`, `SessionEnd`.
2. **The "API-error hook" is `StopFailure`**, not a separate rate-limit-specific hook name. It fires "when the turn ends due to an API error," and its matcher/error-type values include `rate_limit` directly, plus 10 other error types (`overloaded`, `authentication_failed`, `oauth_org_not_allowed`, `account_on_hold`, `billing_error`, `invalid_request`, `model_not_found`, `server_error`, `max_output_tokens`, `cloud_credential_error`, `unknown`). Spec's `LIMITED` state should map from `StopFailure` matcher `rate_limit` (and arguably also `overloaded`), not from all `StopFailure` events — mapping *all* `StopFailure` errors to `LIMITED` would be wrong for e.g. `authentication_failed` or `invalid_request`, which aren't rate-limit conditions.
3. **`PostToolUseFailure` also exists** as a distinct event (tool call failed, separate from `PostToolUse` succeeding) — the spec's "PostToolUse→count" doesn't account for tool failures; consider whether failed tool calls should still increment the count or need separate handling.
4. **`PermissionDenied` also exists**, separate from `PermissionRequest` — fires "when auto mode denies a tool call." Spec only maps `PermissionRequest`; a denial in auto mode won't trigger `PermissionRequest`/`BLOCKED` the same way and may need its own handling if the UI should reflect an auto-denied call.
5. **`Notification.notification_type` has 11 documented values**, not just `permission_prompt`/`idle_prompt`/`auth_success`/`elicitation_dialog`: also `elicitation_url_dialog`, `elicitation_complete`, `elicitation_response`, `agent_needs_input`, `agent_completed`, `quota_auto_resume_fired`, `quota_auto_resume_stale`, `quota_auto_resume_disabled`. The spec only accounts for 2 of these (`permission_prompt`→BLOCKED, `idle_prompt`→WAITING_INPUT); the other 9 fall through unhandled unless explicitly ignored.
6. **`Stop` event schema does not show a `stop_hook_active` field** in the documented example — instead it shows `stop_reason` (e.g. `"end_turn"`). If the implementation depends on `stop_hook_active` to avoid infinite hook loops, that field's existence is **unconfirmed** from this fetch and should be checked against actual runtime payloads before relying on it.
7. **`PreCompact` documented with only `trigger` matcher values `manual`/`auto`** — no separate `PostCompact` state was requested in the spec ("PreCompact→COMPACTING") but `PostCompact` is a distinct, equally real event; the spec has no exit transition out of `COMPACTING` defined via a hook (it would need to rely on the next `SessionStart`/assistant message, or should add a `PostCompact` handler to close out `COMPACTING`).
8. **`SessionEnd` has 5 documented `reason` values** (`clear`, `resume`, `logout`, `prompt_input_exit`, `other`) — spec just says "remove," which is fine as a blanket action, but note `reason: "resume"` on `SessionEnd` is somewhat unusual (session ending because of a resume elsewhere) and might warrant different handling than a hard removal.
9. **Statusline: `output_style` is `output_style.name`, not a bare string field** — spec listed `output_style` without the nested path; must read `.name`.
10. **Statusline: no documented explicit timeout for the statusline script itself** (unlike hooks, which have clear per-type defaults) — only 300ms debounce and cancel-on-new-trigger are documented. If the spec assumes a specific statusline timeout value, that assumption is unconfirmed.
11. **Statusline: `refreshInterval=30` is valid** (minimum allowed is `1`, so 30 is fine), but note refresh is *in addition to* several event-driven triggers (new assistant message, `/compact` finish, permission-mode change, vim toggle, rate-limit `resets_at` passing, prompt-cache `expires_at` passing) — a pure 30s poll is not the only thing driving updates, so UI logic shouldn't assume updates only happen every 30s.
12. **`rate_limits` is Pro/Max-only (or gateway spend-limit) and absent until the first API response** — any spec logic that reads `rate_limits.*` unconditionally at session start will see it absent; must handle the missing-field case (also each window can independently disappear once its `resets_at` passes).
13. **Hook `tool_input` shape for Edit/Write/Read (`file_path`)** was not independently re-confirmed by a direct quote in this fetch (only the Bash `{command}` example was shown) — treat as a carry-over assumption from general knowledge, not freshly verified against the current docs page content retrieved.
14. **Notification event schema shown uses `reason`, not `message`/`title`** — the fetched example is `{"notification_type": "...", "tool_name": "...", "reason": "..."}`. `message`/`title` fields were NOT FOUND in the retrieved schema; if the spec's UI expects a `message`/`title` pair, it should instead use (or fall back to) `reason`.
15. **`async`/HTTP hook types exist and could be used to POST rate-limit/status events directly to a URL** (`type: "http"`), which may simplify the "API-error hook→LIMITED" wiring versus a shell `command` hook — confirmed as a viable alternative implementation path, with config fields `url`, `headers`, `allowedEnvVars`, `timeout`, `statusMessage`, `if`.

No other files were modified; only `/Users/ewills/Documents/EWatch/EWatch_Firmware/ClaudeFace/docs/docs-verification.md` was created.

---

## Main-agent re-check (2026-09-23, from the raw docs markdown: code.claude.com/docs/en/hooks.md and statusline.md)

Corrections to the sub-agent's findings above — **these override it**:
- **Stop** input DOES include `stop_hook_active`, plus `last_assistant_message`, `background_tasks`, `session_crons` (hooks.md, "Stop input"). Sub-agent claim #5 was wrong.
- **Notification** input DOES include `message`, optional `title`, `notification_type` (hooks.md: "Notification hooks receive `message` with the notification text, an optional `title`, and `notification_type`"). Sub-agent claim #13 was wrong.
- Notification types (12): permission_prompt, idle_prompt, auth_success, elicitation_dialog, elicitation_url_dialog, elicitation_complete, elicitation_response, agent_needs_input, agent_completed, quota_auto_resume_fired, quota_auto_resume_stale, quota_auto_resume_disabled.
- `permission_prompt` fires only after ~6 s without typing; **PermissionRequest fires immediately** → use PermissionRequest as the primary BLOCKED trigger (1 s acceptance target), permission_prompt as a backup.
- `idle_prompt` fires ~60 s after Claude finishes and only if you haven't typed; it is NOT sent while waiting on a usage-limit reset.
- **StopFailure** (`error`: rate_limit, overloaded, authentication_failed, oauth_org_not_allowed, account_on_hold, billing_error, invalid_request, model_not_found, server_error, max_output_tokens, cloud_credential_error, unknown) — only `rate_limit` (and billing_error/account_on_hold arguably) means LIMITED; others → DONE with an error.
- `async: true` exists on `type: "command"` hooks (runs in background; timeout not enforced). Default timeout 600 s (30 s for UserPromptSubmit).
- Other events relevant to the state machine: PostToolUseFailure (tool failed → still WORKING, count++), PermissionDenied (auto-mode denial → back to WORKING), PostCompact (leave COMPACTING), SessionEnd.reason (clear/resume/logout/prompt_input_exit/other).
- Statusline: all spec fields confirmed (rate_limits.five_hour/seven_day.{used_percentage,resets_at}, context_window.{used_percentage,context_window_size}, model.display_name, effort.level ∈ low|medium|high|xhigh|max, fast_mode, thinking.enabled, cost.*, session_id, session_name, workspace.project_dir, workspace.repo.name, prompt_cache.{warm,expires_at}, pr.number, pr.review_state ∈ approved|pending|changes_requested|draft, agent.name). `used_percentage` is a float (e.g. 23.5). Extra window `rate_limits.spend_limit` exists (gateway only). `context_window.used_percentage` may be null early. Statusline also re-runs when a warm prompt cache reaches `expires_at`.
