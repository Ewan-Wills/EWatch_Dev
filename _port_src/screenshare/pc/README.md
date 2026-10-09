# screenshare — Mac companion

Streams a Mac window or screen region to the EWatch over USB. The watch runs the
`Screenshare` app (swipe up on the watch face → **Screenshare**); this script
captures the Mac, scales each frame to the 240×280 panel, JPEG-encodes it and
sends it framed over the USB serial link.

## 1. Install

Python 3.9+ recommended. From this `pc/` folder:

```sh
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

`--window "Name"` (capture a specific window by title) additionally needs
`pyobjc-framework-Quartz`:

```sh
pip install pyobjc-framework-Quartz
```

macOS will prompt for **Screen Recording** permission the first time (System
Settings → Privacy & Security → Screen Recording → enable your terminal /
Python). Grant it, then re-run.

## 2. Run

1. On the watch, open the **Screenshare** app. It shows "Waiting for Mac…".
2. On the Mac:

```sh
# Auto-detect the watch's serial port, capture the primary display at 12 fps:
python3 screenshare.py

# Pick a port explicitly:
python3 screenshare.py --port /dev/cu.usbmodem1101

# Capture a rectangle of the screen (X,Y,W,H in screen pixels):
python3 screenshare.py --region 0,0,1280,720 --fps 15 --quality 55

# Capture a specific window by name (needs pyobjc, macOS):
python3 screenshare.py --window "Safari"
```

The watch starts showing frames within a second. Press the watch's side button
(or swipe right) to go back to the launcher.

### Flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--port` | auto `/dev/cu.usbmodem*` | serial port of the watch |
| `--fps` | `12` | target frames per second (watch caps ~15–20) |
| `--quality` | `50` | JPEG quality 1–95; lower = smaller = faster |
| `--region X,Y,W,H` | — | capture this screen rectangle |
| `--window "Name"` | — | capture first window whose title/owner matches (macOS) |
| `--display N` | `1` | mss monitor index when no region/window (1 = primary) |

The script prints a throughput line every ~2 s (`fps`, `KiB/s`, last frame size)
and echoes watch telemetry as `[watch] >>...`.

## 3. Tuning

USB CDC throughput and per-frame JPEG decode on the watch are the limits. If the
watch stutters or lags:

- lower `--quality` (e.g. 40) — smaller frames decode faster;
- lower `--fps` (e.g. 8–10);
- capture a smaller/less busy `--region` (less JPEG entropy).

## 4. Protocol

Little-endian, one frame per screen capture:

```
'E' 'W'          2 magic bytes (0x45 0x57)
uint32  len      JPEG payload length (LE)
len bytes        baseline JPEG, already scaled to <= 240x280
uint8   csum     XOR of all `len` payload bytes
```

The watch resynchronises on the magic bytes and verifies the checksum, so it
tolerates boot-log noise and partial frames. Watch→Mac telemetry is plain ASCII
lines prefixed with `>>` (ignored by this script apart from being printed).
