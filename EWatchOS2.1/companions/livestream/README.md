# livestream companion

`stream_to_watch.py` streams 240x280 RGB565 frames (raw or LZ4) from the Mac to
the watch's Stream app over TCP port 7878 (`ewatch.local`). Build the watch with
`EW_APPS=livestream`, turn WiFi on (Client mode, same network as the Mac), open
**Stream** on the watch, then run the script. Protocol: see
`src/apps/modules/livestream/livestream.h`.

Ported from the `Live-Stream-App` branch (May 2026, ~5 fps with BetterDisplay).
