#!/usr/bin/env python3
"""Screenshare — stream a Mac window/region to the EWatch over USB.

Captures a region (or a named window) of the Mac screen, letterboxes it to the
watch's 240x280 panel, JPEG-encodes it and sends it framed down the USB serial
link to the `screenshare` watch app.

Wire protocol (little-endian), one frame:

    b'EW'             2 magic bytes
    uint32  len       JPEG payload length (LE)
    len bytes         baseline JPEG, <= 240x280
    uint8   csum      XOR of all `len` payload bytes

Flow control: the watch's USB-Serial/JTAG RX queue is 4 KiB with no flow
control (overflow bytes are silently dropped), so the header + payload are sent
in blocks of BLOCK bytes. After each non-final block the watch replies
">>ACK <bytes so far>"; after the last block + csum it replies
">>FRAME OK|BAD <n>". The next block/frame is only sent after that reply.

Other watch lines prefixed with ">>" (e.g. ">>READY screenshare") are printed
as [watch] ... and otherwise ignored.

Run:  python screenshare.py [--port /dev/cu.usbmodemXXXX] [--fps 12]
                            [--region X,Y,W,H | --window "Safari"]
                            [--quality 50] [--display N]

See README.md for details.
"""
import argparse
import glob
import struct
import sys
import time
from functools import reduce
from operator import xor

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed — run: pip install -r requirements.txt")

try:
    import mss
except ImportError:
    sys.exit("mss not installed — run: pip install -r requirements.txt")

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow not installed — run: pip install -r requirements.txt")

MAGIC = b"EW"
BLOCK = 2048          # must match kBlock in src/apps/screenshare.cpp (< RX queue)
REPLY_TIMEOUT = 2.0   # s to wait for an ACK/FRAME reply before resyncing
PANEL_W, PANEL_H = 240, 280


def autodetect_port():
    """Return the first /dev/cu.usbmodem* (macOS native-USB CDC), else None."""
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/tty.usbmodem*"))
    return ports[0] if ports else None


def find_window_region(name):
    """macOS only: return {top,left,width,height} for the first on-screen window
    whose owner or title contains `name` (case-insensitive). Needs pyobjc."""
    try:
        from Quartz import (
            CGWindowListCopyWindowInfo,
            kCGWindowListOptionOnScreenOnly,
            kCGNullWindowID,
        )
    except ImportError:
        sys.exit(
            "--window needs pyobjc (Quartz). Install it with:\n"
            "    pip install pyobjc-framework-Quartz\n"
            "or capture by coordinates with --region X,Y,W,H instead."
        )
    name_l = name.lower()
    wins = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)
    for w in wins:
        owner = (w.get("kCGWindowOwnerName") or "")
        title = (w.get("kCGWindowName") or "")
        if name_l in owner.lower() or name_l in title.lower():
            b = w.get("kCGWindowBounds")
            if not b or b["Width"] < 20 or b["Height"] < 20:
                continue
            return {
                "left": int(b["X"]),
                "top": int(b["Y"]),
                "width": int(b["Width"]),
                "height": int(b["Height"]),
            }
    sys.exit(f'No on-screen window matching "{name}" found.')


def letterbox(img):
    """Scale `img` to fit inside 240x280 preserving aspect, centered on black."""
    img = img.convert("RGB")
    scale = min(PANEL_W / img.width, PANEL_H / img.height)
    w = max(1, int(round(img.width * scale)))
    h = max(1, int(round(img.height * scale)))
    img = img.resize((w, h), Image.BILINEAR)
    canvas = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    canvas.paste(img, ((PANEL_W - w) // 2, (PANEL_H - h) // 2))
    return canvas


def encode_frame(img, quality):
    """Return (header, payload, csum_byte)."""
    import io
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=False)
    data = buf.getvalue()
    csum = reduce(xor, data, 0)
    return MAGIC + struct.pack("<I", len(data)), data, bytes([csum])


def wait_reply(ser, prefix, timeout):
    """Read lines until one starts with `prefix`; return it, or None on timeout.
    Other non-empty lines are echoed as [watch] telemetry."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if line.startswith(prefix):
            return line
        if line:
            print(f"[watch] {line}")
    return None


def send_frame(ser, header, data, csum):
    """Send one frame block-by-block, waiting for the watch's ACKs.
    Returns the ">>FRAME ..." reply, or None if the watch stopped answering."""
    sent = 0
    first = True
    while True:
        block = data[sent:sent + BLOCK]
        sent += len(block)
        last = sent >= len(data)
        ser.write((header if first else b"") + block + (csum if last else b""))
        first = False
        if last:
            return wait_reply(ser, ">>FRAME", REPLY_TIMEOUT)
        if wait_reply(ser, ">>ACK", REPLY_TIMEOUT) is None:
            return None


def drain_telemetry(ser):
    """Print any ">>..." lines the watch sent, non-blocking."""
    try:
        while ser.in_waiting:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line:
                print(f"[watch] {line}")
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser(description="Stream a Mac window/region to the EWatch over USB.")
    ap.add_argument("--port", help="serial port (default: auto-detect /dev/cu.usbmodem*)")
    ap.add_argument("--fps", type=float, default=12.0, help="target frames/sec (default 12)")
    ap.add_argument("--quality", type=int, default=50, help="JPEG quality 1-95 (default 50)")
    ap.add_argument("--region", help="capture region as X,Y,W,H (screen pixels)")
    ap.add_argument("--window", help='capture first window whose name/owner contains this (macOS, needs pyobjc)')
    ap.add_argument("--display", type=int, default=1, help="mss monitor index when no region/window (default 1 = primary)")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if not port:
        sys.exit("No serial port found. Plug in the watch and/or pass --port /dev/cu.usbmodemXXXX")

    # Resolve capture region.
    region = None
    if args.region:
        try:
            x, y, w, h = (int(v) for v in args.region.split(","))
            region = {"left": x, "top": y, "width": w, "height": h}
        except ValueError:
            sys.exit("--region must be X,Y,W,H integers, e.g. --region 0,0,1280,720")
    elif args.window:
        region = find_window_region(args.window)
        print(f"Window region: {region}")

    period = 1.0 / args.fps if args.fps > 0 else 0.0
    print(f"Opening {port} @115200 ...")
    ser = serial.Serial(port, 115200, timeout=0.05)
    time.sleep(0.3)  # let CDC settle

    sct = mss.mss()
    if region is None:
        mons = sct.monitors
        idx = args.display if 0 <= args.display < len(mons) else 1
        region = mons[idx]
        print(f"Capturing display {idx}: {region}")

    print(f"Streaming to watch at ~{args.fps} fps, quality {args.quality}. Ctrl-C to stop.")
    n = 0
    bytes_sent = 0
    t_report = time.time()
    try:
        while True:
            t0 = time.time()
            drain_telemetry(ser)
            shot = sct.grab(region)
            img = Image.frombytes("RGB", shot.size, shot.rgb)
            header, data, csum = encode_frame(letterbox(img), args.quality)
            reply = send_frame(ser, header, data, csum)
            if reply is None:
                # Watch not on the Screenshare screen, or a block was lost.
                # Let the watch's mid-frame timeout (1 s) reset its parser,
                # then start over with a fresh frame.
                print("  no reply from watch (is Screenshare open?) — resyncing")
                time.sleep(1.2)
                ser.reset_input_buffer()
                continue
            if reply.startswith(">>FRAME BAD"):
                print(f"[watch] {reply}")
            n += 1
            bytes_sent += len(header) + len(data) + 1

            now = time.time()
            if now - t_report >= 2.0:
                fps = n / (now - t_report)
                kbps = bytes_sent / 1024 / (now - t_report)
                print(f"  {fps:5.1f} fps  {kbps:6.1f} KiB/s  (last frame {len(data) + 7} B)")
                n = 0
                bytes_sent = 0
                t_report = now

            dt = time.time() - t0
            if period and dt < period:
                time.sleep(period - dt)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
