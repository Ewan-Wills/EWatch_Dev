#!/usr/bin/env python3
"""
EWatch live streamer (macOS).

Captures a region of the Mac screen and streams it as 240x280 RGB565 frames
to the EWatch's Stream app over WiFi. Frames are LZ4-compressed by default
(lossless, ~3 ms decode on the watch, 30-50 percent size reduction on UI
content; expect ~25-30 FPS on a clean 2.4 GHz network).

Discovery is by hostname: the watch advertises itself as `ewatch.local` via
mDNS / Bonjour, and macOS resolves that automatically.

USAGE
  python3 stream_to_watch.py                    # pop up the region picker
  python3 stream_to_watch.py --region X,Y,W,H   # explicit region (skip picker)
  python3 stream_to_watch.py --host 192.168.1.42
  python3 stream_to_watch.py --raw              # disable LZ4 (for debugging)
  python3 stream_to_watch.py --fps 60

DEPENDENCIES
  pip3 install mss pillow lz4 numpy

PERMISSIONS (macOS)
  First run prompts for Screen Recording permission against whatever process
  ran python (Terminal, iTerm2, your IDE). Grant it in System Settings ->
  Privacy & Security -> Screen Recording, then re-launch.
"""

import argparse
import socket
import struct
import sys
import time

try:
    import lz4.block
except ImportError:
    sys.exit("Missing 'lz4'. Install with: pip3 install lz4")
try:
    import mss
except ImportError:
    sys.exit("Missing 'mss'. Install with: pip3 install mss")
try:
    from PIL import Image
except ImportError:
    sys.exit("Missing 'Pillow'. Install with: pip3 install pillow")


# Watch panel — fixed.
W, H = 240, 280

# Wire-format constants — must match src/apps/livestream.cpp.
MAGIC    = 0xE1
FLAG_LZ4 = 0x01


def to_rgb565_bytes(im: Image.Image) -> bytes:
    """Pack a PIL RGB image to little-endian RGB565 bytes."""
    if im.mode != 'RGB':
        im = im.convert('RGB')
    try:
        import numpy as np
        a = np.asarray(im, dtype=np.uint16)
        r = (a[..., 0] >> 3) & 0x1F
        g = (a[..., 1] >> 2) & 0x3F
        b = (a[..., 2] >> 3) & 0x1F
        return ((r << 11) | (g << 5) | b).astype('<u2').tobytes()
    except ImportError:
        out = bytearray(im.width * im.height * 2)
        i = 0
        for r, g, b in im.getdata():
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i]     = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
            i += 2
        return bytes(out)


def pick_region(target_ratio: float, aspect_label: str, display_idx: int = 1):
    """
    Screenshot-based region picker. Captures the primary display once, opens
    a Tk window that shows that frozen snapshot scaled to fit, and lets the
    user click-drag the capture rectangle on top of it. The dragged
    rectangle is locked to `target_ratio` (= width / height) so what you
    select matches the watch panel exactly — no stretching, no distortion.

    Returns (x, y, w, h) in `mss` physical-pixel coordinates ready to feed
    `mss.grab`, or None if cancelled.

    Why this instead of a transparent fullscreen overlay: on macOS
    `attributes('-fullscreen', True)` puts the Tk window on its own Space,
    hiding the rest of your windows — so the picker would show only the
    desktop wallpaper. Drawing on a still-snapshot dodges that entirely AND
    instantly reveals if Screen Recording permission is missing (you'd see
    only the wallpaper in the snapshot itself).
    """
    import tkinter as tk
    try:
        from PIL import ImageTk
    except ImportError:
        sys.exit("Missing 'Pillow' with Tk support. "
                 "Reinstall with: pip3 install --force-reinstall pillow")

    with mss.mss() as s:
        if display_idx < 1 or display_idx >= len(s.monitors):
            sys.exit(f'--display {display_idx} not available. mss sees '
                     f'{len(s.monitors)-1} display(s); pass 1..{len(s.monitors)-1}.')
        mon  = s.monitors[display_idx]
        shot = s.grab(mon)
        # Remember the monitor's origin — picker coords are relative to its
        # top-left, but mss.grab expects absolute screen coords.
        ox, oy = mon['left'], mon['top']
    img_full = Image.frombytes('RGB', shot.size, shot.rgb)
    full_w, full_h = img_full.size               # physical pixels

    # Scale the snapshot to fit comfortably on screen with a margin.
    probe = tk.Tk(); probe.withdraw()
    scr_w = probe.winfo_screenwidth()
    scr_h = probe.winfo_screenheight()
    probe.destroy()
    ratio = min((scr_w - 80) / full_w, (scr_h - 120) / full_h, 1.0)
    show_w, show_h = int(full_w * ratio), int(full_h * ratio)
    img_disp = img_full.resize((show_w, show_h), Image.LANCZOS)

    root = tk.Tk()
    root.title('EWatch Stream  —  drag a region (Esc to cancel)')
    root.configure(bg='black')
    photo  = ImageTk.PhotoImage(img_disp)
    canvas = tk.Canvas(root, width=show_w, height=show_h,
                       highlightthickness=0, bg='black', cursor='crosshair')
    canvas.pack(padx=10, pady=10)
    canvas.create_image(0, 0, anchor='nw', image=photo)
    root.photo = photo                           # keep ref alive

    info = tk.Label(root,
                    text=f'Drag a rectangle  ·  locked to {aspect_label}  ·  Esc to cancel',
                    fg='white', bg='black', font=('Helvetica', 14))
    info.pack(pady=(0, 8))

    st = {}

    # Constrain the dragged corner so the box always has the target aspect.
    # The user effectively drives whichever axis they pull harder on; the
    # other follows. This feels natural and removes the need for stretching.
    def snap(x0, y0, x1, y1):
        dx = x1 - x0
        dy = y1 - y0
        if abs(dx) < 1 and abs(dy) < 1:
            return x0, y0, x0, y0
        # If the user's drag implies a wider-than-target box, height wins.
        # Otherwise width wins. Equivalent to: enclose the cursor.
        if abs(dx) > abs(dy) * target_ratio:
            sy = 1 if dy >= 0 else -1
            dy = sy * abs(dx) / target_ratio
        else:
            sx = 1 if dx >= 0 else -1
            dx = sx * abs(dy) * target_ratio
        return x0, y0, int(x0 + dx), int(y0 + dy)

    def on_press(e):
        st['x0'], st['y0'] = e.x, e.y

    def on_drag(e):
        if 'x0' not in st: return
        canvas.delete('sel')
        x0, y0, x1, y1 = snap(st['x0'], st['y0'], e.x, e.y)
        canvas.create_rectangle(x0, y0, x1, y1,
                                outline='red', width=2, tags='sel')

    def on_release(e):
        if 'x0' not in st: return
        _, _, x1, y1 = snap(st['x0'], st['y0'], e.x, e.y)
        st['x1'], st['y1'] = x1, y1
        root.destroy()

    canvas.bind('<ButtonPress-1>',   on_press)
    canvas.bind('<B1-Motion>',       on_drag)
    canvas.bind('<ButtonRelease-1>', on_release)
    root.bind('<Escape>', lambda e: root.destroy())

    root.mainloop()

    if 'x1' not in st:
        return None
    x0, y0 = min(st['x0'], st['x1']), min(st['y0'], st['y1'])
    x1, y1 = max(st['x0'], st['x1']), max(st['y0'], st['y1'])
    if (x1 - x0) < 4 or (y1 - y0) < 4:
        return None

    # Convert display coords back to physical-pixel mss coords, offset by
    # the monitor's origin (matters for non-primary / virtual displays).
    return (ox + int(x0 / ratio), oy + int(y0 / ratio),
            int((x1 - x0) / ratio), int((y1 - y0) / ratio))


def connect(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # Generous send timeout; keeps us from hanging forever if the watch stalls.
    s.settimeout(10)
    return s


def stream_loop(sock: socket.socket, region: dict, use_lz4: bool,
                fps_cap: int, resample, rotate: int) -> None:
    """Synchronous capture → encode → send loop. Each iteration:
       grab a frame, resize, rotate, RGB565, LZ4, send. Simple and predictable.
    """
    period = 1.0 / max(fps_cap, 1)
    sct = mss.mss()
    t_report = time.time()
    n_frames = 0
    n_bytes  = 0

    pre_w, pre_h = (H, W) if rotate in (90, 270) else (W, H)
    pil_rot = {0: None, 90: -90, 180: 180, 270: 90}[rotate]

    while True:
        t0 = time.time()

        shot = sct.grab(region)
        im   = Image.frombytes('RGB', shot.size, shot.rgb)
        if im.size != (pre_w, pre_h):
            im = im.resize((pre_w, pre_h), resample)
        if pil_rot is not None:
            im = im.rotate(pil_rot, expand=True)
        payload = to_rgb565_bytes(im)

        flags = 0
        if use_lz4:
            payload = lz4.block.compress(payload, mode='default',
                                          acceleration=1, store_size=False)
            flags |= FLAG_LZ4

        sock.sendall(struct.pack('<BBHI', MAGIC, flags, 0, len(payload))
                     + payload)

        n_frames += 1
        n_bytes  += 8 + len(payload)

        now = time.time()
        if now - t_report >= 1.0:
            dt   = now - t_report
            fps  = n_frames / dt
            mbps = n_bytes * 8 / dt / 1e6
            avg  = n_bytes / n_frames
            print(f'  {fps:5.1f} FPS   {mbps:5.2f} Mbps   '
                  f'{avg:6.0f} B/frame avg')
            n_frames = n_bytes = 0
            t_report = now

        rem = period - (time.time() - t0)
        if rem > 0:
            time.sleep(rem)


def main():
    ap = argparse.ArgumentParser(
        description='Stream a region of the Mac screen to the EWatch.')
    ap.add_argument('--host', default='ewatch.local',
                    help='Watch hostname or IP (default: ewatch.local)')
    ap.add_argument('--port', type=int, default=7878)
    ap.add_argument('--region',
                    help='X,Y,W,H in physical pixels (skip the picker)')
    ap.add_argument('--fps',  type=int, default=30, help='Target FPS cap')
    ap.add_argument('--raw',  action='store_true',
                    help='Disable LZ4 compression (default is LZ4 on)')
    ap.add_argument('--resample', default='bilinear',
                    choices=['nearest', 'bilinear', 'bicubic', 'lanczos'])
    ap.add_argument('--rotate', type=int, default=0, choices=[0, 90, 180, 270],
                    help='Rotate frames before sending (Python-side; '
                         'use 90/270 if you wear the watch sideways)')
    ap.add_argument('--display', type=int, default=1,
                    help='Display index (1 = primary). For a virtual display '
                         'from BetterDisplay / Deskreen / DisplayLink, pick the '
                         'higher index; run with --list-displays to enumerate.')
    ap.add_argument('--list-displays', action='store_true',
                    help='List available displays and exit.')
    ap.add_argument('--whole-display', action='store_true',
                    help='Capture the entire --display, no region picker. '
                         'Scaled to fit the watch; if the source aspect does '
                         'not match the watch, the script suggests a --rotate.')
    args = ap.parse_args()

    if args.list_displays:
        with mss.mss() as s:
            for i, m in enumerate(s.monitors):
                tag = 'all' if i == 0 else f'#{i}'
                print(f'  [{tag:>3}]  origin=({m["left"]},{m["top"]})  '
                      f'size={m["width"]}x{m["height"]}')
        return

    # Pre-rotation dimensions — what the captured region maps to before
    # rotation. The picker is locked to this aspect so what you draw is
    # exactly what the watch shows (no stretching).
    pre_w, pre_h = (H, W) if args.rotate in (90, 270) else (W, H)
    aspect_ratio = pre_w / pre_h
    aspect_label = f'{pre_w}:{pre_h}'

    if args.region:
        try:
            x, y, w, h = (int(v.strip()) for v in args.region.split(','))
        except ValueError:
            sys.exit('--region must be four comma-separated integers: X,Y,W,H')
    elif args.whole_display:
        with mss.mss() as s:
            if args.display < 1 or args.display >= len(s.monitors):
                sys.exit(f'--display {args.display} not available '
                         f'(have 1..{len(s.monitors)-1}). Try --list-displays.')
            mon = s.monitors[args.display]
            x, y, w, h = mon['left'], mon['top'], mon['width'], mon['height']
        src_ratio = w / h
        if abs(src_ratio - aspect_ratio) > 0.02:
            # Try the other orientation — does --rotate 90 / 270 fix it?
            flipped = 1.0 / aspect_ratio
            if abs(src_ratio - flipped) < 0.02 and args.rotate in (0, 180):
                print(f'!! display #{args.display} is {w}x{h} ({src_ratio:.3f}) '
                      f'but the watch expects {aspect_label} '
                      f'({aspect_ratio:.3f}). Re-run with --rotate 90.')
            else:
                print(f'!! aspect mismatch: source {src_ratio:.3f} vs '
                      f'target {aspect_ratio:.3f}. Image will be stretched.')
    else:
        print(f'Capturing display #{args.display} for region picker '
              '(make sure Screen Recording permission is granted)...')
        sel = pick_region(aspect_ratio, aspect_label, args.display)
        if not sel:
            print('Region picker cancelled.')
            return
        x, y, w, h = sel

    region = {'left': x, 'top': y, 'width': w, 'height': h}
    rmap = {'nearest':  Image.NEAREST,
            'bilinear': Image.BILINEAR,
            'bicubic':  Image.BICUBIC,
            'lanczos':  Image.LANCZOS}
    resample = rmap[args.resample]

    print(f'Region : {x},{y}  {w}x{h} px  ->  {pre_w}x{pre_h}'
          + (f' (rotated {args.rotate} deg to {W}x{H})' if args.rotate else ''))
    print(f'Codec  : {"raw RGB565" if args.raw else "LZ4 RGB565"}')
    print(f'FPS cap: {args.fps}')

    while True:
        try:
            print(f'Connecting to {args.host}:{args.port} ...', end=' ', flush=True)
            sock = connect(args.host, args.port)
        except OSError as e:
            print(f'failed: {e}')
            print('Retrying in 3 s.  (Open the Stream app on the watch.)')
            time.sleep(3)
            continue
        print('connected.  Ctrl-C to stop.')

        try:
            stream_loop(sock, region, not args.raw, args.fps, resample,
                        args.rotate)
        except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError) as e:
            print(f'\nLink dropped ({e}).  Reconnecting...')
        except KeyboardInterrupt:
            print('\nDone.')
            try: sock.close()
            except Exception: pass
            return
        finally:
            try: sock.close()
            except Exception: pass


if __name__ == '__main__':
    main()
