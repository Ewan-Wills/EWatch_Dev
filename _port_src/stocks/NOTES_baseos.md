# stocks — build notes & hardware-verify guide

Live stock tracker for EWatch. The PC companion (`pc/stocks_feed.py`) polls
Yahoo Finance's keyless v8 chart endpoint and pushes tickers over USB serial;
the watch swipes through them showing symbol, big price, colored day %, and a
sparkline. **The watch does not network — WiFi is gated off.**

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
Verified: `pc/stocks_feed.py`'s `build_frame()` emits exactly this shape and the
watch's `parseLine()` consumes it (same token order).

## Data source: Yahoo v8 chart (symbol format & rate limits)

Stooq was dropped — every variant (`stooq.com`/`.pl`, with/without `.us`) now
returns an HTML `404` even from a home network. Replaced with Yahoo Finance's
keyless v8 chart endpoint, which returns live price **and** daily history in one
call with no API key and no crumb.

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
  companion catches non-200 / HTML / bad-JSON / `429`, logs
  `fetch failed (...)`, and **skips that symbol instead of pushing bad data**.
  Mitigate with a larger `--interval`, fewer tickers, and a residential IP.

## What's stubbed / simplified (honest list)

- **Price granularity is end-of-day**, not intraday ticks: the sparkline is
  daily closes; the final point is replaced with Yahoo's live
  `regularMarketPrice` so the big number moves intraday, but the line itself
  updates once a day. Good enough for the demo; a true intraday sparkline would
  need a finer Yahoo range/interval (e.g. `range=1d&interval=5m`).
- **No persistence:** tickers live only in RAM. On reboot the watch shows
  "Waiting for PC..." until the companion pushes again (it re-sends every
  interval, so this self-heals within one `--interval`).
- **Company names:** the watch shows the ticker symbol as the heading (the
  spec's "name" == the ticker here). Yahoo's chart `meta` does carry a
  `longName`/`shortName`, but the symbol reads better at 24pt and avoids a
  wider frame — left as a possible enhancement.
- **Page dots** cap at 12 even if more than 12 tickers are loaded (the `n / N`
  footer still shows the true count).

## Could NOT be verified here (needs Ewan)

- **On-hardware behavior** — I only compile-build; I do not flash. Everything
  about the live display, touch/swipe navigation, and haptics is unverified on
  real hardware.
- **Live Yahoo data from this machine** — this build environment's IP is
  `429`-rate-limited by Yahoo (both `query1` and `query2`), so I could not pull
  real quotes end-to-end here. The coordinator confirmed the exact endpoint
  returns real data from the home network. The JSON-parse path, the frame
  builder, and the watch parser were cross-checked against each other; the
  error handling (429/HTML/bad-symbol → skip + log, never push bad data) was
  exercised live against the rate-limited responses.

## Build (compiles clean — verified)

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd ShortsApps/stocks
~/.platformio/penv/bin/pio run -e ewatch
```
Last result: **SUCCESS** — RAM 7.9% (26,004 B), Flash 15.3% (480,825 B).
WiFi is gated off (`-DEWATCH_ENABLE_WIFI=0` in `platformio.ini`).

## How to verify on hardware (Ewan)

1. **Flash + monitor:**
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
5. **Quick protocol test without Yahoo** (optional): with the app open, pipe a
   handcrafted frame to the port (adjust the device path), e.g.
   ```sh
   printf '>>TICK DEMO 123.45 2.50 5 120 121 122 123 123.45\n' > /dev/cu.usbmodemXXXX
   ```
   A "DEMO" ticker should appear immediately.
