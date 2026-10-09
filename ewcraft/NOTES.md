# ewcraft — Craft (voxel block game) + Xbox controller over BLE

EWatchOS2.1 copy with the Bluedroid Bluetooth app removed (NimBLE and
Bluedroid can't share a firmware) and two apps added:
- "Xbox Pad" (`src/apps/xbox_pad.*`): NimBLE-Arduino 1.4.3 central for an Xbox
  Wireless Controller (BLE firmware 5.x). Pair: open the app, hold the pair
  button until the logo flashes fast. Bond kept in NVS; reconnects when the
  controller is switched on. Live view of sticks/triggers/buttons; best-effort
  rumble (output report 3). Report layout in xbox_pad.h (from asukiaaa and
  tbekas sources, see research below).
- "Craft" (`src/apps/craft.*`): clean-room per-pixel voxel raycaster (no code
  from Minecraft4k ports, which carry no licence). 64x64x32 island world in 4-bit
  blocks (64 KB internal RAM), 15 block types with procedural 16x16 textures,
  rendered 120x140 on both cores (helper task) and doubled into the frame
  canvas; fog, face shading, see-through water + glass, target outline,
  gravity/collision/jump/swim/fly, mine (RT) / place (LT), 9-slot hotbar.
  Touch fallback + attract mode. Controls in craft.h.
- Flash 60.0% (no Bluedroid). Builds 2026-10-07. NOT yet run on hardware.

## Other open-source Minecraft-likes (research 2026-10-07)
- ClassiCube (BSD, C99, software renderer, has GBA/32X builds at ~256 KB RAM):
  the realistic "real Minecraft Classic" port. Needs a new platform layer
  (window/input/ST7789 blit/PSRAM alloc) modelled on src/gba/; textures: Mojang's
  terrain.png can't be shipped, needs a free pack. Biggest job; next candidate.
- Minicraft GBA (GPL-3, C, 2D, 240 px wide): most complete game; GPL-3 would
  make the firmware GPL.
- Minecraft4k ports (no licence): technique only — what Craft reimplements.
- ESPCraft / esp32craft (no licence, early): reference only.

---

# EWatchOS2.1 notes

## 2026-10-05 - serial/render trap audit (traps A-E from bookreader)

Changed:
- Root fix for top/left text clipping (all views): GFX 1.4.9 drawChar, both
  GFXfont and the built-in glcdfont, only skips a glyph that is entirely
  off-screen; a partly-off glyph writes at negative x/y, before the canvas
  framebuffer (heap corruption). `src/drivers/safe_canvas.h` (SafeCanvas)
  clips every canvas write path; frameCanvas() and viewer3d use it. The audit
  found quickset's scrolling header (y=36..40+rise) hitting this; it needs no
  local guard now.
- `src/core/ewlog.cpp` `log dump`: held sLogMutex across the whole 256 KB
  Serial dump. With a host that isn't draining, each HWCDC write can block
  ~100 ms, so every task that logs would stall (trap E). The mutex is now
  taken per 256 B chunk (open/seek/read) and released before Serial.write.

Audited, no change needed:
- Trap A/B/C: the only Serial reader in the OS is the ewcon console itself.
  No OS view runs a PC protocol over Serial (photos upload via the web UI).
- Trap D, custom fonts: watchface Photo face (FreeSansBold24, baseline 96,
  top 61) and Mono face (FreeMono24, baseline 120) are in bounds;
  drawTitleBar draws FreeSans12/FreeMono12 at baseline >= 24 on the panel
  (gfx, not the canvas). Launcher already guards its scrolling text.
- Trap E: other locks (ModelLock, spotify art/spMutex, photo_store,
  wifi scan/job, storage known-list) are held only for short copies; the
  spotify art download runs outside artMutex.
Not flashed or tested on hardware.
