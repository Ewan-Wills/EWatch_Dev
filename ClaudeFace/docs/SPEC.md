# Claude Face — original spec (from Ewan, 2026-09-23)

> **Transport override (Ewan, same message):** "the connection should be serial over usb-c. therefore you may need a program running on the computer to connect to it."
> So every BLE item below (NimBLE, noble/bleak, GATT characteristics, bonding, RSSI/range, connection interval) is replaced by a USB-CDC serial link.
> The *message contents* (STATE / SESSIONS / TIME / EVENT) are kept exactly, framed as text lines — see `ClaudeFace/protocol/protocol.json` (single source of truth) and `docs/protocol-layout.md` (generated).
> Target firmware is **AuroraOS** (the current unified firmware that grew out of BaseOS; it has the watch-face registry and tiered power manager). "baseOS" in the text below means AuroraOS.
> "Connected" in the power section means: a valid `#CF` frame received within the last 90 s over USB serial.

## Mission

Build an always-on watch face for EWatch that shows, at a glance, what my Claude Code agents are doing and how much of my plan I've burned. It **replaces the default watch face** in baseOS — it is not a launcher app. Time stays at the top. It is mainly visual: I should be able to read the state from across the desk by colour and motion alone, without reading text.

Three deliverables: 1. `claude-bridge` host daemon (collects Claude Code state, pushes to watch). 2. Claude Code wiring (hooks + statusline script). 3. Watch firmware (face, link, haptics, power).

## 1. Where the data comes from

### A. Statusline JSON (push)
Claude Code pipes a JSON blob to the statusline command after each assistant message, on `/compact`, on permission-mode change, when a rate-limit window hits `resets_at`, and every `refreshInterval` seconds if set. Fields to use:
- `rate_limits.five_hour` / `rate_limits.seven_day` → `used_percentage` (0–100), `resets_at` (epoch s). Pro/Max only, only after the first API response in a session, each window independently absent, dropped once `resets_at` passes.
- `context_window.used_percentage`, `context_window.context_window_size`
- `model.display_name`, `effort.level`, `fast_mode`, `thinking.enabled`
- `cost.total_cost_usd` (client-side estimate at list price — notional for subscribers), `cost.total_duration_ms`, `cost.total_api_duration_ms`, `cost.total_lines_added`, `cost.total_lines_removed`
- `session_id`, `session_name`, `workspace.project_dir`, `workspace.repo.name`
- `prompt_cache.warm`, `prompt_cache.expires_at`
- `pr.number`, `pr.review_state`
- `agent.name`
The statusline script must: forward the raw JSON to the bridge fire-and-forget (hard timeout, never block, never error visibly), then still print a normal compact terminal statusline. Set `refreshInterval` to 30 so idle sessions keep reporting.

### B. Hooks (event push) → state machine
| Hook event | Bridge effect |
|---|---|
| SessionStart | Create session → IDLE |
| UserPromptSubmit | WORKING, record turn start, clear unacked DONE |
| PreToolUse | WORKING, current_tool = tool name + short target (Bash: first 24 chars of command; Edit/Write/Read: filename only, no path; others: name only) |
| PostToolUse | WORKING, tool count++ |
| PermissionRequest, Notification (permission prompt type) | BLOCKED |
| Notification (idle prompt type) | WAITING_INPUT |
| Stop | DONE ("your turn"), record turn duration |
| SubagentStart / SubagentStop | Subagent count ± |
| PreCompact | COMPACTING |
| SessionEnd | Remove session |
Also check the docs for any hook that reports API errors / rate-limit hits and map it to LIMITED.

Hook script rules: read stdin, POST to the bridge with a ≤1 s timeout, always exit 0, print nothing to stdout. If the bridge is down, silently do nothing. Install at user level (`~/.claude/settings.json`) by merging, never clobbering existing hooks/statusline. Provide `install.sh` (backs up settings.json with a timestamp) and `uninstall.sh`.

### C. Optional sources — off by default, behind config flags
- OAuth usage endpoint (undocumented) to fetch 5h/7d when no session is active. Fragile; on macOS the token is in the Keychain. Only if enabled; poll no faster than every 5 min.
- Anthropic status page (Statuspage JSON), polled every 5 min → incident flag.

### Honest gaps (reflect in UI)
- claude.ai web/desktop/mobile chat has no hooks; only shows via shared 5h/7d percentages.
- Rate-limit % is only as fresh as the last Claude Code API response. Track last_updated and show staleness.
- API-key sessions never get rate_limits. Rings must show "—", not 0%.

## 2. Bridge (`claude-bridge`)
- Node 20 + TypeScript. HTTP bound to 127.0.0.1 only, port configurable (default 7337). Endpoints: POST /hook, POST /statusline, GET /state (debug JSON), GET /healthz.
- Session table keyed by session_id. Rate-limit cache persisted to disk with resets_at; expire each window at its resets_at.
- Reaping: no event for 10 min and not WORKING → STALE; stale > 1 h → removed. WORKING with no events for 10 min → flag quiet (possibly hung).
- Global state priority: BLOCKED > LIMITED > WAITING_INPUT > WORKING > COMPACTING > DONE(unacked) > IDLE > NONE.
- Primary session: first BLOCKED, else most recent activity. Watch can override by swiping.
- Derived metrics per window: elapsed% = (now − (resets_at − window_len)) / window_len; pace = used% − elapsed%; projected_hit: extrapolate from last 30 min of used% samples; if limit would be hit before resets_at, report that time, else 0.
- Link → watch (was BLE; now USB serial). Auto-reconnect with backoff. Push on change (debounce 250 ms) plus a 30 s heartbeat. On every connect, write current epoch + UTC offset so the watch can sync the RV-3028.
- launchd plist for autostart; logs to ~/Library/Logs/claude-bridge.log.
- Privacy: nothing leaves the Mac except to the watch. No prompt text, no file contents, no full commands, no absolute paths.
- Simulator: `claude-bridge simulate` cycles through every state and edge case (absent rate limits, stale data, LIMITED, 4 sessions, BLOCKED, subagents, protocol version mismatch, bridge disconnect) so the face can be tested without running Claude.

## 3. Protocol (was BLE)
STATE: version byte first, ≤180 bytes: version, seq, flags (stale/incident/quiet/bridge-ok), global state, 5h used% (255 unknown), 5h resets_at, 7d same, 5h/7d pace (int8), projected_hit, session count, blocked count, subagent count, primary session {state, ctx%, model code, effort code, turn_elapsed_s, tool_count, current_tool char[24], project char[16], cache_expires_at, lines ± uint16}, estimated cost today (uint16 cents).
SESSIONS: up to 4 rows {state, ctx%, project char[12], turn_elapsed_s}.
TIME (write): epoch + UTC offset → RV-3028.
EVENT (watch → bridge): ACK (tap), SELECT_SESSION, reserved codes for phase 2 approve/deny.
Byte layout in docs, C struct and TS encoder generated from one definition.

## 4. Watch face — behaviour and visuals
Canvas 240×280. Brand: warm near-black background, burnt orange #E86B2B (RGB565 0xEB45), Space Grotesk for numerals/labels, JetBrains Mono for tool/command text — GFXfonts in flash. No visible flicker: render regions via an Arduino_Canvas in PSRAM (or dirty-region approach) and push only what changed.

Layout top to bottom:
1. Status strip (~40 px): time HH:MM large and dominant. Small battery % and a link glyph at the right. Time always from the RV-3028. With the bridge disconnected, the face must still be a fully working clock.
2. Hero (~130 px): concentric double ring. Outer = 5h used %. Inner = 7d used %. A small tick on each ring marks elapsed % of that window (pace marker). Fill behind the tick = fine. Fill ahead of the tick = burning too fast; ring shifts orange → red with how far ahead. Ring centre: state glyph plus one word. Under it: `5h resets 2h14`. If projected_hit set, show `hits limit ~16:40` instead.
3. Activity strip (~70 px), primary session: project name; current tool (`Bash · npm test`); live turn timer mm:ss; thin context bar (normal < 70 %, orange ≥ 70 %, red ≥ 90 %); one dot per running subagent, max 5.
4. Footer (~25 px): other sessions as small pills coloured by state, plus a count if > 4. When it's my turn and the primary session's prompt cache expires within 5 min, show `cache cold in 3m`.

State visual language:
| State | Look |
|---|---|
| NONE / IDLE | Dim, static, grey centre |
| WORKING | Orange arc rotating in the centre (~1 rev / 2 s, ≤ 15 fps, centre region only) |
| COMPACTING | Dotted / reversed spinner |
| DONE ("YOUR TURN") | Solid check, calm, steady |
| WAITING_INPUT | Like DONE, amber |
| BLOCKED ("NEEDS YOU") | Hero pulses orange ~1 Hz, backlight forced to 100 %, haptic |
| LIMITED | Red; time until reset shown large |
| OFFLINE (no bridge data > 90 s) | Rings greyed with last-known values, `last seen 4m` |
| Stale rate-limit data (> 15 min old) | Rings drawn hollow / dimmed |
Unknown or absent values render as "—", never 0. A protocol version mismatch shows `UPDATE BRIDGE` and nothing else from the payload.

Input: Tap = acknowledge DONE/BLOCKED alert (stops haptic repeats, sends ACK; never approves). Swipe left/right = cycle primary session. Long press = detail page (exact 5h/7d %, reset clock times, projected hit; model and effort; cost estimate and lines ±; PR review state). Side button keeps its baseOS behaviour.

Haptics (DRV2603, LEDC ch 1; EN low when idle):
| Trigger | Pattern |
|---|---|
| BLOCKED | 3 short pulses, repeated every 60 s up to 5× until acked or resolved |
| DONE after a turn > 60 s | 1 medium pulse |
| 5h crosses 80 % and 95 % | 2 short pulses, once per crossing per window |
| Window resets out of LIMITED | 1 short pulse |
Quiet hours configurable (default 23:00–08:00): visual only, no haptics.

Power / always-on:
- While connected to the bridge: face stays on. Backlight 100 % for 10 s after any state change, touch, or wrist raise (MMA8451), then ambient (~15 %, configurable). BLOCKED forces 100 %.
- Disconnected > 5 min: revert to normal baseOS timeout and sleep behaviour. Wake sources stay as baseOS defines them.
- Rendering: animate only the hero centre, only while WORKING. Everything else redraws on data change, or once per minute for the clock.
- Use light sleep between frames where baseOS allows.
- Measure and report battery drain per hour (always-on ambient/WORKING, disconnected).

Robustness: link callbacks copy into a double-buffered snapshot; rendering reads only the snapshot and never blocks on the link. A bridge crash/restart shows OFFLINE, then recovers without rebooting the watch.

## 5. Firmware constraints (non-negotiable)
- arduino-esp32 v2.0.17 with the v2 LEDC API (ledcSetup / ledcAttachPin / ledcWrite). LEDC ch 0 = backlight, ch 1 = motor.
- C++14, no digit separators. Pin macros from pins.h only.
- latchPower() stays the literal first line of setup(). i2cRestoreClock() after touch init.
- Nothing that can make MMA INT1 assert GPIO3 across a reset. GPIO19/20 untouched (USB D-/D+). WiFi not used by this feature.

## 6. Phase 2 — design for it, don't build it
Approve/deny from watch (PermissionRequest hook blocks waiting on bridge; hold 1.5 s approve, swipe deny; timeout falls through; never auto-approve; per-tool allowlist default empty). History page. Status-page incident indicator. ewatch.cloud relay. Admin API usage/cost page.

## 7. Deliverables
bridge/ (TS, README, launchd plist, simulator); claude-config/ (hooks, statusline, install.sh/uninstall.sh); firmware (face module, link service, fonts, config); docs/claude-face.md.

## 8. Acceptance tests
1. Prompt → WORKING within 1 s; tool name updates. 2. Permission prompt → NEEDS YOU + haptic within 1 s; tap stops repeats. 3. Turn ends → YOUR TURN; >60 s → one buzz. 4. Two sessions, one blocked → blocked is primary, other is a pill; swipe switches. 5. Kill bridge → OFFLINE within 90 s, clock runs; restart → recovers without watch reboot. 6. API-key session → rings "—". 7. Clock correct after time-sync and after reboot with no bridge. 8. `pio run` builds with no new warnings; no visible flicker. 9. `claude-bridge simulate` exercises every state.
