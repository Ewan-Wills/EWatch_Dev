"""ewbridge: the Mac side of the EWatch screen link, as one long-running app.

    bin/ewbridge up                    start it in the background + open the UI
    bin/ewbridge status                watches, what they show, fps/latency, input
    bin/ewbridge show app swarm        (launches it if needed)
    bin/ewbridge show window Safari    | show window 1234 | show monitor follow --zoom 2
    bin/ewbridge show screen | show test | show none
    bin/ewbridge show url http://127.0.0.1:8777/cam/0.mjpg   (MJPEG stream or JPEG URL)
    bin/ewbridge orient landscape-left | portrait | landscape-right
    bin/ewbridge snap out.png          read back what the watch shows
    bin/ewbridge events --follow       watch input as JSON lines
    bin/ewbridge sim "tap 120 140"     inject watch input (tests)
    bin/ewbridge console "log dump"    watch console (logs, reset reasons) without freeing the port
    bin/ewbridge release 60            free the USB port for `ewt flash`
    bin/ewbridge mcp                   MCP server on stdio (for LLM agents)
    bin/ewbridge down

Add --watch ID to target one watch when several are connected. --json prints
raw API output.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PIDFILE = os.path.join(ROOT, "runs", "ewbridge.pid")
LOGFILE = os.path.join(ROOT, "runs", "ewbridge.log")
BASE = "http://127.0.0.1:7480"


class ApiError(Exception):
    pass


def api(method, path, body=None, raw=False, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            out = r.read()
    except urllib.error.HTTPError as e:
        try:
            msg = json.loads(e.read()).get("error")
        except ValueError:
            msg = str(e)
        raise ApiError(msg) from None
    except (urllib.error.URLError, ConnectionError) as e:
        raise ApiError(f"ewbridge isn't running ({e}); start it with: bin/ewbridge up") from None
    return out if raw else json.loads(out)


def running():
    try:
        api("GET", "/api/help", timeout=1)
        return True
    except ApiError:
        return False


def q(watch):
    return f"?watch={watch}" if watch else ""


# ---------------------------------------------------------------------------
def cmd_serve(a):
    import faulthandler
    from .core import Bridge
    from .server import serve
    os.makedirs(os.path.join(ROOT, "runs"), exist_ok=True)
    # `kill -USR1 <pid>` writes every thread's stack to the log (stuck-thread debugging).
    faulthandler.register(signal.SIGUSR1, all_threads=True)
    with open(PIDFILE, "w") as f:
        f.write(str(os.getpid()))
    b = Bridge(orientation=a.orientation, quality=a.quality,
               ports=[a.port] if a.port else None, wifi=a.wifi or ())
    b.start()
    if a.open:
        subprocess.Popen(["open", BASE + "/"])
    try:
        serve(b)
    except KeyboardInterrupt:
        pass


def cmd_up(a):
    if running():
        print(f"ewbridge already running: {BASE}/")
    else:
        os.makedirs(os.path.join(ROOT, "runs"), exist_ok=True)
        args = [sys.executable, "-W", "ignore", "-m", "ewbridge", "serve", "--orientation", a.orientation]
        if a.port:
            args += ["--port", a.port]
        for w in a.wifi or ():
            args += ["--wifi", w]
        subprocess.Popen(args, cwd=ROOT, stdout=open(LOGFILE, "a"), stderr=subprocess.STDOUT,
                         start_new_session=True)
        for _ in range(50):
            if running():
                break
            time.sleep(0.1)
        else:
            sys.exit(f"ewbridge didn't start; see {LOGFILE}")
        print(f"ewbridge running: {BASE}/  (log: {LOGFILE})")
    if not a.no_ui:
        subprocess.Popen(["open", BASE + "/"])


def cmd_down(a):
    try:
        pid = int(open(PIDFILE).read())
        os.kill(pid, signal.SIGTERM)
        print("ewbridge stopped")
    except (OSError, ValueError):
        print("ewbridge wasn't running")


def cmd_status(a):
    s = api("GET", "/api/status")
    if a.json:
        return print(json.dumps(s, indent=1))
    if not s["watches"]:
        print("no watch found (plug one in over USB-C)")
    for w in s["watches"]:
        st = w["stream"]
        conn = "connected" if w["connected"] else (
            f"released ({w['released_for_s']} s)" if w["released_for_s"] else "not connected")
        print(f"watch {w['id']}  [{conn}{', Screenshare open' if w['app_open'] else ''}]  {w['port']}")
        print(f"  shows   {w['source']['desc'] or w['source']['spec'].get('kind')}   {w['orientation']} "
              f"{w['picture'][0]}x{w['picture'][1]}  q{w['quality']}")
        print(f"  stream  {st['fps']} fps  latency p50 {st['lat_p50']} / p95 {st['lat_p95']} ms  "
              f"{st['kbps']} KiB/s  frames {st['frames']}  bad {st['bad']}")
        i = w["input"]
        print(f"  input   touch {i['touch']}  button {'down' if i['button'] else 'up'}  imu {i['imu']}")
    apps = s["apps"]
    print("apps    connected: " + (", ".join(f"{c['name']} (on {', '.join(c['on']) or 'nothing'})"
                                             for c in apps["connected"]) or "none"))
    print("        available: " + ", ".join(x["name"] for x in apps["available"]))


def cmd_windows(a):
    ws = api("GET", "/api/windows")
    if a.json:
        return print(json.dumps(ws, indent=1))
    for w in ws:
        print(f"{w['id']:>7}  {w['owner'][:24]:24s} {w['title'][:50]:50s} {w['w']}x{w['h']}")


def wait_switch(watch, timeout=40):
    """Poll status until the source change finishes; return (ok, text)."""
    end = time.time() + timeout
    last_msg = None
    while time.time() < end:
        st = api("GET", "/api/status")
        ws = [w for w in st["watches"] if not watch or watch in (w["id"], w["mac"], w["port"])]
        if not ws:
            return False, "no watch"
        w = ws[0]
        sw = w.get("switch")
        if sw is None or sw["phase"] == "done":
            return True, w["source"]["desc"] or w["source"]["spec"].get("kind")
        if sw["phase"] == "error":
            return False, sw["error"]
        if sw.get("msg") and sw["msg"] != last_msg:
            print(f"  {sw['msg']}...", file=sys.stderr)
            last_msg = sw["msg"]
        if sw["phase"] == "first_frame" and not w["app_open"]:
            return True, (w["source"]["desc"] or "") + " (open Screenshare on the watch to see it)"
        time.sleep(0.15)
    return False, "timed out waiting for the switch"


def cmd_show(a):
    k = a.kind
    if k == "app":
        if not a.target:
            sys.exit("show app NAME")
        spec = {"kind": "app", "name": a.target}           # the bridge launches it if needed
    elif k == "window":
        if not a.target:
            sys.exit("show window ID|NAME (see: windows)")
        spec = {"kind": "window", "id": int(a.target)} if a.target.isdigit() else {"kind": "window", "name": a.target}
    elif k == "monitor":
        spec = {"kind": "monitor", "view": a.target or ("follow" if a.zoom else "fit")}
        if a.zoom:
            spec["zoom"] = a.zoom
    elif k == "screen":
        spec = {"kind": "screen", "display": int(a.target or 1)}
    elif k == "url":
        if not a.target:
            sys.exit("show url http://host:port/stream.mjpg")
        spec = {"kind": "url", "url": a.target}
    else:
        spec = {"kind": k}
    spec["watch"] = a.watch
    api("POST", "/api/source", spec)
    ok, text = wait_switch(a.watch)
    if a.json:
        return print(json.dumps({"ok": ok, "shows" if ok else "error": text}))
    if not ok:
        sys.exit(f"ewbridge: couldn't switch: {text}")
    print(f"watch shows: {text}")


def cmd_monitor_off(a):
    print(json.dumps(api("POST", "/api/monitor/close", {"watch": a.watch})))


def cmd_orient(a):
    r = api("POST", "/api/orientation", {"orientation": a.orientation, "watch": a.watch})
    print(f"{r['id']}: {r['orientation']} ({r['picture'][0]}x{r['picture'][1]})")


def cmd_apps(a):
    r = api("GET", "/api/apps")
    if a.json:
        return print(json.dumps(r, indent=1))
    for x in r["available"]:
        live = [c for c in r["connected"] if c["name"] == x["name"]]
        on = f"  [running, on {', '.join(live[0]['on']) or 'nothing'}]" if live else ""
        print(f"  {x['name']:12s} {x['about']}{on}")


def cmd_launch(a):
    print(json.dumps(api("POST", "/api/apps/launch", {"name": a.name, "watch": a.watch})))


def cmd_stop(a):
    print(json.dumps(api("POST", "/api/apps/stop", {"name": a.name})))


def cmd_input(a):
    print(json.dumps(api("GET", "/api/input" + q(a.watch))))


def cmd_events(a):
    if not a.follow:
        evs = api("GET", f"/api/events?since={a.since}" + (f"&watch={a.watch}" if a.watch else ""))
        for e in evs:
            print(json.dumps(e))
        return
    url = BASE + "/api/stream?imu=" + ("1" if a.imu else "0") + (f"&watch={a.watch}" if a.watch else "")
    try:
        with urllib.request.urlopen(url) as r:
            for line in r:
                line = line.decode().strip()
                if line.startswith("data: "):
                    print(line[6:], flush=True)
    except KeyboardInterrupt:
        pass


def cmd_snap(a):
    png = api("GET", "/api/snapshot.png" + q(a.watch), raw=True)
    open(a.out, "wb").write(png)
    print(a.out)


def cmd_preview(a):
    open(a.out, "wb").write(api("GET", "/api/preview.jpg" + q(a.watch), raw=True))
    print(a.out)


def cmd_haptic(a):
    print(json.dumps(api("POST", "/api/haptic", {"ms": a.ms, "strength": a.strength, "watch": a.watch})))


def cmd_sim(a):
    print(json.dumps(api("POST", "/api/sim", {"cmd": " ".join(a.cmd), "watch": a.watch})))


def cmd_console(a):
    r = api("POST", "/api/console", {"line": " ".join(a.line), "seconds": a.seconds, "watch": a.watch},
            timeout=a.seconds + 10)
    print("\n".join(r["lines"]))


def cmd_release(a):
    print(json.dumps(api("POST", "/api/release", {"seconds": a.seconds, "watch": a.watch})))


def cmd_mcp(a):
    from .mcp import main as mcp_main
    mcp_main()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="ewbridge", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--watch", help="watch id (see status); default: the first connected")
    ap.add_argument("--json", action="store_true", help="print raw API JSON")
    sp = ap.add_subparsers(dest="cmd", required=True)

    for name in ("serve", "up"):
        p = sp.add_parser(name, help="run the bridge (serve: foreground; up: background + open UI)")
        p.add_argument("--orientation", default="landscape-left",
                       choices=["portrait", "landscape-left", "landscape-right"])
        p.add_argument("--port", help="only this serial port (default: every /dev/cu.usbmodem*)")
        p.add_argument("--wifi", action="append", help="also a watch over WiFi (host[:port]), repeatable")
        if name == "serve":
            p.add_argument("--quality", type=int, default=60)
            p.add_argument("--open", action="store_true", help="open the UI in the browser")
            p.set_defaults(fn=cmd_serve)
        else:
            p.add_argument("--no-ui", action="store_true", help="don't open the browser")
            p.set_defaults(fn=cmd_up)
    sp.add_parser("down", help="stop the background bridge").set_defaults(fn=cmd_down)
    sp.add_parser("status", help="watches, sources, stats, input").set_defaults(fn=cmd_status)
    sp.add_parser("windows", help="windows you can show").set_defaults(fn=cmd_windows)
    p = sp.add_parser("show", help="choose what the watch shows")
    p.add_argument("kind", choices=["app", "window", "monitor", "screen", "url", "test", "none"])
    p.add_argument("target", nargs="?",
                   help="app name | window id or name | monitor view (fit|follow) | display | MJPEG/JPEG URL")
    p.add_argument("--zoom", type=float, help="monitor follow: magnification")
    p.set_defaults(fn=cmd_show)
    sp.add_parser("monitor-off", help="remove the virtual second monitor").set_defaults(fn=cmd_monitor_off)
    p = sp.add_parser("orient", help="portrait | landscape-left | landscape-right")
    p.add_argument("orientation", choices=["portrait", "landscape-left", "landscape-right"])
    p.set_defaults(fn=cmd_orient)
    sp.add_parser("apps", help="apps you can launch / running").set_defaults(fn=cmd_apps)
    p = sp.add_parser("launch", help="start apps/NAME.py (its own window)")
    p.add_argument("name")
    p.set_defaults(fn=cmd_launch)
    p = sp.add_parser("stop", help="stop a launched app")
    p.add_argument("name")
    p.set_defaults(fn=cmd_stop)
    sp.add_parser("input", help="latest input state (touch, button, imu)").set_defaults(fn=cmd_input)
    p = sp.add_parser("events", help="input events as JSON lines")
    p.add_argument("--since", type=int, default=0)
    p.add_argument("--follow", "-f", action="store_true", help="keep streaming (Ctrl-C to stop)")
    p.add_argument("--imu", action="store_true", help="with --follow: include 50 Hz IMU samples")
    p.set_defaults(fn=cmd_events)
    p = sp.add_parser("snap", help="read back what the watch shows -> PNG")
    p.add_argument("out", nargs="?", default="watch.png")
    p.set_defaults(fn=cmd_snap)
    p = sp.add_parser("preview", help="the last frame sent -> JPEG")
    p.add_argument("out", nargs="?", default="preview.jpg")
    p.set_defaults(fn=cmd_preview)
    p = sp.add_parser("haptic", help="buzz the watch")
    p.add_argument("ms", type=int, nargs="?", default=60)
    p.add_argument("strength", type=int, nargs="?", default=200)
    p.set_defaults(fn=cmd_haptic)
    p = sp.add_parser("sim", help='inject watch input for tests: "tap X Y", "btn short", "tilt 0.5 0 0.8"')
    p.add_argument("cmd", nargs="+")
    p.set_defaults(fn=cmd_sim)
    p = sp.add_parser("console", help='watch console command, e.g. "log dump" (Screenshare not open)')
    p.add_argument("line", nargs="+")
    p.add_argument("--seconds", type=float, default=3)
    p.set_defaults(fn=cmd_console)
    p = sp.add_parser("release", help="free the USB port for N seconds (ewt flash)")
    p.add_argument("seconds", type=float, nargs="?", default=60)
    p.set_defaults(fn=cmd_release)
    sp.add_parser("mcp", help="MCP server on stdio (needs the bridge running)").set_defaults(fn=cmd_mcp)
    a = ap.parse_args(argv)
    try:
        a.fn(a)
    except ApiError as e:
        sys.exit(f"ewbridge: {e}")
