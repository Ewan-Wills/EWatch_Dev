# eWatch Cloud messages (watch side)

The contract with the server (API, formats, behaviour) is `MESSAGES_DESIGN.md`
in the EwatchCloud repo, branch feat/messages. This file covers the firmware.

## Files

| file | role |
|---|---|
| `core/cloud_msg.{h,cpp}` | the service: "cloud" task (core 0), pairing, contacts, inbox, read marks, photo download + JPEG decode, a 2-slot outbox, NVS `ewcloud` |
| `core/cloud_net.{h,cpp}` | `cloudHttp()`, the one network call: TLS (Let's Encrypt roots in `cloud_ca.h`), plain http, or the USB relay |
| `core/cloud_ca.h` | ISRG Root X1 + X2 (PEM) |
| `apps/system/cloud_views.{h,cpp}` | CloudSettings (link), CloudNotify (new-message card), Inbox, InboxRead, ContactPicker, the send toast |
| `apps/modules/write` | Send button (only while linked) |
| `apps/modules/draw` | Send in the toolbar |

Hooks: `controller.cpp` calls `cloudViewsPollNotify()` + `cloudToastPaint()` after
each render; `ewlog.cpp` passes console lines to `cloudConsole()`; `power_mgr.cpp`
keeps CloudSettings awake while pairing; `main.cpp` calls `cloudInit()`.

## Behaviour

* **Link:** Settings → eWatch Cloud → Link shows an 8-character code (pixel font,
  size 3). The service polls `pair/poll` every `interval` s until linked, expired
  (410) or wrong (404). Token `ewd_...` in NVS. A 401 later is confirmed with
  `GET /device/me`; only a 401 there too drops the token (Unlinked). 5xx and
  network errors never unlink.
* **New messages:** each time WiFi associates (any sync window) and every 60 s
  while it stays up, `GET /messages/summary`. A new `latest.id` raises the card
  (View / Later). In a busy screen (Write, Draw, the cloud screens) a toast
  replaces the card. The card uses `fromAscii` / `previewAscii` when the server
  sends them.
* **Inbox:** `GET /messages?compact=1&box=in&limit=10` (received only, so `r` is
  our read state). Opening a message marks it read; photos load as
  `?v=watch` JPEG, decoded to RGB565 (max 320x320).
* **Send:** Write and Draw call `cloudPickContact()` ("Me" first = your own
  account, e.g. a program on your computer), then `cloudSendText` /
  `cloudSendImage` (EWI1 runs, base64). Reply from the reader skips the picker
  ("to Name" over Write's Send). Write clears its text on Sent and keeps it on
  Failed. Toasts: Sending… / Sent / reason.
* **No WiFi enabled** (Settings → WiFi off or AP mode): sends fail with "Turn on
  WiFi"; nothing else happens.

## USB relay (bad WiFi, tests)

`cloud url usb:http://HOST:4100` makes `cloudHttp` print each request on the
USB console (`#WQ/#WB/#WE` lines, base64 body, CRC32), and ewt, while it holds
the port, makes the real HTTP call and streams the answer back in acked
150-byte chunks (`#WP` pending, `#WR/#WD/#WZ`). A request is printed again after
1.5 s without an answer; ewt answers a repeated id from its cache, so a POST
never runs twice. About 30–80 ms per request. The radio is not used at all, so
WiFi can stay off. Nobody relaying = "Can't reach eWatch Cloud" after 3 tries.
`cloud url default` goes back to https://api.ewatch.cloud.

## Console

`cloud status` (link, url, counts, wifi=/client=/known=/up=), `cloud check`,
`cloud pair`, `cloud unlink`, `cloud url <u>|default`. Write: `write text`, `write set <t>`.
Simulator only: `cloud fake say <text>`, `cloud fake fail <status>`.

## Tests

* Simulator (fake server = `tools/ewsim_host/host/stubs/cloud_net.cpp`):
  `EW_APPS="write draw" ewsimrec EWatchOS2.1 --env ewatch_v20 --snap DIR test/cloud/sim_send.ewt`
  (pair, reply, picker send, failed send keeps text, Draw send) and
  `test/cloud/sim_notify.ewt` (card: Later, View opens that message) and
  `test/cloud/sim_auth.ewt` (a stray 401 keeps the link, a revoked token unlinks).
  A copy of sim_send with a wrong recipient fails at that step.
* Watch, local backend: `test/cloud/pair.ewt` (relay; claims the code with
  `~/Documents/EWatch/local-run/messages/claim.sh` via ewt's `sh` step).

## Status / open

Verified on the watch (2026-10-13, local backend, USB relay, `test/cloud/pair.ewt` +
`test/cloud/e2e.ewt`, 49/51 steps; the last snap failed, see below):
pairing; text from Py B -> card -> View -> reader; reply from Write reaches Py B
(checked server-side); photo from Py B -> card -> View -> decoded 240x180; Draw ->
picker -> Py B receives the drawing (web PNG matches the stroke drawn).
Write and Draw smoke tests pass on the watch; Draw's must-fail fails at its step.

Open:
* TLS against https://api.ewatch.cloud once production is deployed.
* Screenshots (`ewt snap`) on an active screen still need a retry now and then
  (and one Draw snap failed 4 times). Cause found and mostly fixed: the CPU
  governor switched 80<->240 MHz twice a second on the watch face, which
  disturbs the USB console; it now holds 240 MHz while USB is plugged in.
* The outbox is shared: Write clears its text on the next "Sent", even if that
  was a Draw send queued at the same moment (rare).
