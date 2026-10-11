"""The bridge: one long-lived owner of every watch link.

A Bridge holds any number of Watches (one per USB port found, plus WiFi ones
added by hand). Each Watch has its own source (what it shows), orientation,
stream loop, stats and input. Sources switch at any time without touching the
link: an app client, a window, the virtual monitor, the screen, a test pattern.
Watch input (touch, button, gestures, IMU) comes back on the same link and goes
to the app client on that watch, to the event bus (HTTP / SSE / MCP / CLI) and,
optionally, to the Mac's mouse.

Watches are named by the id the firmware reports (">>ID <MAC>"); until it has,
by port. Settings (source, orientation) are remembered per id, so a watch that
is re-plugged onto another port picks up where it was.

Event dicts (bus and app clients); all carry "watch" and "t", bus ones "seq":
  {"type": "touch", "phase": "down"|"move"|"up", "x", "y", "px", "py"}
       x, y: the watch picture (w x h); px, py: panel pixels 240x280
  {"type": "button", "phase": "down"|"up"|"short"}
  {"type": "gesture", "code", "name", "x", "y", "px", "py"}
  {"type": "imu", "ax", "ay", "az"}            g, watch axes (raw / 4096)
  {"type": "config", "w", "h", "orientation"}  app clients: picture size changed
  {"type": "focus", "active": bool}            app clients: on / off a watch
  {"type": "watch", "state": "connected"|"disconnected"}   bus only
"""
import collections
import glob
import io
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import screenshare as ss  # noqa: E402  (sources, packets, encoder)

from .client import APP_PORT  # noqa: E402

API_PORT = 7480
GESTURES = {1: "swipe_down", 2: "swipe_up", 3: "swipe_left", 4: "swipe_right",
            5: "tap", 11: "double_tap", 12: "long_press"}
TOUCH_PHASE = {"d": "down", "m": "move", "u": "up"}
BUTTON_PHASE = {"d": "down", "u": "up", "s": "short"}
ORIENTATIONS = ("portrait", "landscape-left", "landscape-right")

# screenshare.py's sources read the picture size from module globals; with
# several watches (maybe in different orientations) those are set per grab.
SS_LOCK = threading.Lock()


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def geometry(orientation):
    """(w, h, rot) of the picture for an orientation."""
    if orientation == "portrait":
        return ss.PW, ss.PH, 0
    return ss.PH, ss.PW, (1 if orientation == "landscape-left" else -1)


# ---------------------------------------------------------------------------
class AppClient:
    """A connected program drawing frames (apps/*.py via the runner, or any
    language speaking the client.py wire format)."""

    def __init__(self, bridge, sock):
        self.bridge, self.sock = bridge, sock
        self.name = self.title = None
        self.want_watch = None                 # hello {"watch": id}: preferred watch
        self.frame = None
        self.lock = threading.Lock()
        self.wlock = threading.Lock()
        self.alive = True

    def send(self, ev):
        body = json.dumps(ev).encode()
        try:
            with self.wlock:
                self.sock.sendall(struct.pack("<I", len(body) + 1) + b"J" + body)
        except OSError:
            self.alive = False

    def grab(self, w, h):
        with self.lock:
            f = self.frame
        if f is not None and f.shape[:2] != (h, w):    # orientation just changed
            f = ss.fit(Image.fromarray(f))
        return f

    def serve(self):
        buf = b""
        try:
            while True:
                d = self.sock.recv(1 << 20)
                if not d:
                    break
                buf += d
                while len(buf) >= 5:
                    n = struct.unpack("<I", buf[:4])[0]
                    if len(buf) < 4 + n:
                        break
                    kind, body = buf[4:5], buf[5:4 + n]
                    buf = buf[4 + n:]
                    self._handle(kind, body)
        except OSError:
            pass
        self.alive = False
        self.bridge.client_gone(self)

    def _handle(self, kind, body):
        if kind == b"H":
            hello = json.loads(body)
            self.name = hello.get("name") or f"client{id(self) % 1000}"
            self.title = hello.get("title") or self.name
            self.want_watch = hello.get("watch")
            self.bridge.client_hello(self)
        elif kind == b"F":
            w, h = struct.unpack("<HH", body[:4])
            a = np.frombuffer(body[4:], np.uint8)
            if a.size == w * h * 3:
                with self.lock:
                    self.frame = a.reshape(h, w, 3)
        elif kind == b"J":
            cmd = json.loads(body)
            for wt in self.bridge.watches_showing(self.name):
                if cmd.get("cmd") == "haptic":
                    wt.haptic(cmd.get("ms", 40), cmd.get("strength", 200))
            if cmd.get("cmd") == "focus":
                wt = self.bridge.watch(cmd.get("watch") or self.want_watch)
                wt.set_source({"kind": "app", "name": self.name})


class ClientSource:
    def __init__(self, client, watch):
        self.client, self.watch = client, watch
        self.desc = f"app {client.name}"

    def grab(self):
        return self.client.grab(self.watch.w, self.watch.h)


class WindowById:
    """One window by CGWindowID (what the UI's window picker hands over)."""

    def __init__(self, wid):
        import Quartz as Q
        self.Q, self.wid = Q, wid
        info = Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionIncludingWindow, wid) or []
        if not info:
            raise ValueError(f"no window {wid}")
        w = info[0]
        self.desc = f'window {w.get("kCGWindowOwnerName")} "{w.get("kCGWindowName") or ""}"'

    def grab(self):
        Q = self.Q
        im = Q.CGWindowListCreateImage(Q.CGRectNull, Q.kCGWindowListOptionIncludingWindow, self.wid,
                                       Q.kCGWindowImageBoundsIgnoreFraming | Q.kCGWindowImageNominalResolution)
        if im is None:
            raise RuntimeError("window closed")
        w, h, bpr = Q.CGImageGetWidth(im), Q.CGImageGetHeight(im), Q.CGImageGetBytesPerRow(im)
        if w == 0 or h == 0:
            return None
        data = Q.CGDataProviderCopyData(Q.CGImageGetDataProvider(im))
        return ss.fit(Image.frombuffer("RGBA", (w, h), bytes(data), "raw", "BGRA", bpr, 1))

    def view_to_screen(self, x, y):
        Q = self.Q
        info = Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionIncludingWindow, self.wid) or []
        if not info:
            return None
        b = info[0]["kCGWindowBounds"]
        return ss._view_to_rect(x, y, b["X"], b["Y"], b["Width"], b["Height"])


class UrlSource:
    """An MJPEG stream (multipart/x-mixed-replace) or a JPEG/PNG URL polled, letterboxed
    into the picture. A thread keeps the newest decoded frame; frames are found by
    JPEG start/end markers, so any MJPEG boundary works."""

    def __init__(self, url, w, h):
        self.url, self.w, self.h = url, w, h
        self.desc = f"url {url}"
        self.frame, self.err = None, None
        self.frame_t = 0.0
        self.lock = threading.Lock()
        self.alive = True
        self.placeholder = None
        threading.Thread(target=self._run, daemon=True).start()

    def _set(self, data):
        try:
            img = Image.open(io.BytesIO(data)).convert("RGB")
        except Exception:  # noqa: BLE001 - torn frame
            return
        with SS_LOCK:
            ss.VW, ss.VH = self.w, self.h
            f = ss.fit(img)
        with self.lock:
            self.frame, self.frame_t = f, time.time()

    def _no_signal(self):
        """Shown when the URL has given nothing for 2 s (server down / closed)."""
        if self.placeholder is None:
            from PIL import ImageDraw
            img = Image.new("RGB", (self.w, self.h), (14, 16, 22))
            d = ImageDraw.Draw(img)
            msg, sub = "No signal", self.url.split("//")[-1][:34]
            d.text(((self.w - d.textlength(msg)) / 2, self.h / 2 - 14), msg, fill=(230, 230, 235))
            d.text(((self.w - d.textlength(sub)) / 2, self.h / 2 + 4), sub, fill=(110, 118, 135))
            self.placeholder = np.asarray(img)
        return self.placeholder

    def _run(self):
        import urllib.request
        while self.alive:
            try:
                with urllib.request.urlopen(self.url, timeout=5) as r:
                    ctype = r.headers.get("Content-Type", "")
                    if "multipart" not in ctype:              # a single image: poll it
                        self._set(r.read())
                        time.sleep(1 / 15)
                        continue
                    buf = b""
                    while self.alive:
                        d = r.read1(65536) if hasattr(r, "read1") else r.read(4096)
                        if not d:
                            break
                        buf += d
                        end = buf.rfind(b"\xff\xd9")
                        start = buf.rfind(b"\xff\xd8", 0, end) if end > 0 else -1
                        if start >= 0:
                            self._set(buf[start:end + 2])     # the newest complete JPEG
                            buf = buf[end + 2:]
                        elif len(buf) > 4 << 20:
                            buf = b""
                self.err = None
            except Exception as e:  # noqa: BLE001 - server not up yet / restarted
                self.err = str(e)
                time.sleep(1.0)

    def grab(self):
        with self.lock:
            f, t = self.frame, self.frame_t
        if time.time() - t > 2.0 and (self.err or f is None):
            return self._no_signal()
        return f

    def close(self):
        self.alive = False


def list_windows():
    """On-screen normal windows, front to back: [{id, owner, title, w, h}]."""
    import Quartz as Q
    out = []
    for w in Q.CGWindowListCopyWindowInfo(
            Q.kCGWindowListOptionOnScreenOnly | Q.kCGWindowListExcludeDesktopElements, Q.kCGNullWindowID) or []:
        b = w.get("kCGWindowBounds")
        if w.get("kCGWindowLayer", 0) != 0 or not b or b["Width"] < 80 or b["Height"] < 60:
            continue
        out.append({"id": int(w["kCGWindowNumber"]), "owner": w.get("kCGWindowOwnerName") or "",
                    "title": w.get("kCGWindowName") or "", "w": int(b["Width"]), "h": int(b["Height"])})
    return out


# ---------------------------------------------------------------------------
class Watch:
    """One watch link: its source, orientation, stream loop, input, stats."""

    def __init__(self, bridge, port=None, wifi=None, orientation="landscape-left", quality=60):
        self.bridge, self.port, self.wifi = bridge, port, wifi
        self.id = wifi or os.path.basename(port)           # until the firmware says >>ID
        self.mac = None
        self.quality = quality
        self.link = None
        self.link_lock = threading.Lock()
        self.app_open = False
        self.src = None
        self.src_spec = {"kind": "none"}
        self.src_lock = threading.Lock()
        self.src_changed = True
        self.monitor = None
        self.touch_mouse = False
        self.state = {"touch": None, "button": False, "imu": None, "last_input": None}
        self.stats = {"fps": 0.0, "lat_p50": None, "lat_p95": None, "kbps": 0.0,
                      "dec_ms": None, "push_ms": None, "frames": 0, "bad": 0, "lost": 0}
        self.last_view = None
        self.snap_px, self.snap_ev = None, threading.Event()
        self.replies, self.reply_cond = {}, threading.Condition()
        self.shown = {}
        self.sim_seq = 0
        self.console_lines = collections.deque(maxlen=4000)   # (t, line): watch text that isn't ">>"
        self.keep_next = False
        self.keep_seq = None                     # seq of the last KEEP frame sent
        self.snapping = False                    # snapshot in progress: send nothing else
        self.gone_since = None
        self.last_reset, self.last_up = None, None   # from >>ID: why the watch last reset
        self.hold_until = 0.0                    # release(): leave the port alone until then
        self.stop = threading.Event()
        self.set_orientation(orientation)

    # ------------------------------------------------------------ config
    def set_orientation(self, o):
        if o not in ORIENTATIONS:
            raise ValueError("orientation: " + " | ".join(ORIENTATIONS))
        rebuild = None
        with self.src_lock:
            self.orientation = o
            self.w, self.h, self.rot = geometry(o)
            self.src_changed = True
            if self.monitor is not None:          # its shape follows the picture: rebuild
                rebuild = dict(self.src_spec)
                self.monitor = self.src = None
        if self.src_spec.get("kind") == "app":
            c = self.bridge.clients.get(self.src_spec.get("name"))
            if c:
                c.send(self._config_ev())
        if rebuild:
            self.set_source(rebuild)
        self.bridge.remember(self)

    def _config_ev(self):
        return {"type": "config", "w": self.w, "h": self.h, "orientation": self.orientation,
                "watch": self.id, "t": time.time()}

    def set_source(self, spec):
        """spec: {"kind": "none"|"test"|"screen"|"window"|"monitor"|"region"|"url"|"app", ...}"""
        try:
            with SS_LOCK:
                ss.VW, ss.VH, ss.ROT = self.w, self.h, self.rot
                out = self._set_source(spec)
        except SystemExit as e:                   # screenshare.py sources exit on errors
            raise ValueError(str(e)) from None
        self.bridge.remember(self)
        return out

    def _set_source(self, spec):
        kind = spec.get("kind")
        if kind == "none":
            src = None
        elif kind == "test":
            src = ss.TestSource()
        elif kind == "screen":
            import mss
            sct = mss.MSS() if hasattr(mss, "MSS") else mss.mss()
            mons = sct.monitors
            idx = int(spec.get("display", 1))
            src = ss.MssSource(mons[idx] if 0 < idx < len(mons) else mons[1])
        elif kind == "region":
            x, y, w, h = (int(v) for v in spec["rect"])
            src = ss.MssSource({"left": x, "top": y, "width": w, "height": h})
        elif kind == "window":
            src = WindowById(int(spec["id"])) if "id" in spec else ss.WindowSource(spec["name"])
        elif kind == "monitor":
            if any(wt.monitor is not None for wt in self.bridge.all_watches() if wt is not self):
                raise ValueError("another watch already shows the virtual monitor")
            view, zoom = spec.get("view", "fit"), float(spec.get("zoom", 2.0))
            if self.monitor is None:
                mw = ss.MonitorSource.MIN_W
                self.monitor = ss.MonitorSource(mw, round(mw * self.h / self.w), view, zoom, spec.get("side"))
            self.monitor.view, self.monitor.zoom = view, max(1.0, zoom)
            src = self.monitor
        elif kind == "url":
            src = UrlSource(spec["url"], self.w, self.h)
        elif kind == "app":
            c = self.bridge.clients.get(spec.get("name"))
            if c is None:
                raise ValueError(f"no app client {spec.get('name')!r} (connected: {list(self.bridge.clients)})")
            src = ClientSource(c, self)
        else:
            raise ValueError(f"unknown source kind {kind!r}")
        with self.src_lock:
            old, old_src = self.src_spec, self.src
            self.src, self.src_spec, self.src_changed = src, dict(spec), True
        if hasattr(old_src, "close") and old_src is not src:
            old_src.close()
        with self.src_lock:
            if kind != "monitor":
                self.monitor = None               # the virtual display goes when we leave it
        old_app = old.get("name") if old.get("kind") == "app" else None
        new_app = spec.get("name") if kind == "app" else None
        if old_app != new_app:
            if old_app in self.bridge.clients:
                self.bridge.clients[old_app].send({"type": "focus", "active": False, "watch": self.id,
                                                   "t": time.time()})
            if new_app in self.bridge.clients:
                c = self.bridge.clients[new_app]
                c.send(self._config_ev())
                c.send({"type": "focus", "active": True, "watch": self.id, "t": time.time()})
        log(f"[{self.id}] source ->", json.dumps(spec))
        return self.describe_source()

    def describe_source(self):
        return {"spec": self.src_spec, "desc": getattr(self.src, "desc", None) if self.src else None}

    # ------------------------------------------------------------ input
    def panel_to_view(self, px, py):
        if self.rot == 1:
            return self.w - 1 - py, px
        if self.rot == -1:
            return py, self.h - 1 - px
        return px, py

    def _input(self, ev):
        ev["t"], ev["watch"] = time.time(), self.id
        if ev["type"] != "imu":
            self.state["last_input"] = ev["t"]
        if ev["type"] == "touch":
            self.state["touch"] = None if ev["phase"] == "up" else (ev["x"], ev["y"])
        elif ev["type"] == "button" and ev["phase"] in ("down", "up"):
            self.state["button"] = ev["phase"] == "down"
        elif ev["type"] == "imu":
            self.state["imu"] = (round(ev["ax"], 3), round(ev["ay"], 3), round(ev["az"], 3))
        if self.src_spec.get("kind") == "app":
            c = self.bridge.clients.get(self.src_spec.get("name"))
            if c:
                c.send(ev)
        self.bridge.publish(ev)
        if self.touch_mouse and ev["type"] == "touch":
            self._drive_mouse(ev)

    def _drive_mouse(self, ev):
        """Touch moves/clicks the Mac pointer (window/monitor/screen sources).
        Needs Accessibility permission for the app running the bridge."""
        to_screen = getattr(self.src, "view_to_screen", None)
        pt = to_screen(ev["x"], ev["y"]) if to_screen else None
        if pt is None:
            return
        import Quartz as Q
        kind = {"down": Q.kCGEventLeftMouseDown, "move": Q.kCGEventLeftMouseDragged,
                "up": Q.kCGEventLeftMouseUp}[ev["phase"]]
        Q.CGEventPost(Q.kCGHIDEventTap, Q.CGEventCreateMouseEvent(None, kind, pt, Q.kCGMouseButtonLeft))

    # ------------------------------------------------------------ link
    def _open_link(self):
        if self.wifi:
            host, _, port = self.wifi.partition(":")
            return ss.TcpLink(host, int(port or 7878))
        if not os.path.exists(self.port):
            return None
        return ss.UsbLink(self.port)

    def write(self, b):
        with self.link_lock:
            if self.link is None:
                raise ConnectionError("watch not connected")
            self.link.write(b)

    def _reader(self, link):
        buf = bytearray()
        try:
            while self.link is link:
                d = link.read(65536)
                if d:
                    buf += d
                    buf = self._parse(buf)
        except Exception as e:  # noqa: BLE001 - port gone
            log(f"[{self.id}] link read ended:", e)
        if self.link is link:
            self._drop_link()

    def _parse(self, buf):
        while True:
            i = buf.find(b"\n")
            if i < 0:
                return buf
            line = bytes(buf[:i]).decode("utf-8", "replace").strip()
            if line.startswith(">>SNAP "):
                n = int(line.split()[1])
                if len(buf) < i + 1 + n:
                    return buf
                self.snap_px = bytes(buf[i + 1:i + 1 + n])
                del buf[:i + 1 + n]
                self.snap_ev.set()
                continue
            del buf[:i + 1]
            self._line(line, time.perf_counter())

    def _line(self, line, now):
        p = line.split()
        if not p:
            return
        try:
            k = p[0]
            if k == ">>R":
                with self.reply_cond:
                    self.replies["R"] = int(p[1])
                    self.reply_cond.notify_all()
            elif k == ">>D":
                seq = int(p[1])
                with self.reply_cond:
                    self.replies["R"] = seq
                    self.shown[seq] = (now, int(p[2]), int(p[3]))
                    self.reply_cond.notify_all()
            elif k == ">>P":
                self.app_open = True
                with self.reply_cond:
                    self.replies["P"] = int(p[1])
                    self.reply_cond.notify_all()
            elif k == ">>ID":
                info = dict(x.split("=", 1) for x in p[2:] if "=" in x)
                if info:
                    up = int(info.get("up", 0))
                    if up < (self.last_up or 1 << 62):          # first contact, or it rebooted
                        log(f"[{p[1]}] watch boot: reset={info.get('reset')} up={up / 1000:.1f}s")
                    self.last_reset, self.last_up = info.get("reset"), up
                self.bridge.rename(self, p[1])
            elif k == ">>READY":
                self.app_open = True
            elif k == ">>BYE":
                self.app_open = False
            elif k == ">>BAD":
                self.stats["bad"] += 1
                log(f"[{self.id}] watch:", line)
            elif k == ">>T":
                px, py = int(p[2]), int(p[3])
                if not (0 <= px < ss.PW and 0 <= py < ss.PH):      # chip's "no position" (4095)
                    return
                x, y = self.panel_to_view(px, py)
                self._input({"type": "touch", "phase": TOUCH_PHASE.get(p[1], p[1]),
                             "x": x, "y": y, "px": px, "py": py})
            elif k == ">>B":
                self._input({"type": "button", "phase": BUTTON_PHASE.get(p[1], p[1])})
            elif k == ">>G":
                code, px, py = int(p[1]), int(p[2]), int(p[3])
                x, y = self.panel_to_view(px, py)
                self._input({"type": "gesture", "code": code, "name": GESTURES.get(code, str(code)),
                             "x": x, "y": y, "px": px, "py": py})
            elif k == ">>I":
                self._input({"type": "imu", "ax": int(p[1]) / 4096, "ay": int(p[2]) / 4096,
                             "az": int(p[3]) / 4096})
            elif k.startswith("S:"):
                with self.reply_cond:
                    self.replies["S:" + k[2:]] = line
                    self.reply_cond.notify_all()
            elif not k.startswith(">>"):
                self.console_lines.append((time.time(), line))
                if "boot reset=" in line or "Guru Meditation" in line or "Backtrace" in line \
                        or "=== EWatch boot ===" in line:
                    log(f"[{self.id}] watch: {line}")          # resets show up in our log
        except (ValueError, IndexError):
            pass

    def _drop_link(self):
        with self.link_lock:
            link, self.link = self.link, None
            self.app_open = False
        if link:
            try:
                link.close()
            except Exception:  # noqa: BLE001
                pass
            log(f"[{self.id}] watch link closed")
            self.bridge.publish({"type": "watch", "state": "disconnected", "watch": self.id,
                                 "t": time.time()})

    def release(self, seconds=60):
        """Let go of the port for a while (e.g. for `ewt flash`), then reconnect."""
        self.hold_until = time.time() + seconds
        self._drop_link()
        return {"watch": self.id, "released_for_s": seconds}

    def _ensure_link(self):
        if self.link is not None:
            return True
        if time.time() < self.hold_until:
            return False
        try:
            link = self._open_link()
        except Exception as e:  # noqa: BLE001 - busy (another program) / gone; retry later
            log(f"[{self.id}] link open failed:", str(e).splitlines()[0])
            link = None
        if link is None:
            if self.gone_since is None:
                self.gone_since = time.time()
            return False
        self.gone_since = None
        with self.link_lock:
            self.link = link
        self.shown = {}
        threading.Thread(target=self._reader, args=(link,), daemon=True).start()
        self.write(ss.packet(ss.T_PING, 0))                  # hello: the watch answers >>P + >>ID
        log(f"[{self.id}] watch link open: {link.name}")
        self.bridge.publish({"type": "watch", "state": "connected", "watch": self.id, "t": time.time()})
        return True

    def _wait(self, pred, timeout):
        end = time.perf_counter() + timeout
        with self.reply_cond:
            while not pred():
                left = end - time.perf_counter()
                if left <= 0 or self.link is None:
                    return False
                self.reply_cond.wait(left)
            return True

    # ------------------------------------------------------------ commands
    def haptic(self, ms=40, strength=200):
        self.write(ss.packet(4, 0, int(ms), int(strength)))
        return {"ok": True, "watch": self.id}

    def sim(self, cmd, timeout=5.0):
        """ewsim command on the watch (tap X Y, drag X1 Y1 X2 Y2 MS, btn short,
        tilt GX GY GZ, ...), sent between frames: the watch acts as if a finger,
        the button or a tilt did it. For tests."""
        parts = cmd.split()
        if parts and parts[0] == "tilt" and len(parts) == 4:   # g -> counts (4096 = 1 g), like ewt
            cmd = "tilt " + " ".join(str(round(float(v) * 4096)) for v in parts[1:])
        self.sim_seq += 1
        s = str(self.sim_seq)
        self.write(f"\nsim {s} {cmd}\n".encode())
        ok = self._wait(lambda: ("S:" + s) in self.replies, timeout)
        return {"ok": ok, "reply": self.replies.get("S:" + s), "watch": self.id}

    def console(self, line, seconds=2.0):
        """Send a line to the watch's ewlog console (`log dump`, `pwr stats`, `help`)
        and return the text it prints within `seconds`. Works when the Screenshare
        app is NOT open (it owns the console while open; use sim there)."""
        t0 = time.time()
        self.write(f"\n{line}\n".encode())
        time.sleep(seconds)
        return {"watch": self.id, "lines": [l for t, l in list(self.console_lines) if t >= t0]}

    def snapshot(self, timeout=5.0):
        """What the watch shows (its own copy of the pixels it sent to the panel),
        PNG, picture orientation."""
        # 1. the stream loop resends the current frame whole, flagged KEEP (the
        #    watch only copies KEEP frames into its snapshot buffer);
        # 2. wait until that frame is on the panel;
        # 3. pause the stream while the 134 KB of pixels come back: any other
        #    reply line (>>R for a new frame) would land in the middle of them.
        deadline = time.time() + timeout
        self.keep_seq, self.keep_next = None, True
        while self.keep_next and self.src is not None and time.time() < deadline:
            time.sleep(0.01)
        self.snapping = True
        try:
            if self.keep_seq is not None:
                self._wait(lambda: self.keep_seq in self.shown, max(0.1, deadline - time.time()))
            time.sleep(0.05)                      # replies already on their way
            self.snap_ev.clear()
            self.write(ss.packet(ss.T_SNAP, 0))
            if not self.snap_ev.wait(max(0.5, deadline - time.time())):
                raise TimeoutError("no >>SNAP from the watch (is Screenshare open?)")
        finally:
            self.snapping = False
        px = np.frombuffer(self.snap_px, dtype="<u2").reshape(ss.PH, ss.PW)
        rgb = np.dstack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63,
                         (px & 31) * 255 // 31]).astype(np.uint8)
        if self.rot:
            rgb = np.rot90(rgb, -self.rot)
        b = io.BytesIO()
        Image.fromarray(np.ascontiguousarray(rgb)).save(b, "PNG")
        return b.getvalue()

    def preview_jpeg(self):
        f = self.last_view if self.last_view is not None else np.zeros((self.h, self.w, 3), np.uint8)
        b = io.BytesIO()
        Image.fromarray(f).save(b, "JPEG", quality=85)
        return b.getvalue()

    def status(self):
        if getattr(self, "_win", None) is not None:
            self._update_stats(self._win, self._bytes_win)      # decays to 0 when idle
        return {"id": self.id, "mac": self.mac, "port": self.wifi or self.port,
                "last_reset": self.last_reset,
                "connected": self.link is not None, "app_open": self.app_open,
                "released_for_s": max(0, round(self.hold_until - time.time())),
                "source": self.describe_source(), "orientation": self.orientation,
                "picture": [self.w, self.h], "quality": self.quality, "touch_mouse": self.touch_mouse,
                "stream": self.stats,
                "input": {"touch": self.state["touch"], "button": self.state["button"],
                          "imu": self.state["imu"]}}

    # ------------------------------------------------------------ stream loop
    def run(self):
        """Capture -> encode -> send until stop. Same pacing as screenshare.py:
        at most two frames not yet on the panel, capture timed to land as the
        watch frees up, only the changed rectangle sent."""
        seq, prev, sent = 0, None, {}
        pc_est, disp_est, tx_est = 0.015, 0.025, 0.010
        last_ping = 0.0
        win = self._win = collections.deque(maxlen=120)
        bytes_win = self._bytes_win = collections.deque(maxlen=240)
        seen = set()
        while not self.stop.is_set():
            if not self._ensure_link():
                time.sleep(1.0)
                continue
            try:
                with self.src_lock:
                    src, changed, keep = self.src, self.src_changed, self.keep_next
                    self.src_changed = False
                if changed:
                    prev = None
                if self.snapping:
                    time.sleep(0.01)
                    continue
                if src is None or not self.app_open:
                    if time.perf_counter() - last_ping > 1.0:          # also finds the app opening
                        self.write(ss.packet(ss.T_PING, 0xFFFF if self.app_open else 0))
                        last_ping = time.perf_counter()
                    self.keep_next = False
                    time.sleep(0.05)
                    continue
                if seq >= 2:
                    self._wait(lambda: ((seq - 2) & 0xFFFF) in self.shown, 0.1)
                    d_prev = self.shown.get((seq - 2) & 0xFFFF)
                    if d_prev:
                        d = d_prev[0] + disp_est - pc_est - tx_est - 0.002 - time.perf_counter()
                        if d > 0:
                            time.sleep(d)
                t_cap = time.perf_counter()
                try:
                    with SS_LOCK:
                        ss.VW, ss.VH, ss.ROT = self.w, self.h, self.rot
                        view = src.grab()
                except Exception as e:  # noqa: BLE001 - window closed etc.
                    log(f"[{self.id}] source error:", e)
                    self.set_source({"kind": "none"})
                    continue
                if view is None:
                    time.sleep(0.01)
                    continue
                frame = np.ascontiguousarray(np.rot90(view, self.rot)) if self.rot else view
                rect = (0, 0, ss.PW, ss.PH) if keep else ss.changed_rect(prev, frame, 6)
                if rect is None:
                    if time.perf_counter() - last_ping > 1.0:
                        self.write(ss.packet(ss.T_PING, 0xFFFF))
                        last_ping = time.perf_counter()
                    time.sleep(0.004)
                    continue
                pkt = ss.packet(ss.T_JPEG, seq, *rect, payload=ss.encode(frame, rect, self.quality),
                                flags=ss.F_KEEP if keep else 0)
                self.write(pkt)
                t_sent = time.perf_counter()
                if keep:
                    self.keep_seq = seq & 0xFFFF
                    self.keep_next = False
                sent[seq & 0xFFFF] = t_cap
                pc_est = 0.8 * pc_est + 0.2 * (t_sent - t_cap)
                prev, self.last_view = frame, view
                bytes_win.append((t_sent, len(pkt)))
                seq += 1
                if self._wait(lambda: self.replies.get("R") == (seq - 1) & 0xFFFF, 0.08):
                    tx_est = 0.8 * tx_est + 0.2 * (time.perf_counter() - t_sent)
                else:
                    self.stats["lost"] += 1
                for k, (t_d, dec, push) in list(self.shown.items()):
                    if k in seen or k not in sent:
                        continue
                    seen.add(k)
                    win.append((t_d, 1000 * (t_d - sent.pop(k)), dec, push))
                    disp_est = 0.7 * disp_est + 0.3 * ((dec + push) / 1e6)
                    self.stats["frames"] += 1
                if len(self.shown) > 400:
                    for k in list(self.shown)[:200]:
                        self.shown.pop(k, None)
                        seen.discard(k)
                if len(sent) > 400:
                    for k in list(sent)[:200]:
                        sent.pop(k, None)
                self._update_stats(win, bytes_win)
            except (ConnectionError, OSError) as e:
                log(f"[{self.id}] link error:", e)
                self._drop_link()
                time.sleep(0.5)
        self._drop_link()

    def _update_stats(self, win, bytes_win):
        now = time.perf_counter()
        recent = [w for w in list(win) if now - w[0] < 2.0]
        self.stats["fps"] = round(len(recent) / 2.0, 1)
        if recent:
            lat = sorted(w[1] for w in recent)
            self.stats["lat_p50"] = round(lat[len(lat) // 2], 1)
            self.stats["lat_p95"] = round(lat[min(len(lat) - 1, int(0.95 * len(lat)))], 1)
            self.stats["dec_ms"] = round(recent[-1][2] / 1000, 1)
            self.stats["push_ms"] = round(recent[-1][3] / 1000, 1)
        self.stats["kbps"] = round(sum(n for t, n in list(bytes_win) if now - t < 2.0) / 2.0 / 1024, 1)


# ---------------------------------------------------------------------------
class Bridge:
    """Every watch, every app client, the event bus."""

    def __init__(self, orientation="landscape-left", quality=60, ports=None, wifi=()):
        self.default_orientation, self.quality = orientation, quality
        self.fixed_ports = ports                    # None = find /dev/cu.usbmodem* by itself
        self.watches = {}                           # id -> Watch
        self.wlock = threading.RLock()
        self.saved = self._load_saved()             # id -> {"source", "orientation"} (remembered)
        self.clients = {}                           # name -> AppClient
        self.procs = {}                             # name -> Popen (launched by us)
        self.bus_seq = 0
        self.ring = collections.deque(maxlen=500)
        self.subs, self.sub_lock = [], threading.Lock()
        self.stop = threading.Event()
        for w in wifi:
            self.add_watch(wifi=w)

    # ------------------------------------------------------------ watches
    def all_watches(self):
        with self.wlock:
            return list(self.watches.values())

    def watch(self, wid=None):
        """A watch by id/port (None: the first connected one)."""
        with self.wlock:
            if wid:
                if wid in self.watches:
                    return self.watches[wid]
                for w in self.watches.values():
                    if wid in (w.mac, w.port, w.wifi) or (w.port and os.path.basename(w.port) == wid):
                        return w
                raise KeyError(f"no watch {wid!r} (have: {list(self.watches)})")
            live = [w for w in self.watches.values() if w.link is not None] or list(self.watches.values())
            if not live:
                raise KeyError("no watch connected")
            return live[0]

    def add_watch(self, port=None, wifi=None):
        w = Watch(self, port=port, wifi=wifi, orientation=self.default_orientation, quality=self.quality)
        with self.wlock:
            self.watches[w.id] = w
        threading.Thread(target=w.run, daemon=True, name=f"watch-{w.id}").start()
        log("watch added:", w.id)
        return w

    def rename(self, w, mac):
        """The firmware told us its id: key the watch by it and restore its settings."""
        if w.mac == mac:
            return
        with self.wlock:
            for k, v in list(self.watches.items()):
                if v is w:
                    del self.watches[k]
            old = self.watches.get(mac)
            if old is not None and old is not w:          # the same watch on its old port: retire it
                old.stop.set()
            w.mac, w.id = mac, mac
            self.watches[mac] = w
        log(f"watch {w.port or w.wifi} is {mac}")
        saved = self.saved.get(mac)
        if saved:
            try:
                if saved.get("orientation") and saved["orientation"] != w.orientation:
                    w.set_orientation(saved["orientation"])
                spec = saved.get("source") or {"kind": "none"}
                if spec.get("kind") not in ("app", "window", "url") and w.src_spec.get("kind") == "none":
                    w.set_source(spec)                  # apps/windows don't survive a restart
            except Exception as e:  # noqa: BLE001
                log(f"[{mac}] restore failed:", e)

    def remember(self, w):
        if not w.mac:
            return
        self.saved[w.mac] = {"source": w.src_spec, "orientation": w.orientation}
        try:
            with open(self._saved_path(), "w") as f:
                json.dump(self.saved, f, indent=1)
        except OSError:
            pass

    def _saved_path(self):
        return os.path.join(ROOT, "runs", "ewbridge-watches.json")

    def _load_saved(self):
        try:
            with open(self._saved_path()) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def scan(self):
        """Find watches on USB: one Watch per /dev/cu.usbmodem*; drop ones gone for 10 s."""
        while not self.stop.is_set():
            ports = self.fixed_ports or sorted(glob.glob("/dev/cu.usbmodem*"))
            with self.wlock:
                have = {w.port for w in self.watches.values() if w.port}
                for p in ports:
                    if p not in have:
                        self.add_watch(port=p)
                for k, w in list(self.watches.items()):
                    if w.port and w.gone_since and time.time() - w.gone_since > 10 \
                            and not os.path.exists(w.port) and time.time() > w.hold_until:
                        w.stop.set()
                        del self.watches[k]
                        log("watch removed:", k)
                    elif w.stop.is_set():
                        del self.watches[k]
            time.sleep(2.0)

    def watches_showing(self, app_name):
        return [w for w in self.all_watches()
                if w.src_spec.get("kind") == "app" and w.src_spec.get("name") == app_name]

    # ------------------------------------------------------------ app clients
    def client_hello(self, c):
        old = self.clients.get(c.name)
        if old and old is not c:
            old.alive = False
        self.clients[c.name] = c
        log("app connected:", c.name)
        try:
            w = self.watch(c.want_watch)
        except KeyError:
            c.send({"type": "config", "w": 280, "h": 240, "orientation": self.default_orientation,
                    "watch": None, "t": time.time()})
            return
        c.send(w._config_ev())
        # A new app takes its watch unless something else was picked on purpose.
        if w.src_spec.get("kind") in ("none", "app", "test"):
            w.set_source({"kind": "app", "name": c.name})
        else:
            c.send({"type": "focus", "active": False, "watch": w.id, "t": time.time()})

    def client_gone(self, c):
        if c.name and self.clients.get(c.name) is c:
            del self.clients[c.name]
            log("app disconnected:", c.name)
            for w in self.watches_showing(c.name):
                nxt = next(iter(self.clients), None)
                w.set_source({"kind": "app", "name": nxt} if nxt else {"kind": "none"})

    def launch_app(self, name, watch=None):
        """Start apps/<name>.py as a client process (its own Mac window)."""
        import apps
        apps.load(name)                               # fails early on a bad name
        p = self.procs.get(name)
        if p and p.poll() is None:
            return {"name": name, "pid": p.pid, "already_running": True}
        cmd = [sys.executable, "-W", "ignore", "-m", "apps.runner", name]
        if watch:
            cmd += ["--watch", watch]
        logf = open(os.path.join(ROOT, "runs", f"app-{name}.log"), "w")
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT)
        self.procs[name] = p
        log("launched app", name, "pid", p.pid)
        return {"name": name, "pid": p.pid}

    def stop_app(self, name):
        p = self.procs.pop(name, None)
        if p and p.poll() is None:
            p.terminate()
        c = self.clients.get(name)
        if c:
            try:
                c.sock.close()
            except OSError:
                pass
        return {"name": name, "stopped": bool(p or c)}

    def app_server(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", APP_PORT))
        srv.listen(8)
        while not self.stop.is_set():
            s, _ = srv.accept()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=AppClient(self, s).serve, daemon=True).start()

    # ------------------------------------------------------------ event bus
    def publish(self, ev):
        with self.sub_lock:
            self.bus_seq += 1
            ev = dict(ev, seq=self.bus_seq)
            if ev["type"] != "imu":
                self.ring.append(ev)
            for q in self.subs:
                try:
                    q.put_nowait(ev)
                except queue.Full:
                    pass

    def subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self.sub_lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.sub_lock:
            if q in self.subs:
                self.subs.remove(q)

    def events_since(self, seq=0, watch=None):
        with self.sub_lock:
            return [e for e in self.ring if e["seq"] > seq and (watch is None or e.get("watch") == watch)]

    def status(self):
        import apps
        return {"watches": [w.status() for w in self.all_watches()],
                "apps": {"connected": [{"name": c.name, "title": c.title,
                                        "on": [w.id for w in self.watches_showing(c.name)]}
                                       for c in list(self.clients.values())],
                         "launched": [n for n, p in self.procs.items() if p.poll() is None],
                         "available": [{"name": n, "about": d} for n, d in apps.available()]},
                "last_seq": self.bus_seq}

    def start(self):
        threading.Thread(target=self.app_server, daemon=True, name="apps").start()
        if self.fixed_ports is not False:
            threading.Thread(target=self.scan, daemon=True, name="scan").start()
