# HeadFace

Stand-alone EWatch v2 firmware: **Ewan's own head, from his CT scan (11/09/2026),
turning under the time.** Each full turn peels one layer deeper, with a glowing
scan plane sweeping down the head to reveal the next:

skin → skull → brain → ventricles (inside a ghost of the brain) → skin …

Calcifications are left out (too small to see at 240 px). The skin is the real
CT surface only: the scan stops at eye level, and the reconstructed lower face
(`skin_reconstructed_lower`) is not used.

| Input | Does |
|---|---|
| tap / short button press | clock on/off |
| swipe left / right | jump to the next / previous layer |
| hold button 2 s | power off |

- 384 frames (4 turns × 96, 3.75° steps), 240×280 baseline JPEG, played at
  20 fps: one turn = 4.8 s, the whole loop = 19.2 s. The last 30 frames of each
  turn are the wipe into the next layer.
- Clock: HH:MM in anti-aliased Space Grotesk Bold (OFL, `tools/OFL-SpaceGrotesk.txt`),
  date above, layer name with a colour dot at the bottom. Time from the RV-3028
  RTC (set it with AuroraOS).
- Core 0 decodes the next frame into PSRAM while core 1 shows the current one
  (two 134 KB slots); the display is pushed only when the frame or minute changes.

## Build / flash
```bash
pio run -t upload && pio device monitor
```
The Claude Face bridge (`launchd com.ewills.claude-bridge`) holds the watch's
serial port: `launchctl bootout gui/$(id -u)/com.ewills.claude-bridge` first,
`launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.ewills.claude-bridge.plist` after.
The monitor prints `pushfps= headfps= busy= late= decodeFail=` every 5 s;
healthy is headfps ≈ 20.0 and late = 0.

## Rebuild the frames
1. Render (Blender 5.2, Cycles/Metal, ~17 min for 384 frames):
   ```bash
   cd ~/Documents/ewan-ct-scan/watchface
   /Applications/Blender.app/Contents/MacOS/Blender -b ../3d-model/head_all_parts.blend \
       --python render_head.py -- --frames 96 --samples 128 --out renders
   ```
   `render_head.py` sets the materials (subsurface skin, ivory bone, wet pink
   brain, blue CSF ventricles + X-ray brain ghost), lights, camera and turntable,
   and drops the ventricle mask's non-ventricle CSF islands (comment in the script).
2. Pack: `uv run --with pillow --with numpy tools/build_frames.py [renders_dir] [--budget BYTES]`
   picks the highest JPEG quality that fits the budget (default 3.70 MB).
3. Font (only if changing sizes): `uv run --with pillow tools/build_font.py`.

The CT data and renders stay in `~/Documents/ewan-ct-scan/` (patient data, not in
any repo). `src/frames_data.h` is generated from them.

## Host checks (no watch needed)
```bash
test/render.sh
```
Frame timing over two whole loops incl. the wrap; a slow decoder makes the head
wait (never skip); swipes land on the first frame of the target layer; every
frame decodes through the firmware's JPEGDEC path and matches Pillow; a minute
tick only touches the last digit. Writes clock-over-head PNGs to `test/out/`.
Every checker is first run against broken input to prove it can fail.

Watch-exact preview video of one loop:
```bash
BIN=${TMPDIR:-/tmp}/headface_host; $BIN test/out --video /tmp/hf.raw
ffmpeg -f rawvideo -pix_fmt rgb565le -s 240x280 -r 20 -i /tmp/hf.raw -vf scale=480:560:flags=neighbor -pix_fmt yuv420p headface_preview.mp4
```

## Files
- `src/main.cpp` hardware, tasks, frame loop, input
- `src/head.*` frame player + decode slots (host-testable)
- `src/clock_draw.*` anti-aliased clock (host-testable, no display library)
- `src/frames_data.h`, `src/font_data.h` generated, do not edit
- `partitions_maxapp.csv` one 4 MB factory app, no OTA/filesystem
