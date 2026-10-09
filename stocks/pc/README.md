# stocks — PC companion (`stocks_feed.py`)

Polls Yahoo Finance's keyless **v8 chart** endpoint for your tickers and pushes
each one to the watch over USB serial. The watch does no networking — this
script is the only thing that talks to the internet.

## Install

```sh
cd ShortsApps/stocks/pc
python3 -m venv .venv && source .venv/bin/activate   # optional but tidy
pip install -r requirements.txt                      # just pyserial
```

## Run

Flash + plug in the watch first, close any serial monitor (the port is
single-owner), then:

```sh
python3 stocks_feed.py --tickers AAPL,MSFT,GOOGL,TSLA,NVDA --interval 60
```

- `--port` — serial device. Omit to auto-detect `/dev/cu.usbmodem*` (macOS).
  Override with e.g. `--port /dev/cu.usbmodem1101`.
- `--tickers` — comma-separated symbols (default `AAPL,MSFT,GOOGL,TSLA,NVDA`).
  US equities are bare tickers (`AAPL`). UK/LSE listings use the `.L` suffix
  (`VOD.L`); indices start with `^` (`^GSPC` = S&P 500, `^FTSE`); FX pairs look
  like `GBPUSD=X`. (These are Yahoo symbols — the same ones that work in the
  URL above.)
- `--interval` — seconds between full polls (default `60`).
- `--points` — sparkline length in daily closes (default `20`, max `32`).
- `--once` — poll a single time and exit (handy for a quick test).
- `--no-serial` — print the frames to stdout instead of opening the port, so
  you can check the live data without the watch plugged in.

On start the script sends `>>CLEAR` so the watch's list matches this run, then
loops: for each symbol it fetches the latest quote + recent daily closes and
sends one `>>TICK` frame. Progress is logged to stderr:

```
  AAPL: 182.52 (+1.34%)  spark[20]
```

Open the **Stocks** app on the watch (swipe up → Stocks). Swipe left / up / down
(or tap the left/right half of the screen) to move between tickers. Right-swipe
or the SW2 button goes back.

## Yahoo notes / gotchas

- **Endpoint:** one keyless request per symbol per poll —
  `https://query1.finance.yahoo.com/v8/finance/chart/<SYM>?range=1mo&interval=1d`
  (the script also falls back to `query2...` if `query1` rate-limits). No API
  key and no "crumb" are needed, but a **browser User-Agent is required** —
  Yahoo returns `403`/`429` to a bare agent, so the script sends a Mozilla UA.
- **Symbol format:** bare tickers for US equities (`AAPL`, `MSFT`, `TSLA`).
  UK/LSE uses `.L` (`VOD.L`); indices use `^` (`^GSPC`, `^FTSE`); FX pairs are
  `GBPUSD=X`. A bad symbol comes back as a Yahoo error and is logged + skipped.
- **Rate limits:** Yahoo throttles heavy or datacenter/VPN traffic with `429`.
  The script logs `fetch failed (HTTP 429 ...)` and **skips that symbol rather
  than pushing bad data**. If you hit it: raise `--interval`, use fewer tickers,
  and run from a normal home connection (cloud/VPN IPs get limited hardest). A
  handful of tickers at 60 s is well within limits from a home network.
- **Prices:** the big number is `meta.regularMarketPrice` (live). The percentage
  is change vs `meta.chartPreviousClose`. The sparkline is the last N daily
  closes (`indicators.quote[0].close`, nulls dropped) with the live price
  swapped in as the final point — so it's end-of-day granularity, not intraday
  ticks, but it ends "now".

## Serial protocol

See `../NOTES.md` for the full frame spec. In short, newline-terminated ASCII:

```
>>TICK <symbol> <price> <changePct> <nPoints> <p0> <p1> ... <p(n-1)>
>>CLEAR
```
