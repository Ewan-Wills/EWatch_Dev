# screenshare — Mac companion

Mirrors a Mac window, screen region or test pattern onto the EWatch with low
latency. One script for both watch apps:

| watch app | build | run |
|---|---|---|
| Screenshare (USB-C) | `EW_APPS=screenshare` | `python3 screenshare.py` |
| Stream (WiFi) | `EW_APPS=livestream` | `python3 screenshare.py --wifi ewatch.local` |

## ewbridge (the app)

`ewbridge` runs in the background and owns the watch link. Everything else (the UI, the CLI,
games, LLM agents) talks to it, so you can switch what the watch shows, start and stop games,
or point it at a window without ever dropping the connection.

```sh
bin/ewbridge up                 # start it + open the UI at http://127.0.0.1:7480/
bin/ewbridge show app swarm     # launches the game if needed
bin/ewbridge status
bin/ewbridge down
```

Open **Screenshare** on the watch. The bridge streams as soon as it's open, and a 1.2 s hold of
the side button leaves the app.

| want | CLI | UI |
|---|---|---|
| a game/app | `show app NAME` | Apps → Show |
| a window | `windows`, then `show window ID` (or a name) | Windows → Show |
| second monitor | `show monitor` / `show monitor follow --zoom 2` | Other sources |
| main screen, test pattern, nothing | `show screen` / `show test` / `show none` | Other sources |
| an MJPEG stream / JPEG URL | `show url http://…/x.mjpg` | URL box |
| orientation | `orient landscape-left\|portrait\|landscape-right` | Picture |
| what the watch shows (read back) | `snap out.png` | Read back from watch |
| watch input | `input`, `events -f` | Watch input |
| buzz | `haptic 60 200` | Buzz |
| test input | `sim "tap 120 140"`, `sim btn short`, `sim tilt 0.5 0 0.85` | |
| flash with ewt | `release 60`, then flash; it reconnects | Free USB port |

**Watch input**: touch (down/move/up), gestures, the side button (down/up/short) and the
accelerometer (50 Hz, in g). Touch positions are in the picture's coordinates; the rotation is
already undone. Apps get it all in `Input` (`touch`, `tapped`, `tilt`, `wbutton`, `wpressed`,
`gestures`; touch also drives `mouse` unless the app sets `touch_as_mouse = False`) and can call
`self.haptic(ms, strength)`. With "Touch drives the Mac pointer" on, touches click and drag on
the window or monitor being shown (needs Accessibility permission).

**Several watches**: every `/dev/cu.usbmodem*` becomes a watch, named by the id its firmware
reports. Each has its own source and orientation; pick one with `--watch ID`, `?watch=ID`, or the
tabs in the UI. A watch remembers its settings when re-plugged onto another port. (Built for,
not yet tested with, two watches.)

**Your own programs**: `ewbridge/client.py` puts pixels on the watch from any program and gets
its input back (`examples/client_example.py`). The wire format is plain TCP, documented in that
file, so any language works.

**LLMs**: everything is an HTTP call (`GET /api/help` lists them), a CLI command (`--json` for
raw output), or an MCP tool:
```sh
claude mcp add ewatch -- ~/Documents/EWatch/EWatch_Firmware/EWatchOS2.1/companions/screenshare/bin/ewbridge mcp
```
Tools: `ewatch_status`, `ewatch_list_windows`, `ewatch_show`, `ewatch_orientation`, `ewatch_apps`,
`ewatch_stop_app`, `ewatch_snapshot` (an image), `ewatch_input`, `ewatch_haptic`, `ewatch_sim`,
`ewatch_release`.

`screenshare.py` below is the one-shot, no-bridge version (and the measuring tool).

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

## Apps: run on the Mac, play on the watch

```sh
.venv/bin/python screenshare.py --app swarm --landscape     # play with the mouse in the Mac window
.venv/bin/python screenshare.py --app list                  # what's in apps/
```

An app runs on the MacBook in its own window (mouse, keyboard and controller input go
there) and every frame it draws goes straight to the watch: no screen capture, no Screen
Recording permission, exact pixels. `--app-scale N` sets the Mac window zoom (default 3).

To make one, add `apps/<name>.py` with one subclass of `apps.App`:

```python
from .base import App

class Bounce(App):
    """One line shown by --app list."""
    title = "Bounce"
    fps = 60

    def update(self, dt, inp):          # inp.mouse, inp.clicked, inp.keys, inp.pressed, inp.axes, inp.pad
        ...

    def draw(self):                     # draw into self.surface (pygame, self.w x self.h)
        ...                             # or return an h x w x 3 uint8 numpy array
```

`self.w x self.h` is the watch picture: 280x240 with `--landscape`, 240x280 without. The
streamer handles rotation, change detection and pacing. Draw your own pointer, because the
watch doesn't see the Mac's. `apps/swarm.py` is the worked example: 30k-particle numpy
physics with trails and bloom (~3.5 ms a frame on an M4 Pro), mouse-only.

## How it's fast

- Only the changed rectangle is sent (16-px aligned JPEG, 4:2:0).
- One packet in flight: nothing queues, so latency doesn't grow.
- USB: the watch reads the USB FIFO directly, so the link applies the back-pressure.
  There are no per-block ACKs.
- On the watch, JPEG strips go to the panel by GDMA while the next strip decodes.

Protocol: `src/core/ewstream.h`.
