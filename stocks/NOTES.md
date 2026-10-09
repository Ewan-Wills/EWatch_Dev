# stocks — build notes & hardware-verify guide (EWatchOS2.1)

Live stock tracker for EWatch, **ported to EWatchOS2.1** (the full shipping OS:
watch faces, launcher, settings, WiFi, Spotify, games, etc. — the stocks app is
added on top). The PC companion (`pc/stocks_feed.py`) polls Yahoo Finance's
keyless v8 chart endpoint and pushes tickers over USB serial; the watch swipes
through them showing symbol, big price, colored day %, and a sparkline.

**WiFi stays ON in this OS2.1 build** (Spotify / Gallery / WiFi-settings depend
on it — the BaseOS `-DEWATCH_ENABLE_WIFI=0` size optimisation was dropped). The
stocks app itself still does **no networking**: all quote data arrives over USB
serial from the PC companion. Leaving WiFi on does not change the app's data
path; it just keeps the rest of the OS intact.

## What changed in the OS2.1 port

The app's view code (`src/apps/stocks.{h,cpp}`) is unchanged from BaseOS — it
already used only APIs that survived the OS2.1 diff (`ModelLock`,
`model.revision`, `switchTo`, `theme()`/`ThemeColors`, `tappedBack`,
`frameCanvas()`, `hapticBuzz`). Registration moved files vs BaseOS:

- `src/core/model.h`: added `Stocks,` to `enum class Screen` (after `Island`).
- `src/apps/system/registry.cpp`: `#include "stocks.h"`; `static StocksView
  vStocks;`; `case Screen::Stocks: return &vStocks;` in `viewFor()`; and one
  launcher row `{ "Stocks", Icon::Sparkle, aura::kGreen, Screen::Stocks, nullptr }`
  in `kTopApps[]`. (No perfect stock icon exists; `Sparkle` is the closest.)
- `src/core/power_mgr.cpp`: added `case Screen::Stocks: return true;` to
  `screenBlocksSleep()` so the screen stays lit while a live feed is pushing,
  with no touch input. (Keep-awake moved here from BaseOS's
  `controller.cpp::blockSleep`.)

**No `desiredFrameMs()` override was added.** stocks has no continuous
animation (the sparkline is a static polyline redrawn on data change, no
transitions). In OS2.1 the render task polls `model.revision` every ~50 ms, and
the RX task's `nudgeRenderer()` bumps `model.revision` on every store mutation,
so incoming frames and swipes still wake the renderer promptly. A
`desiredFrameMs()` override is only needed for continuous motion, which this app
doesn't have.

`pc/` is **unchanged** — still the Yahoo v8 chart companion (Stooq is dead; do
not revert it). No `main.cpp` edits: the RX task self-starts lazily from
`StocksView::onEnter()` via `ensureStoreAndTask()`.

## What it does

- **Watch app** (`src/apps/stocks.{h,cpp}`, registered in the launcher as
  "Stocks"): a dedicated FreeRTOS RX task reads newline-terminated ASCII frames
  from the USB-CDC serial link, parses them into a mutex-guarded in-RAM store
  (up to 16 tickers, 32 sparkline points each), and bumps `model.revision` to
  wake the renderer. The view draws into the shared `frameCanvas()` (flicker-
  free double buffer; falls back to direct panel draw if PSRAM is exhausted):
  symbol (FreeSansBold24), price, a green/red triangle + signed percent, an
  autoscaled sparkline polyline, an index `n / N` footer and page dots. Until
  the first frame arrives it shows a "Waiting for PC..." screen.
- **PC companion** (`pc/`): Python 3 + pyserial. Polls Yahoo's v8 chart endpoint
  (one request per symbol), reads the live price + previous close + daily closes
  from the JSON, computes change% vs previous close, and sends one `>>TICK`
  frame per symbol. Only external dep is pyserial (HTTP/JSON use the stdlib).
  See `pc/README.md`.

## Serial protocol (PC -> watch)

Native USB CDC, `Serial` @ 115200 (CDC ignores baud / runs at USB FS speed).
Newline-terminated ASCII, one frame per line. Per the ShortsApps convention,
telemetry/control lines are prefixed with the `>>` sentinel so the watch's
parser ignores boot-log and any other noise. Lines not starting with `>>` are
dropped. Overlong lines are truncated at the newline (parser can't run away).

```
>>TICK <symbol> <price> <changePct> <nPoints> <p0> <p1> ... <p(n-1)>
>>CLEAR
```

- `TICK` — upsert one ticker. `symbol` is 1..11 chars, no spaces (shown on the
  watch as-is, uppercased by the PC side). `price` and `changePct` are floats.
  `nPoints` is 0..32; that many float sparkline points follow, **oldest first**.
  A repeated symbol updates in place; a new one is appended (max 16, then
  further new symbols are ignored).
- `CLEAR` — drop all tickers. The PC sends this once at startup so the watch's
  list matches the current `--tickers`.

Direction is PC -> watch only; the watch sends nothing back for this app.

### Example frame
```
>>TICK AAPL 182.5200 1.3400 5 180.1 181.0 181.8 182.1 182.52
```

## Data source: Yahoo v8 chart (symbol format & rate limits)

Stooq was dropped — every variant now returns an HTML `404`. Replaced with
Yahoo Finance's keyless v8 chart endpoint, which returns live price **and**
daily history in one call with no API key and no crumb.

- **Endpoint (one request/symbol/poll):**
  `https://query1.finance.yahoo.com/v8/finance/chart/<SYM>?range=1mo&interval=1d`
  (`range` widens to `3mo`/`6mo` when `--points` > 20/60). The script falls back
  to `query2.finance.yahoo.com` on `429`/5xx.
- **A browser User-Agent is required** — Yahoo answers a bare agent with
  `403`/`429`. The script sends a Mozilla UA.
- **JSON fields used** (`chart.result[0]`): `meta.regularMarketPrice` (live
  price), `meta.chartPreviousClose` (→ change% = `(price-prev)/prev*100`),
  `indicators.quote[0].close` (daily-close array → sparkline; **nulls are
  dropped**, final point swapped for the live price).
- **Symbols:** bare US tickers (`AAPL`); UK/LSE `.L` (`VOD.L`); indices `^`
  (`^GSPC`, `^FTSE`); FX `GBPUSD=X`.
- **Rate limiting:** Yahoo `429`s heavy / datacenter / VPN traffic. The
  companion catches non-200 / HTML / bad-JSON / `429`, logs `fetch failed
  (...)`, and **skips that symbol instead of pushing bad data**. Mitigate with a
  larger `--interval`, fewer tickers, and a residential IP.

## What's stubbed / simplified (honest list)

- **Price granularity is end-of-day**, not intraday ticks: the sparkline is
  daily closes; the final point is replaced with Yahoo's live
  `regularMarketPrice` so the big number moves intraday, but the line itself
  updates once a day.
- **No persistence:** tickers live only in RAM. On reboot the watch shows
  "Waiting for PC..." until the companion pushes again (it re-sends every
  interval, so this self-heals within one `--interval`).
- **Company names:** the watch shows the ticker symbol as the heading.
- **Page dots** cap at 12 even if more than 12 tickers are loaded (the `n / N`
  footer still shows the true count).

## Could NOT be verified here (needs Ewan)

- **On-hardware behavior** — this port is compile-verified only; I do not flash.
  Everything about the live display, touch/swipe navigation, haptics, and
  stay-awake-while-receiving is unverified on real hardware.
- **Live Yahoo data from this machine** — the build environment's IP may be
  `429`-rate-limited by Yahoo. The JSON-parse path, frame builder, and watch
  parser were cross-checked against each other but not run end-to-end here.

## Build (compiles clean — verified on OS2.1)

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd ShortsApps/stocks
~/.platformio/penv/bin/pio run -e ewatch
```
Last result: **[SUCCESS]** — RAM 62.1% (203,492 B), Flash 54.9% (1,726,121 B).
(Larger than the BaseOS build because this is the full OS2.1 with WiFi + Spotify
+ games + the stocks app on top, not a stripped single-app image.)

## How to verify on hardware (Ewan)

1. **Flash:**
   ```sh
   export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
   cd ShortsApps/stocks
   ~/.platformio/penv/bin/pio run -e ewatch -t upload
   ```
   (Skip `-t monitor` — the companion needs sole ownership of the port. If you
   do monitor first to read boot logs, close it before running the feed.)
2. **Open the app:** on the watch, swipe up from the watch face → tap **Stocks**.
   You should see "Waiting for PC...".
3. **Start the feed** (new terminal, watch still plugged in):
   ```sh
   cd ShortsApps/stocks/pc
   pip install -r requirements.txt
   python3 stocks_feed.py --tickers AAPL,MSFT,TSLA --interval 60
   ```
   Watch its stderr: each symbol should log `AAPL: <price> (<pct>%) spark[N]`.
   If you see `fetch failed (HTTP 429 ...)`, see the Yahoo rate-limit note above.
4. **Look for (acceptance):**
   - Within a second of the first log line, the watch shows a ticker: symbol at
     top, big price, a green ▲ / red ▼ with the signed %, and a sparkline.
   - **Swipe left** (or up) advances to the next ticker; **swipe down** goes
     back one; tapping the screen's right/left half also moves next/prev. Each
     move gives a short haptic buzz.
   - **Right-swipe** or a short **SW2** press returns to the launcher.
   - Green for gainers, red for losers; the sparkline is the same color.
   - The screen should stay lit (no dim/sleep) while the feed is pushing.
5. **Quick protocol test without Yahoo** (optional): with the app open, pipe a
   handcrafted frame to the port (adjust the device path), e.g.
   ```sh
   printf '>>TICK DEMO 123.45 2.50 5 120 121 122 123 123.45\n' > /dev/cu.usbmodemXXXX
   ```
   A "DEMO" ticker should appear immediately.

## 2026-10-05 — serial hardening (traps A-E from the bookreader fix)

- **Trap A (fixed), `src/apps/stocks.cpp`:** `stocksRxTask` read Serial
  forever once Stocks had been opened, racing the ewlog console task (ewcon)
  for every byte: split `>>TICK` lines on the watch side, "unknown: ... (try
  help)" replies from the console, and the app also stole console commands
  after you left it. Now `onEnter` calls `ewlogConsolePause(true)` and sets
  `g_viewActive`; `onExit` clears it and calls `ewlogConsolePause(false)`. The
  RX task only reads while `g_viewActive && ewlogConsolePaused()`, otherwise it
  idles (50 ms) and drops any half line. Side effect: frames sent while the app
  is closed are not stored; the PC re-sends every poll (default 60 s), so after
  re-entering the screen the data refreshes on the next poll.
- **Trap B (not an instance):** each `>>TICK` line is < ~450 B and the PC sends
  them one at a time with an HTTP fetch between each; the watch drains every
  15 ms. Nowhere near the 4 KB RX queue, so no flow control added.
- **Trap C (not an instance):** the watch never replies; the PC never waits.
- **Trap D (not an instance):** custom-font baselines are 62/112 (Bold24,
  minYOffset -35), 70/149 (Bold12, -17), 150/208 (Sans12, -17), all glyph tops
  >= 27. A long symbol can make `drawCentered` x slightly negative, but at
  y >= 27 that only wraps into the previous row inside the canvas, not before it.
- **Trap E (not an instance):** `g_mutex` is held only for short copies.
- Built `pio run -e ewatch` SUCCESS. Not flashed / not tested on hardware.

## 2026-10-05 - shared OS hardening (from bookreader + EWatchOS2.1 audit)
- Power latch is pad-held (`holdLatch()` in main.cpp / power.h): flashing no
  longer needs the side button; `unlatchPower()` releases the hold first.
- `Serial.setRxBufferSize(4096)`; `ewlogConsolePause()` API so an app that
  reads Serial can stop the debug console stealing its bytes.
- `SafeCanvas` (src/drivers/safe_canvas.h) for frameCanvas()/viewer3d: GFX
  1.4.9 text partly above/left of the screen wrote outside the framebuffer.
- ewlog `log dump` no longer holds the log mutex across the whole dump.

## Verified on hardware (ewt) — 2026-10-06
- `test/smoke.ewt` PASS on the real V2.0 watch: boot to Stocks ('Waiting for PC...'), swipe-left stays in-app, 0 errors (9 steps). Snap: `.ewt/boot.png`. The app owns USB serial, so the smoke runs with `raw keepcon 1` (ewlog test hook): the console stays with ewt and the app's PC RX reader is deliberately inert.
- NOT exercised: the live PC-data path (pc/ companion over USB serial). ewt holds the USB serial port while driving the watch, so that path needs the companion run separately with ewt not holding the port.
