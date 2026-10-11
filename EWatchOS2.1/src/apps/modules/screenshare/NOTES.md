# screenshare module notes

Mirror the Mac onto the watch over USB-C. The watch is a sink for JPEG rectangles; capture,
scaling, rotation and change detection all happen in `companions/screenshare/screenshare.py`.
Packet format and the decode/GDMA path are shared with the WiFi `livestream` module:
`src/core/ewstream.{h,cpp}`.

## Measured (2026-10-11, V2.0 board, USB-C to an M4 Pro MacBook)
- Full-screen motion (every pixel changes every frame), 60 s: 1940/1940 frames, 0 bad,
  32.4 fps (the watch's own count agrees), latency p50 47 ms / p95 52 ms. Latency runs from
  the start of capture to the watch reporting the pixels sent to the panel; add 0-16 ms of
  panel scan-out for glass.
- Animated Safari window: 40.8 fps, latency p50 41 ms.
- Per full frame: JPEG decode ~16 ms (SIMD colour), panel push 28 ms at the 40 MHz SPI
  clock. The SPI clock is the cap (~33 fps for full frames). 80 MHz was not tried: the panel
  is write-only (no MISO), so the result couldn't be checked from here.

## How it works
- RX: a core-0 task. Idle: HWCDC owns RX as usual (ewt works with the app open). From the
  first packet header until 500 ms of quiet, the HWCDC RX interrupt is masked and the task
  reads the USB-Serial/JTAG FIFO directly. The host is NAKed while the FIFO is full, so there
  are no drops and no ACK round trips. The task spins through a burst: sleeping 1 ms between
  packets capped the link at ~180 KiB/s.
- TX: replies are written straight to the USB IN FIFO. HWCDC's write() drops data whenever
  its `connected` flag is false, and only its RX interrupt sets that flag again.
- Render: JPEG strips go to the panel by GDMA while the next strip decodes, and the last
  strip of frame N overlaps decode of N+1. Nothing is copied per frame. Only KEEP-flagged
  frames are copied into the snapshot buffer (a PSRAM copy costs 9 ms a frame).
- Flow: the host keeps at most two frames not yet shown and times each capture so it lands
  as the watch frees up.

## Gotchas
- The Claude Face bridge daemon probes the port every ~4 s. The companion takes the port with
  TIOCEXCL; ewt/flash may need a retry while the bridge runs.
- `--monitor` (virtual second display): macOS won't run a desktop narrower than 800 pt. Once
  it would not come online with a 4K monitor attached, though later it did.

## Input back to the Mac (2026-10-11)
While a host is talking to the app (any packet in the last 3 s), the app sends touch
(`>>T d|m|u X Y`, panel pixels; the chip's 4095 "no position" samples are dropped), gestures
(`>>G`), the side button (`>>B d|u|s`) and the accelerometer (`>>I`, raw counts at 4096/g,
50 Hz). It costs the stream nothing: the replies use the IN direction, which is otherwise idle.
Swipe-right is not "back" in this app (controller.cpp). A 1.2 s button hold exits. Host-to-watch
haptics are packet type 4. After a ping with seq 0 the watch sends `>>ID <MAC>`, so a host can
keep several watches apart. The Mac side is `companions/screenshare/ewbridge` (see the README there).
