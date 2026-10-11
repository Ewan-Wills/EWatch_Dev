#!/usr/bin/env python3
"""Screenshare: mirror a Mac window/region onto the EWatch, low latency.

One companion for both watch apps:
    USB   (Screenshare app, EW_APPS=screenshare)  python3 screenshare.py
    WiFi  (Stream app,      EW_APPS=livestream)   python3 screenshare.py --wifi ewatch.local

Each frame: capture -> letterbox to 240x280 -> find the rectangle that changed
since the last frame sent -> JPEG (4:2:0, MCU-aligned) -> one packet. The
watch decodes it straight to the panel. Packet format and replies:
src/core/ewstream.h. At most one packet is un-acknowledged (>>R), and the
next capture is timed so the frame lands as the watch frees up ("jit"
pacing): frames never queue, so latency stays at capture + encode + link +
decode/push.

Measures as it runs: fps shown on the watch (>>D replies) and latency from the
start of the capture to the watch reporting the rect on the panel. Add the
panel's own scan-out (0-16 ms) for glass.

    --source test       synthetic full-screen motion (worst case, no capture)
    --window NAME       capture one window (Quartz, works when covered)
    --region X,Y,W,H    capture a screen rectangle
    --seconds N --json OUT   timed run with a stats file (for tests)
    --snap-dir DIR      save PC frame + watch frame (>>SNAP) at the end
"""
import argparse
import glob
import io
import json
import os
import queue
import socket
import statistics
import struct
import sys
import threading
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFont

PW, PH = 240, 280            # the panel
# The picture as the viewer sees it: 240x280 portrait, or 280x240 landscape
# (rotated onto the panel; the watch is held on its side).
VW, VH = PW, PH
ROT = 0                      # np.rot90 k applied to a landscape picture
T_JPEG, T_PING, T_SNAP = 1, 2, 3
F_KEEP = 1          # watch also copies the rect for >>SNAP / ewt snap (costs ~9 ms)
MCU = 16


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------
class UsbLink:
    def __init__(self, port):
        import serial
        # exclusive: keeps port-probing tools (e.g. the Claude Face bridge) out
        # mid-stream; they may hold the port briefly, so retry the open.
        for i in range(40):
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=2, exclusive=True)
                # flock is advisory; TIOCEXCL makes the kernel refuse any other
                # open() (a probing tool's bytes would land mid-frame).
                import fcntl
                import termios
                fcntl.ioctl(self.ser.fileno(), termios.TIOCEXCL)
                break
            except serial.SerialException:
                if i == 39:
                    raise
                time.sleep(0.1)
        self.name = port

    def write(self, b):
        self.ser.write(b)

    def read(self, n):
        return self.ser.read(max(1, min(n, self.ser.in_waiting or 1)))

    def close(self):
        self.ser.close()


class TcpLink:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(0.05)
        self.name = f"{host}:{port}"

    def write(self, b):
        self.sock.sendall(b)

    def read(self, n):
        try:
            d = self.sock.recv(max(1, n))
        except socket.timeout:
            return b""
        if d == b"":
            raise ConnectionError("watch closed the connection")
        return d

    def close(self):
        self.sock.close()


def autodetect_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def packet(ptype, seq, x=0, y=0, w=0, h=0, payload=b"", flags=0):
    cs = 0
    if payload:
        cs = int(np.bitwise_xor.reduce(np.frombuffer(payload, dtype=np.uint8)))
    return (b"EX" + struct.pack("<BBHHHHHI", ptype, flags, seq & 0xFFFF, x, y, w, h, len(payload))
            + payload + bytes([cs]))


# ---------------------------------------------------------------------------
# Reply reader: >>R / >>D / >>P / >>SNAP / >>BAD / >>READY
# ---------------------------------------------------------------------------
class Replies(threading.Thread):
    def __init__(self, link, verbose):
        super().__init__(daemon=True)
        self.link, self.verbose = link, verbose
        self.buf = bytearray()
        self.cond = threading.Condition()
        self.acked = -1            # last seq with >>R
        self.shown = {}            # seq -> (t_recv, dec_us, push_us)
        self.snap = None
        self.ready = False
        self.bad = []
        self.pongs = {}
        self.err = None
        self.copy_us = 0
        self.extra = []
        self.lost_r = 0

    def run(self):
        try:
            while True:
                d = self.link.read(65536)
                if d:
                    self.buf += d
                    self._parse()
        except Exception as e:      # port gone
            self.err = e
            with self.cond:
                self.cond.notify_all()

    def _parse(self):
        while True:
            i = self.buf.find(b"\n")
            if i < 0:
                return
            line = bytes(self.buf[:i]).decode("utf-8", "replace").strip()
            if line.startswith(">>SNAP "):
                n = int(line.split()[1])
                if len(self.buf) < i + 1 + n:
                    return                       # wait for the pixels
                px = bytes(self.buf[i + 1:i + 1 + n])
                del self.buf[:i + 1 + n]
                with self.cond:
                    self.snap = px
                    self.cond.notify_all()
                continue
            del self.buf[:i + 1]
            now = time.perf_counter()
            p = line.split()
            with self.cond:
                if line.startswith(">>R "):
                    self.acked = int(p[1])
                elif line.startswith(">>D "):
                    self.shown[int(p[1])] = (now, int(p[2]), int(p[3]))
                    self.copy_us = int(p[4]) if len(p) > 4 else 0
                    self.extra = [int(v) for v in p[5:]]       # gap, queue-wait, reply-write (us)
                    self.acked = int(p[1])            # shown implies received
                elif line.startswith(">>P "):
                    self.pongs[int(p[1])] = (now, int(p[2]))
                elif line.startswith(">>READY"):
                    self.ready = True
                elif line.startswith(">>BAD"):
                    self.bad.append(line)
                    print(f"[watch] {line}")
                elif line and self.verbose:
                    print(f"[watch] {line}")
                self.cond.notify_all()

    def wait(self, pred, timeout):
        end = time.perf_counter() + timeout
        with self.cond:
            while not pred():
                if self.err:
                    raise ConnectionError(self.err)
                left = end - time.perf_counter()
                if left <= 0:
                    return False
                self.cond.wait(left)
            return True


# ---------------------------------------------------------------------------
# Sources -> 240x280 RGB uint8 arrays
# ---------------------------------------------------------------------------
def fit(img):
    """Letterbox a PIL image into the view (VW x VH): black bars keep the aspect."""
    s = min(VW / img.width, VH / img.height)
    w, h = max(1, round(img.width * s)), max(1, round(img.height * s))
    if (w, h) != img.size:
        img = img.resize((w, h), Image.BILINEAR, reducing_gap=2.0)
    if (w, h) == (VW, VH):
        return np.asarray(img.convert("RGB"))
    out = np.zeros((VH, VW, 3), np.uint8)
    x, y = (VW - w) // 2, (VH - h) // 2
    out[y:y + h, x:x + w] = np.asarray(img.convert("RGB"))
    return out


def fit_box(w, h):
    """Where fit() puts a w x h picture in the view: (scale, x offset, y offset)."""
    s = min(VW / w, VH / h)
    return s, (VW - max(1, round(w * s))) // 2, (VH - max(1, round(h * s))) // 2


# Mouse pointer drawn on the watch (screen captures leave it out): white arrow,
# black outline, tip at (0, 0). 12 px tall in view pixels.
CURSOR = [(0, 0), (0, 12), (3, 9), (5, 14), (7, 13), (5, 8), (9, 8)]


class MonitorSource:
    """A virtual display (second monitor) that exists while this runs.

    Drag windows or tabs onto it like any monitor; it is streamed to the
    watch. macOS won't run a desktop narrower than 800 points, so the monitor
    is at least 800 pt wide (default: exactly that, in the watch's shape:
    800x686 landscape, 800x933 portrait), and things can't be made bigger by
    shrinking it. What the watch shows (view):
      fit     the whole monitor
      follow  1/zoom of it, centred on the mouse pointer (like macOS Zoom)
    Rendered at 2x (HiDPI) so text is scaled down from crisp pixels. Stays where you put it in System Settings > Displays (--side forces it). Uses macOS's private
    CGVirtualDisplay API (as DeskPad / BetterDisplay do). It did not come
    online while a 4K monitor was also attached (2026-10-11, M4 Pro).
    """
    SERIAL = 0xE1E1
    MIN_W = 800

    def __init__(self, w, h, view="fit", zoom=2.0, side=None):
        import objc
        import Quartz as Q
        from dispatch import dispatch_queue_create
        self.Q = Q
        if w < self.MIN_W:
            h, w = round(h * self.MIN_W / w), self.MIN_W
            print(f"  macOS needs >= {self.MIN_W} pt wide: monitor is {w}x{h} (use --zoom to enlarge)")
        D = objc.lookUpClass("CGVirtualDisplayDescriptor")
        V = objc.lookUpClass("CGVirtualDisplay")
        S = objc.lookUpClass("CGVirtualDisplaySettings")
        M = objc.lookUpClass("CGVirtualDisplayMode")
        d = D.alloc().init()
        d.setName_("EWatch")
        d.setMaxPixelsWide_(w * 2)
        d.setMaxPixelsHigh_(h * 2)
        d.setSizeInMillimeters_(Q.CGSizeMake(w * 2 / 220 * 25.4, h * 2 / 220 * 25.4))   # ~220 ppi
        d.setVendorID_(0xEEEE)
        d.setProductID_(0x00E1)
        d.setSerialNum_(self.SERIAL)
        d.setQueue_(dispatch_queue_create(b"ewatch.vdisplay", None))
        self.vd = V.alloc().initWithDescriptor_(d)          # keep: the display lives as long as this
        if self.vd is None:
            sys.exit("Could not create the virtual display (CGVirtualDisplay).")
        s = S.alloc().init()
        s.setHiDPI_(1)
        s.setModes_([M.alloc().initWithWidth_height_refreshRate_(w * 2, h * 2, 60),
                     M.alloc().initWithWidth_height_refreshRate_(w, h, 60)])
        if not self.vd.applySettings_(s):
            sys.exit("Virtual display refused the mode.")
        self.did = self.vd.displayID()
        modes = None
        for _ in range(20):                                  # WindowServer brings it up
            time.sleep(0.25)
            modes = Q.CGDisplayCopyAllDisplayModes(self.did, {Q.kCGDisplayShowDuplicateLowResolutionModes: True})
            if modes:
                break
        if not modes or Q.CGDisplayCreateImage(self.did) is None:
            sys.exit("The virtual monitor didn't come online. If another external monitor is "
                     "attached, unplug it and try again.")
        # The default is the 1x mode: pick w x h points at 2x, keep it right of main.
        want = None
        for m in modes:
            if Q.CGDisplayModeIsUsableForDesktopGUI(m) and Q.CGDisplayModeGetWidth(m) == w \
                    and Q.CGDisplayModeGetHeight(m) == h:
                if want is None or Q.CGDisplayModeGetPixelWidth(m) > Q.CGDisplayModeGetPixelWidth(want):
                    want = m
        main = Q.CGMainDisplayID()
        mb = Q.CGDisplayBounds(main)
        err, cfg = Q.CGBeginDisplayConfiguration(None)
        if want is not None:
            Q.CGConfigureDisplayWithDisplayMode(cfg, self.did, want, None)
        # Where it goes: macOS reuses the spot it had last time (so moving it in
        # System Settings > Displays sticks). --side forces it beside the main
        # display, top-aligned; so does a first run that lands on top of main.
        b0 = Q.CGDisplayBounds(self.did)
        if side or (b0.origin.x == 0 and b0.origin.y == 0):
            Q.CGConfigureDisplayOrigin(cfg, main, 0, 0)
            Q.CGConfigureDisplayOrigin(cfg, self.did, -w if side == "left" else int(mb.size.width), 0)
        Q.CGCompleteDisplayConfiguration(cfg, Q.kCGConfigureForSession)
        time.sleep(0.5)
        b = Q.CGDisplayBounds(self.did)
        c = Q.CGDisplayCopyDisplayMode(self.did)
        self.view = view
        self.last, self.gone_since = None, None
        self.zoom = max(1.0, zoom)
        self.center = (b.size.width / 2, b.size.height / 2)   # follow: viewport centre, points
        how = {"fit": "whole monitor", "follow": f"zoom {self.zoom:g}x following the pointer"}[view]
        self.desc = (f"virtual monitor {int(b.size.width)}x{int(b.size.height)} pt "
                     f"({Q.CGDisplayModeGetPixelWidth(c)} px wide) at {int(b.origin.x)},{int(b.origin.y)}, {how}")

    def _viewport(self, b, px, py, on):
        """(x, y, w, h) in display-local points for the current view."""
        W, H = b.size.width, b.size.height
        if self.view == "follow":
            vw, vh = W / self.zoom, H / self.zoom
            if on:
                self.center = (px, py)
            cx = min(max(self.center[0], vw / 2), W - vw / 2)
            cy = min(max(self.center[1], vh / 2), H - vh / 2)
            return cx - vw / 2, cy - vh / 2, vw, vh
        return 0, 0, W, H

    def grab(self):
        Q = self.Q
        b = Q.CGDisplayBounds(self.did)
        p = Q.CGEventGetLocation(Q.CGEventCreate(None))
        px, py = p.x - b.origin.x, p.y - b.origin.y
        on = 0 <= px < b.size.width and 0 <= py < b.size.height
        lx, ly, vw, vh = self._viewport(b, px, py, on)
        rect = Q.CGRectMake(b.origin.x + lx, b.origin.y + ly, vw, vh)
        im = Q.CGDisplayCreateImageForRect(self.did, Q.CGRectMake(lx, ly, vw, vh))
        if im is None:
            # Displays being rearranged (System Settings): no image for a moment.
            # Show the last frame until it's back; give up after 10 s.
            if self.gone_since is None:
                self.gone_since = time.time()
            if time.time() - self.gone_since > 10 or self.last is None:
                raise RuntimeError("virtual display gone")
            time.sleep(0.1)
            return self.last
        self.gone_since = None
        w, h, bpr = Q.CGImageGetWidth(im), Q.CGImageGetHeight(im), Q.CGImageGetBytesPerRow(im)
        data = Q.CGDataProviderCopyData(Q.CGImageGetDataProvider(im))
        frame = fit(Image.frombuffer("RGBA", (w, h), bytes(data), "raw", "BGRA", bpr, 1))
        # Captures have no pointer: draw it where the mouse is.
        if on and rect.origin.x <= p.x < rect.origin.x + vw and rect.origin.y <= p.y < rect.origin.y + vh:
            s, ox, oy = fit_box(vw, vh)
            x, y = ox + (p.x - rect.origin.x) * s, oy + (p.y - rect.origin.y) * s
            img = Image.fromarray(frame)
            ImageDraw.Draw(img).polygon([(x + a, y + c) for a, c in CURSOR],
                                        fill=(255, 255, 255), outline=(0, 0, 0))
            frame = np.asarray(img)
        self.last = frame
        return frame


class MssSource:
    def __init__(self, region):
        import mss
        self.sct = mss.MSS() if hasattr(mss, "MSS") else mss.mss()
        self.region = region or self.sct.monitors[1]

    def grab(self):
        g = self.sct.grab(self.region)
        return fit(Image.frombuffer("RGB", g.size, g.rgb, "raw", "RGB", 0, 1))


class WindowSource:
    """One window via CGWindowListCreateImage (works while it's covered)."""
    def __init__(self, name):
        import Quartz as Q
        self.Q = Q
        name_l = name.lower()
        for w in Q.CGWindowListCopyWindowInfo(Q.kCGWindowListOptionOnScreenOnly, Q.kCGNullWindowID):
            owner, title = w.get("kCGWindowOwnerName") or "", w.get("kCGWindowName") or ""
            b = w.get("kCGWindowBounds")
            if (name_l in owner.lower() or name_l in title.lower()) and b and b["Width"] > 40 \
                    and b["Height"] > 40 and w.get("kCGWindowLayer", 0) == 0:
                self.wid = w["kCGWindowNumber"]
                self.desc = f'{owner} "{title}" {int(b["Width"])}x{int(b["Height"])}'
                return
        sys.exit(f'No on-screen window matching "{name}".')

    def grab(self):
        Q = self.Q
        im = Q.CGWindowListCreateImage(Q.CGRectNull, Q.kCGWindowListOptionIncludingWindow, self.wid,
                                       Q.kCGWindowImageBoundsIgnoreFraming | Q.kCGWindowImageNominalResolution)
        if im is None:
            raise RuntimeError("window gone")
        w, h, bpr = Q.CGImageGetWidth(im), Q.CGImageGetHeight(im), Q.CGImageGetBytesPerRow(im)
        data = Q.CGDataProviderCopyData(Q.CGImageGetDataProvider(im))
        img = Image.frombuffer("RGBA", (w, h), bytes(data), "raw", "BGRA", bpr, 1)
        return fit(img)


class TestSource:
    """Worst case: every pixel changes every frame, plus a frame counter."""
    def __init__(self):
        yy, xx = np.mgrid[0:VH, 0:VW]
        self.xx, self.yy = xx.astype(np.float32), yy.astype(np.float32)
        self.n = 0
        try:
            self.font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 44)
        except OSError:
            self.font = ImageFont.load_default()

    def grab(self):
        t = self.n / 30.0
        self.n += 1
        r = 127 + 127 * np.sin(self.xx / 23 + t * 3.1)
        g = 127 + 127 * np.sin(self.yy / 31 - t * 2.3)
        b = 127 + 127 * np.sin((self.xx + self.yy) / 40 + t * 1.7)
        img = Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))
        d = ImageDraw.Draw(img)
        bx = int((VW - 60) * (0.5 + 0.5 * np.sin(t * 2)))
        d.rectangle([bx, VH - 80, bx + 60, VH - 20], fill=(255, 255, 255))
        d.rectangle([20, 40, 220, 110], fill=(0, 0, 0))
        d.text((30, 50), f"{self.n:05d}", fill=(255, 255, 0), font=self.font)
        return np.asarray(img)


# ---------------------------------------------------------------------------
# Encoder: changed rect -> JPEG
# ---------------------------------------------------------------------------
def changed_rect(prev, cur, thresh):
    if prev is None:
        return 0, 0, PW, PH
    diff = np.abs(cur.astype(np.int16) - prev.astype(np.int16)).max(axis=2) > thresh
    rows, cols = np.flatnonzero(diff.any(axis=1)), np.flatnonzero(diff.any(axis=0))
    if rows.size == 0:
        return None
    x0, x1 = cols[0] // MCU * MCU, min(PW, -(-(cols[-1] + 1) // MCU) * MCU)
    y0, y1 = rows[0] // MCU * MCU, min(PH, -(-(rows[-1] + 1) // MCU) * MCU)
    return int(x0), int(y0), int(x1 - x0), int(y1 - y0)


def encode(frame, rect, quality):
    x, y, w, h = rect
    buf = io.BytesIO()
    Image.fromarray(frame[y:y + h, x:x + w]).save(buf, "JPEG", quality=quality, subsampling=2,
                                                  optimize=False, progressive=False)
    return buf.getvalue()


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="USB serial port (default: auto /dev/cu.usbmodem*)")
    ap.add_argument("--wifi", metavar="HOST[:PORT]", help="stream over WiFi to the Stream app instead")
    ap.add_argument("--source", choices=["screen", "test"], default="screen")
    ap.add_argument("--monitor", nargs="?", const="auto", metavar="WxH",
                    help="make a virtual second monitor and stream it (drag windows onto it). "
                         "WxH in points, at least 800 wide (macOS limit); default 800 wide in the "
                         "watch's shape. See --view")
    ap.add_argument("--side", choices=["right", "left"],
                    help="--monitor: put it beside the main display (default: wherever it was "
                         "last, e.g. where you moved it in System Settings > Displays)")
    ap.add_argument("--view", choices=["fit", "follow"],
                    help="--monitor: fit = whole monitor; follow = 1/ZOOM of it following the "
                         "mouse. Default: follow if --zoom is given, else fit")
    ap.add_argument("--zoom", type=float,
                    help="--monitor --view follow: magnification (default 2)")
    ap.add_argument("--window", help="capture the first window whose owner/title contains this")
    ap.add_argument("--region", help="capture X,Y,W,H (screen points)")
    ap.add_argument("--display", type=int, default=1, help="mss monitor index (default 1)")
    ap.add_argument("--landscape", nargs="?", const="left", choices=["left", "right"],
                    help="landscape 280x240 (black bars keep the aspect): the picture's top is "
                         "on the panel's left edge (default) or right edge")
    ap.add_argument("--fps", type=float, default=60, help="cap on frames sent per second (default 60)")
    ap.add_argument("--quality", type=int, default=60, help="JPEG quality (default 60)")
    ap.add_argument("--thresh", type=int, default=6, help="per-channel change that counts as changed")
    ap.add_argument("--pace", choices=["jit", "eager"], default="jit",
                    help="jit: capture so the frame lands as the watch frees up (default)")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds")
    ap.add_argument("--json", help="write run stats here at the end")
    ap.add_argument("--snap-dir", help="at the end save pc.png (sent) + watch.png (>>SNAP) here")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo all watch lines")
    a = ap.parse_args()

    global VW, VH, ROT
    if a.landscape:
        VW, VH = PH, PW
        # top of the picture on the panel's left edge (rot90 k=1, counter-
        # clockwise) or its right edge (k=-1)
        ROT = 1 if a.landscape == "left" else -1

    if a.wifi:
        host, _, port = a.wifi.partition(":")
        link = TcpLink(host, int(port or 7878))
    else:
        port = a.port or autodetect_port()
        if not port:
            sys.exit("No /dev/cu.usbmodem* - plug the watch in or pass --port")
        link = UsbLink(port)
    rx = Replies(link, a.verbose)
    rx.start()

    if a.source == "test":
        src, what = TestSource(), "test pattern (full-screen motion)"
    elif a.monitor:
        if a.monitor == "auto":
            mw, mh = MonitorSource.MIN_W, round(MonitorSource.MIN_W * VH / VW)
        else:
            try:
                mw, mh = (int(v) for v in a.monitor.lower().split("x"))
            except ValueError:
                sys.exit("--monitor takes WxH, e.g. --monitor 420x360")
        view = a.view or ("follow" if a.zoom else "fit")
        src = MonitorSource(mw, mh, view, a.zoom or 2.0, a.side)
        what = src.desc
    elif a.window:
        src = WindowSource(a.window)
        what = f"window {src.desc}"
    else:
        region = None
        if a.region:
            x, y, w, h = (int(v) for v in a.region.split(","))
            region = {"left": x, "top": y, "width": w, "height": h}
        src = MssSource(region)
        what = f"region {src.region}"

    # Wait for the app (a READY line, or a ping answer if it was already up).
    link.write(packet(T_PING, 0))
    if not rx.wait(lambda: rx.ready or 0 in rx.pongs, 5):
        sys.exit(f"No reply on {link.name}: open Screenshare (USB) / Stream (WiFi) on the watch.")
    orient = f"landscape-{a.landscape}" if a.landscape else "portrait"
    print(f"Streaming {what} -> {link.name}, {orient}, q{a.quality}, pace={a.pace}. Ctrl-C to stop.")

    seq = 0
    prev = None                  # last frame sent (240x280x3)
    sent = {}                    # seq -> (t_capture_start, bytes, rect area)
    pc_est, disp_est, tx_est = 0.015, 0.025, 0.010
    last_r_t = None
    min_period = 1.0 / a.fps if a.fps > 0 else 0
    t_start = time.perf_counter()
    t_report, last_sent_t, last_ping = t_start, 0.0, t_start
    lat_all, rep_shown, rep_bytes = [], 0, 0
    lat_win, dec_win, push_win = [], [], []
    shown_seen = set()
    skipped = 0

    try:
        while True:
            now = time.perf_counter()
            if a.seconds and now - t_start >= a.seconds:
                break
            # Credit: at most two frames not yet on the panel (one being pushed,
            # one waiting), so receive overlaps decode but nothing queues.
            if seq >= 2:
                rx.wait(lambda: ((seq - 2) & 0xFFFF) in rx.shown, 0.1)
            # jit: the watch frees up ~disp_est after it finished frame seq-2;
            # start capturing so capture + encode + transfer land just then.
            if a.pace == "jit" and seq >= 2:
                with rx.cond:
                    d_prev = rx.shown.get((seq - 2) & 0xFFFF)
                if d_prev:
                    target = d_prev[0] + disp_est - pc_est - tx_est - 0.002
                    d = target - time.perf_counter()
                    if d > 0:
                        time.sleep(d)
            if min_period:
                d = last_sent_t + min_period - time.perf_counter()
                if d > 0:
                    time.sleep(d)

            t_cap = time.perf_counter()
            frame = src.grab()
            if ROT:
                frame = np.ascontiguousarray(np.rot90(frame, ROT))
            rect = changed_rect(prev, frame, a.thresh)
            if rect is None:
                skipped += 1
                if time.perf_counter() - last_ping > 1.0:   # keep the watch out of "waiting"
                    link.write(packet(T_PING, 0xFFFF))
                    last_ping = time.perf_counter()
                time.sleep(0.004)
                last_r_t = None
                _collect(rx, sent, shown_seen, lat_win, lat_all, dec_win, push_win)
                continue
            jpg = encode(frame, rect, a.quality)
            pkt = packet(T_JPEG, seq, *rect, payload=jpg)
            link.write(pkt)
            t_sent = time.perf_counter()
            sent[seq & 0xFFFF] = (t_cap, len(pkt))
            pc_est = 0.8 * pc_est + 0.2 * (t_sent - t_cap)
            prev = frame
            last_sent_t = t_cap
            rep_bytes += len(pkt)
            seq += 1
            last_r_t = None

            # Wait for >>R of this packet before deciding when to capture next.
            # A lost reply line must not stall the stream: the link itself
            # applies back-pressure, so after 80 ms just carry on.
            if rx.wait(lambda: rx.acked == (seq - 1) & 0xFFFF, 0.08):
                last_r_t = time.perf_counter()
                tx_est = 0.8 * tx_est + 0.2 * (last_r_t - t_sent)
            else:
                rx.lost_r += 1
            # Watch-side display time estimate from its own D reports.
            n_new = _collect(rx, sent, shown_seen, lat_win, lat_all, dec_win, push_win)
            rep_shown += n_new
            if dec_win:
                disp_est = 0.7 * disp_est + 0.3 * ((dec_win[-1] + push_win[-1]) / 1e6)

            now = time.perf_counter()
            if now - t_report >= 2.0:
                dt = now - t_report
                print(f"  {rep_shown / dt:5.1f} fps on watch  latency p50 {pct(lat_win, 50):5.1f} ms"
                      f"  p95 {pct(lat_win, 95):5.1f}  {rep_bytes / dt / 1024:6.1f} KiB/s"
                      f"  watch dec {pct(dec_win, 50) / 1000:4.1f} ms push-wait {pct(push_win, 50) / 1000:4.1f} ms"
                      f" copy {rx.copy_us / 1000:4.1f} ms gap/qwait/reply {rx.extra}  idle {skipped} lostR {rx.lost_r}")
                t_report, rep_shown, rep_bytes, skipped = now, 0, 0, 0
                lat_win, dec_win, push_win = [], [], []
    except KeyboardInterrupt:
        print()

    # Drain the last >>D, then summary.
    rx.wait(lambda: ((seq - 1) & 0xFFFF) in rx.shown, 1.0)
    _collect(rx, sent, shown_seen, [], lat_all, [], [])
    dur = time.perf_counter() - t_start
    shown_ts = sorted(v[0] for k, v in rx.shown.items() if k in shown_seen)
    # steady-state fps: drop the first second
    steady = [t for t in shown_ts if t - t_start > 1.0]
    fps = (len(steady) - 1) / (steady[-1] - steady[0]) if len(steady) > 2 else 0.0
    gaps = [1000 * (b - a_) for a_, b in zip(steady, steady[1:])]
    stats = {
        "transport": "wifi" if a.wifi else "usb", "orientation": orient, "source": what, "seconds": round(dur, 2),
        "frames_sent": seq, "frames_shown": len(shown_ts), "bad": len(rx.bad), "lost_replies": rx.lost_r,
        "fps": round(fps, 2),
        "frame_gap_ms_p95": round(pct(gaps, 95), 1) if gaps else None,
        "latency_ms": {k: round(pct(lat_all, p), 1) for k, p in (("p50", 50), ("p95", 95), ("max", 100))},
        "watch_decode_ms_p50": None, "quality": a.quality,
    }
    print(json.dumps(stats))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(stats, f, indent=1)

    # Last frame again, whole and flagged KEEP, so the watch holds a copy for
    # >>SNAP and for `ewt snap` after we let go of the port.
    if prev is not None:
        link.write(packet(T_JPEG, seq, 0, 0, PW, PH, encode(prev, (0, 0, PW, PH), a.quality), F_KEEP))
        rx.wait(lambda: (seq & 0xFFFF) in rx.shown, 1.0)
        seq += 1
    if a.snap_dir:
        os.makedirs(a.snap_dir, exist_ok=True)
        if prev is not None:
            Image.fromarray(prev).save(os.path.join(a.snap_dir, "pc.png"))
            if ROT:   # as the viewer sees it, the right way up
                Image.fromarray(np.rot90(prev, -ROT)).save(os.path.join(a.snap_dir, "pc_view.png"))
        rx.snap = None
        link.write(packet(T_SNAP, 0))
        if rx.wait(lambda: rx.snap is not None, 5):
            px = np.frombuffer(rx.snap, dtype="<u2").reshape(PH, PW)
            rgb = np.dstack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63,
                             (px & 31) * 255 // 31]).astype(np.uint8)
            Image.fromarray(rgb).save(os.path.join(a.snap_dir, "watch.png"))
            if ROT:
                Image.fromarray(np.rot90(rgb, -ROT)).save(os.path.join(a.snap_dir, "watch_view.png"))
            if prev is not None:
                err = np.abs(rgb.astype(int) - prev.astype(int)).mean()
                print(f"snap: watch vs pc mean abs diff {err:.1f}/255 -> {a.snap_dir}")
        else:
            print("snap: no >>SNAP reply")
    link.close()


def _collect(rx, sent, seen, lat_win, lat_all, dec_win, push_win):
    n = 0
    with rx.cond:
        items = [(k, v) for k, v in rx.shown.items() if k not in seen]
    for k, (t_d, dec, push) in items:
        seen.add(k)
        if k in sent:
            lat = 1000 * (t_d - sent[k][0])
            lat_win.append(lat)
            lat_all.append(lat)
            dec_win.append(dec)
            push_win.append(push)
            n += 1
    return n


if __name__ == "__main__":
    main()
