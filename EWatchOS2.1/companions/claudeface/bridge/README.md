# claude-bridge

Host daemon for Claude Face (EWatch). Collects Claude Code session state via
hooks + the statusline, tracks rate limits, and pushes it to the watch over
USB-CDC serial (protocol in `../protocol/protocol.json`).

## Install

```sh
cd bridge
npm install
npm run build
npm test
```

Run it directly: `node dist/src/cli.js` (binds `127.0.0.1:7337` by default).

To autostart via launchd (NOT done automatically — run when you're ready):

```sh
sh scripts/install-launchd.sh     # writes + loads ~/Library/LaunchAgents/com.ewills.claude-bridge.plist
sh scripts/uninstall-launchd.sh   # unloads + removes it
```

Logs go to `~/Library/Logs/claude-bridge.log` (bridge) and
`~/Library/Logs/claude-bridge-watch.log` (raw watch log lines, rotated at 5MB).

Wire up the Claude Code hooks + statusline with `../claude-config/install.sh`
(see that directory's own notes) — it merges into `~/.claude/settings.json`
without clobbering anything you already have.

## Config

`~/.config/claude-bridge/config.json` (optional; all fields optional):

```json
{
  "port": 7337,
  "serialPath": "/dev/cu.usbmodem1101",
  "statuspage": { "enabled": false },
  "oauthUsage": { "enabled": false },
  "quietHours": { "start": "23:00", "end": "08:00" }
}
```

Env overrides (take precedence over the config file):
- `CLAUDE_BRIDGE_PORT`
- `CLAUDE_BRIDGE_SERIAL_PATH`
- `CLAUDE_BRIDGE_CONFIG_DIR` (default `~/.config/claude-bridge`; also relocates
  `ratelimits.json`)

`serialPath` / `CLAUDE_BRIDGE_SERIAL_PATH` is an explicit override; leave unset
and the bridge auto-probes every `/dev/cu.usbmodem*` by writing `#CFQ\n` and
waiting up to 1.5s for a valid `#CFH` HELLO frame.

## Endpoints (127.0.0.1 only)

| Method | Path | Purpose |
|---|---|---|
| POST | `/hook` | Claude Code hook payload (see `claude-config/hooks/`) |
| POST | `/statusline` | Claude Code statusline payload |
| GET | `/state` | Debug JSON: session table, rate limits, last hook/frame timestamps |
| GET | `/healthz` | `{ok:true}` liveness check |
| POST | `/serial/release?seconds=N` | Closes the serial port for N seconds |
| POST | `/serial/reacquire` | Re-opens the serial port immediately |

## Serial-port sharing with PlatformIO

The watch's USB-CDC port can only be held by one process. Before running
`pio run -t upload` or `pio device monitor`, release it:

```sh
curl -s -X POST 'http://127.0.0.1:7337/serial/release?seconds=60'
# ... pio run -t upload ...
curl -s -X POST 'http://127.0.0.1:7337/serial/reacquire'   # optional; auto-reacquires after `seconds`
```

The bridge never toggles DTR/RTS on open (`hupcl: false`) — the ESP32-S3's
native USB-JTAG serial resets on certain DTR/RTS transitions. **This is
documented but not yet verified against real hardware** — treat it as a risk
to confirm on the bench, not a settled fact.

## Simulator

Exercises every documented scene (state, rate-limit edge cases, session
fan-out, protocol-version mismatch, bridge disconnect) without a running
Claude Code session:

```sh
node dist/src/cli.js simulate --dry-run --speed 20     # prints decoded frames, no serial port
node dist/src/cli.js simulate --port /dev/cu.usbmodemXXXX --speed 1
```

`--speed N` divides all sleeps by N (useful for CI / fast iteration).
`--dry-run` never opens a real port; frames are decoded and printed to stdout.

## Privacy

The bridge never stores or forwards prompt text, file contents, full shell
commands, or absolute paths — only short derived summaries (`Bash|npm test`,
`Edit|file.ts`, a project basename). Server logs print only the hook/event
*name* and an 8-character session-id prefix, never payload bodies. The hook
script additionally strips `prompt` (UserPromptSubmit) and `tool_response`
(PostToolUse) client-side before anything reaches the loopback socket at all.

## Tests

`npm test` builds then runs `node --test` over `dist/test/*.test.js`: state
machine transitions per hook, priority ordering, reaping/quiet timers (clock
injected), rate-limit pace/elapsed/projected-hit math, current-tool formatting
privacy, protocol encode/decode round-trips + CRC rejection, and an end-to-end
HTTP-server-with-fake-link test measuring POST→frame latency.

## Known risks / unverified assumptions

- DTR/RTS-on-open behavior against real ESP32-S3 USB-JTAG hardware (see
  above) — not yet bench-tested.
- `SerialLink`'s reconnect/backoff and `/serial/release`+`/serial/reacquire`
  paths are implemented per spec but only exercised through the `FakeLink` in
  automated tests; no test opens a real serial port (SPEC explicitly disallows
  real serial in tests).
- Hook/statusline field assumptions are recorded in
  `../docs/docs-verification.md`; where that doc and SPEC.md disagreed, the
  docs won (e.g. `StopFailure` is the real hook name for API errors, not a
  separate rate-limit hook; `Notification` carries `message`/`title` in
  addition to `notification_type`).
