"""HTTP API + web UI for the bridge (127.0.0.1 only).

Every call that acts on one watch takes ?watch=ID (or "watch" in the JSON
body); without it, the first connected watch. GET /api/help lists the calls.
"""
import json
import os
import queue
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .core import API_PORT, list_windows, log

HERE = os.path.dirname(os.path.abspath(__file__))

HELP = {
    "GET /api/status": "watches (connection, source, orientation, fps/latency, input), apps",
    "GET /api/windows": "on-screen windows you can show: [{id, owner, title, w, h}]",
    "GET /api/apps": "apps in apps/ you can launch, and connected app clients",
    "POST /api/source": 'what the watch shows. {"kind": "app", "name"} | {"kind": "window", "id"} | '
                        '{"kind": "window", "name"} | {"kind": "monitor", "view": "fit|follow", "zoom"} | '
                        '{"kind": "screen", "display"} | {"kind": "region", "rect": [x, y, w, h]} | '
                        '{"kind": "url", "url": "http://.../x.mjpg"} | '
                        '{"kind": "test"} | {"kind": "none"}',
    "POST /api/orientation": '{"orientation": "portrait|landscape-left|landscape-right"}',
    "POST /api/config": '{"quality": 1-95, "touch_mouse": bool}  touch_mouse: watch touches drive the Mac '
                        'pointer (window/monitor/screen sources; needs Accessibility permission)',
    "GET /api/input": "latest input state: touch (x, y) or null, button held, imu (g)",
    "GET /api/events?since=SEQ": "input events after SEQ (touch, button, gesture; not imu), newest last",
    "GET /api/stream": "server-sent events, everything incl. imu (50 Hz); ?imu=0 to leave it out",
    "GET /api/preview.jpg": "the last frame sent, as the viewer sees it",
    "GET /api/snapshot.png": "read back what the watch shows (its own copy of the panel)",
    "POST /api/haptic": '{"ms": 40, "strength": 200}  buzz the watch',
    "POST /api/sim": '{"cmd": "tap 120 140"}  inject input on the watch for tests (ewsim: tap X Y, '
                     'drag X1 Y1 X2 Y2 MS, swipe l|r|u|d, btn short|long, tilt GX GY GZ, tilt off)',
    "POST /api/apps/launch": '{"name": "swarm"}  run apps/NAME.py in its own Mac window, shown on the watch',
    "POST /api/apps/stop": '{"name": "swarm"}',
    "POST /api/release": '{"seconds": 60}  let go of the USB port (for `ewt flash`), then reconnect',
    "POST /api/watch/add": '{"wifi": "ewatch.local"}  add a watch over WiFi (Stream app)',
}


def make_handler(bridge):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        # -- helpers ------------------------------------------------------
        def _send(self, code, body, ctype="application/json"):
            if isinstance(body, (dict, list)):
                body = json.dumps(body, indent=1).encode()
            elif isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if not n:
                return {}
            return json.loads(self.rfile.read(n) or b"{}")

        def _watch(self, q, body=None):
            wid = (body or {}).get("watch") or (q.get("watch") or [None])[0]
            return bridge.watch(wid)

        # -- routes -------------------------------------------------------
        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                if u.path in ("/", "/index.html"):
                    with open(os.path.join(HERE, "ui.html"), "rb") as f:
                        return self._send(200, f.read(), "text/html; charset=utf-8")
                if u.path == "/api/help":
                    return self._send(200, HELP)
                if u.path == "/api/status":
                    return self._send(200, bridge.status())
                if u.path == "/api/windows":
                    return self._send(200, list_windows())
                if u.path == "/api/apps":
                    return self._send(200, bridge.status()["apps"])
                if u.path == "/api/input":
                    w = self._watch(q)
                    return self._send(200, dict(w.state, watch=w.id, last_seq=bridge.bus_seq))
                if u.path == "/api/events":
                    since = int((q.get("since") or ["0"])[0])
                    wid = (q.get("watch") or [None])[0]
                    return self._send(200, bridge.events_since(since, wid))
                if u.path == "/api/preview.jpg":
                    return self._send(200, self._watch(q).preview_jpeg(), "image/jpeg")
                if u.path == "/api/snapshot.png":
                    return self._send(200, self._watch(q).snapshot(), "image/png")
                if u.path == "/api/stream":
                    return self._sse(q)
                return self._send(404, {"error": f"no route {u.path}", "help": "/api/help"})
            except (KeyError, ValueError, TimeoutError, ConnectionError) as e:
                return self._send(400, {"error": str(e).strip("'\"")})

        def do_POST(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            try:
                b = self._body()
                if u.path == "/api/source":
                    spec = {k: v for k, v in b.items() if k != "watch"}
                    return self._send(200, self._watch(q, b).set_source(spec))
                if u.path == "/api/orientation":
                    w = self._watch(q, b)
                    w.set_orientation(b["orientation"])
                    return self._send(200, w.status())
                if u.path == "/api/config":
                    w = self._watch(q, b)
                    if "quality" in b:
                        w.quality = max(1, min(95, int(b["quality"])))
                    if "touch_mouse" in b:
                        w.touch_mouse = bool(b["touch_mouse"])
                    return self._send(200, w.status())
                if u.path == "/api/haptic":
                    return self._send(200, self._watch(q, b).haptic(b.get("ms", 40), b.get("strength", 200)))
                if u.path == "/api/sim":
                    return self._send(200, self._watch(q, b).sim(b["cmd"]))
                if u.path == "/api/apps/launch":
                    return self._send(200, bridge.launch_app(b["name"], b.get("watch")))
                if u.path == "/api/apps/stop":
                    return self._send(200, bridge.stop_app(b["name"]))
                if u.path == "/api/release":
                    return self._send(200, self._watch(q, b).release(float(b.get("seconds", 60))))
                if u.path == "/api/watch/add":
                    return self._send(200, bridge.add_watch(wifi=b["wifi"]).status())
                return self._send(404, {"error": f"no route {u.path}", "help": "/api/help"})
            except SystemExit as e:
                return self._send(400, {"error": str(e)})
            except (KeyError, ValueError, TimeoutError, ConnectionError, json.JSONDecodeError) as e:
                return self._send(400, {"error": str(e).strip("'\"")})

        def _sse(self, q):
            imu = (q.get("imu") or ["1"])[0] != "0"
            wid = (q.get("watch") or [None])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            sub = bridge.subscribe()
            try:
                last = time.time()
                while True:
                    try:
                        ev = sub.get(timeout=1.0)
                    except queue.Empty:
                        ev = None
                    if ev is not None and (imu or ev["type"] != "imu") and (wid is None or ev.get("watch") == wid):
                        self.wfile.write(f"data: {json.dumps(ev)}\n\n".encode())
                        self.wfile.flush()
                    elif time.time() - last > 10:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        last = time.time()
            except OSError:
                pass
            finally:
                bridge.unsubscribe(sub)

    return H


def serve(bridge, port=API_PORT):
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler(bridge))
    srv.daemon_threads = True
    log(f"ewbridge UI + API: http://127.0.0.1:{port}/")
    srv.serve_forever()
