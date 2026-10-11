# screenshare — Mac companion

Mirrors a Mac window, screen region or test pattern onto the EWatch with low
latency. One script for both watch apps:

| watch app | build | run |
|---|---|---|
| Screenshare (USB-C) | `EW_APPS=screenshare` | `python3 screenshare.py` |
| Stream (WiFi) | `EW_APPS=livestream` | `python3 screenshare.py --wifi ewatch.local` |

## Install

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

macOS asks for Screen Recording permission the first time.

## Run

Open the app on the watch (or `ewt do "goto Screenshare"`), then:

```sh
.venv/bin/python screenshare.py --window Safari          # one window, works while covered
.venv/bin/python screenshare.py --region 0,0,480,560     # a screen rectangle
.venv/bin/python screenshare.py --source test --seconds 20 --json run.json --snap-dir snaps
```

It prints fps as shown on the watch, latency (capture start to the watch
reporting the pixels on the panel), link KiB/s and the watch's decode and
push-wait times every 2 s. `--snap-dir` saves `pc.png` (the last frame sent)
and `watch.png` (read back from the watch) at the end.

| flag | default | |
|---|---|---|
| `--landscape [left\|right]` | off | 280x240 picture rotated onto the panel, black bars keep the aspect; `left` (default) puts the picture's top on the panel's left edge |
| `--quality` | 60 | JPEG quality |
| `--fps` | 60 | cap on frames sent |
| `--pace` | jit | `jit` captures so the frame lands as the watch frees up; `eager` sends as soon as the last one is acknowledged |
| `--thresh` | 6 | per-channel change that counts as a changed pixel |

## Second monitor

```sh
.venv/bin/python screenshare.py --landscape --monitor --view follow --zoom 2 # magnifier that follows the pointer
.venv/bin/python screenshare.py --landscape --monitor                      # whole monitor
```

`--monitor` adds a virtual "EWatch" display for as
long as the script runs. Move it in System Settings > Displays like any monitor (it stays
there next time; `--side left|right` puts it back beside the main one). Drag windows or tabs onto it and they show on the watch. The Mac
pointer is drawn on the watch while it's over that monitor.

Views (`--view`):

| view | shows | bigger text/icons by |
|---|---|---|
| `fit` | the whole monitor | nothing (everything is ~1/3 size) |
| `follow` | 1/`--zoom` of the monitor, centred on the pointer (like macOS Zoom) | `--zoom` |

- macOS won't run a desktop narrower than **800 points** (800x600 is its smallest mode), so the
  monitor itself can't be shrunk to make things bigger. It's 800 pt wide in the watch's shape
  (800x686 landscape, 800x933 portrait), rendered at 2x. `--monitor WxH` only makes it bigger.
- It didn't come online while a 4K external monitor was also attached (M4 Pro, 2026-10-11).
  Unplug the other monitor if it says it didn't come online.
- When the script stops, the display goes away and macOS moves its windows back. macOS
  remembers which windows were on it and puts them back next time.

## How it's fast

- Only the changed rectangle is sent (16-px aligned JPEG, 4:2:0).
- One packet in flight: nothing queues, so latency doesn't grow.
- USB: the watch reads the USB FIFO directly, so the link applies the back-pressure.
  There are no per-block ACKs.
- On the watch, JPEG strips go to the panel by GDMA while the next strip decodes.

Protocol: `src/core/ewstream.h`.
