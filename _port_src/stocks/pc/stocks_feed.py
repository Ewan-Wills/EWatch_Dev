#!/usr/bin/env python3
"""stocks_feed.py — EWatch 'stocks' companion.

Polls Yahoo Finance's keyless v8 chart endpoint for a list of tickers and
pushes each one to the watch over the USB-CDC serial link as a newline-
terminated ASCII frame:

    >>TICK <symbol> <price> <changePct> <nPoints> <p0> <p1> ... <p(n-1)>\\n

It sends one >>CLEAR at startup so the watch's ticker list matches this run.

Data source (one request per symbol, per poll):
    https://query1.finance.yahoo.com/v8/finance/chart/<SYM>?range=1mo&interval=1d

From the JSON we read chart.result[0]:
    meta.regularMarketPrice                      -> live price
    meta.chartPreviousClose / meta.previousClose -> change% vs prev close
    indicators.quote[0].close                    -> daily closes (sparkline)

No API key, no crumb. A Mozilla User-Agent is sent (Yahoo 403s a bare agent).
Only dependency is pyserial (everything else is stdlib). See README.md.
"""
import argparse
import glob
import json
import sys
import time
import urllib.request
import urllib.error
import urllib.parse

# pyserial is only needed when actually sending to the watch. Import it lazily
# so --no-serial (useful for testing the data feed without hardware) works
# without the dependency installed.
try:
    import serial  # pyserial
    from serial.tools import list_ports
    _HAVE_SERIAL = True
except ImportError:
    _HAVE_SERIAL = False

YAHOO_HOSTS = ("query1.finance.yahoo.com", "query2.finance.yahoo.com")
YAHOO_CHART = ("https://{host}/v8/finance/chart/{sym}?range={rng}&interval=1d")
HTTP_TIMEOUT = 15
# Yahoo rejects non-browser agents with 403/429; a normal UA is required.
USER_AGENT = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
              "AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/122.0 Safari/537.36")


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class FetchError(Exception):
    """Yahoo returned non-200, HTML, or JSON we can't use."""


def fetch_chart_json(sym, rng):
    # Try each Yahoo host; query2 often answers when query1 rate-limits (429).
    # A 4xx that isn't a rate-limit (e.g. 404 bad symbol) fails fast.
    last = None
    for host in YAHOO_HOSTS:
        url = YAHOO_CHART.format(host=host, sym=urllib.parse.quote(sym), rng=rng)
        req = urllib.request.Request(url, headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/json",
        })
        try:
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
                status = getattr(r, "status", 200)
                raw = r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            body = ""
            try:
                body = e.read().decode("utf-8", "replace")[:200]
            except Exception:
                pass
            last = FetchError(f"HTTP {e.code} {body}".strip())
            # Retry the other host only on rate-limit / server errors.
            if e.code in (429, 500, 502, 503, 504):
                continue
            raise last
        except (urllib.error.URLError, OSError) as e:
            last = FetchError(str(e))
            continue
        if status != 200:
            last = FetchError(f"HTTP {status}")
            continue
        head = raw.lstrip()[:1].lower()
        if head == "<":
            last = FetchError("HTML response (not JSON) — endpoint blocked/changed")
            continue
        try:
            return json.loads(raw)
        except ValueError as e:
            raise FetchError(f"bad JSON: {e}")
    raise last or FetchError("no response")


def fetch_symbol(sym, want_points):
    """Return (price, change_pct, spark_list) for `sym`, or raise FetchError.

    `spark_list` is up to `want_points` recent daily closes, oldest first, with
    the final point swapped for the live price so the line ends 'now'."""
    # range covers roughly want_points trading days plus slack for holidays.
    rng = "1mo" if want_points <= 20 else ("3mo" if want_points <= 60 else "6mo")
    data = fetch_chart_json(sym, rng)

    chart = (data or {}).get("chart") or {}
    if chart.get("error"):
        raise FetchError(str(chart["error"]))
    results = chart.get("result") or []
    if not results:
        raise FetchError("empty result")
    res = results[0]
    meta = res.get("meta") or {}

    price = meta.get("regularMarketPrice")
    prev = meta.get("chartPreviousClose")
    if prev is None:
        prev = meta.get("previousClose")

    # Daily closes for the sparkline; drop nulls (Yahoo pads gaps with null).
    closes = []
    try:
        closes = [c for c in res["indicators"]["quote"][0]["close"] if c is not None]
    except (KeyError, IndexError, TypeError):
        closes = []

    if price is None:
        if not closes:
            raise FetchError("no price and no closes")
        price = closes[-1]
    price = float(price)

    if prev is None:
        prev = closes[-2] if len(closes) >= 2 else price
    prev = float(prev)
    change_pct = ((price - prev) / prev * 100.0) if prev else 0.0

    spark = closes[-want_points:] if closes else [price]
    # Make the last point the current live price.
    spark = (spark[:-1] + [price]) if spark else [price]
    return price, change_pct, spark


def build_frame(display_sym, price, change_pct, spark):
    pts = " ".join(f"{p:.4g}" for p in spark)
    return f">>TICK {display_sym} {price:.4f} {change_pct:.4f} {len(spark)} {pts}\n"


def autodetect_port():
    # macOS native-USB CDC shows up as /dev/cu.usbmodem*
    for pat in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    ports = list(list_ports.comports())
    if ports:
        return ports[0].device
    return None


def main():
    ap = argparse.ArgumentParser(description="EWatch stocks serial feed (Yahoo v8).")
    ap.add_argument("--port", help="serial port (default: auto-detect usbmodem)")
    ap.add_argument("--tickers", default="AAPL,MSFT,GOOGL,TSLA,NVDA",
                    help="comma-separated symbols, e.g. AAPL,MSFT,VOD.L,^GSPC")
    ap.add_argument("--interval", type=float, default=60.0,
                    help="seconds between full polls (default 60)")
    ap.add_argument("--points", type=int, default=20,
                    help="sparkline length (daily closes, default 20, max 32)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by native USB CDC but set anyway")
    ap.add_argument("--once", action="store_true",
                    help="poll once and exit (for testing)")
    ap.add_argument("--no-serial", action="store_true",
                    help="print frames to stdout instead of opening the port")
    args = ap.parse_args()

    points = max(2, min(32, args.points))
    symbols = [s.strip() for s in args.tickers.split(",") if s.strip()]
    if not symbols:
        sys.exit("no tickers given")

    ser = None
    if not args.no_serial:
        if not _HAVE_SERIAL:
            sys.exit("pyserial is required to talk to the watch:  pip install -r requirements.txt"
                     "   (or use --no-serial to just print frames)")
        port = args.port or autodetect_port()
        if not port:
            sys.exit("no serial port found; pass --port /dev/cu.usbmodemXXXX")
        log(f"Opening {port} @ {args.baud}")
        try:
            ser = serial.Serial(port, args.baud, timeout=1)
        except serial.SerialException as e:
            sys.exit(f"could not open {port}: {e}")
        time.sleep(1.0)   # let the CDC link settle

    def send(frame):
        if ser is not None:
            ser.write(frame.encode("ascii", "ignore"))
            ser.flush()
        else:
            sys.stdout.write(frame)
            sys.stdout.flush()

    send(">>CLEAR\n")

    try:
        while True:
            t0 = time.time()
            for disp in symbols:
                sym = disp.upper()
                try:
                    price, change_pct, spark = fetch_symbol(sym, points)
                except FetchError as e:
                    log(f"  {disp}: fetch failed ({e}); skipping")
                    continue
                send(build_frame(sym, price, change_pct, spark))
                log(f"  {disp}: {price:.2f} ({change_pct:+.2f}%)  "
                    f"spark[{len(spark)}]")
            if args.once:
                break
            elapsed = time.time() - t0
            time.sleep(max(1.0, args.interval - elapsed))
    except KeyboardInterrupt:
        log("stopping")
    finally:
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()
