# ewmouse — EWatchOS2.1 + Bluetooth LE mouse

Mouse app (launcher "Mouse") pairs a BLE mouse; the cursor then works on every
screen. Design (btmouse.h): the mouse is a virtual finger injected at the
controller input layer (same place as ewsim), so no view changed:
- left press/hold/release = touch down/hold/up at the cursor; click = tap,
  double-click = double tap, hold still 0.8 s = long press, quick flick-drag
  (>50 px in <450 ms) = swipe gesture, slow drag = plain drag (sliders don't
  fire swipe-right "back")
- wheel = short finger flick at the cursor (lists scroll with their physics);
  horizontal wheel = swipe left/right
- right = back (virtual side button), middle = home (watch face), button 4 = back
- cursor composited on the render task from the ewsim bus-tap shadow with the
  tap paused (shadow never holds cursor pixels); hides after 5 s idle;
  orange while left is held; ~60 Hz while moving
- report map parsed for X/Y/wheel/AC-pan/buttons (any bit layout, report IDs);
  boot layout fallback
- Bluedroid client: encryption level + BLESecurity Just Works bond, waits for
  onAuthenticationComplete before GATT, getCharacteristicsByHandle (duplicate
  0x2A4D), 0x2908 report refs, address type from scan.
- Shared stack refcount `src/core/ble_stack.*` so the Bluetooth remote app and
  the mouse don't pull BLE from each other.
- While connected: fake sleep (no real light sleep), WiFi suspended, CPU pinned.
- Test hook: console `msim dx dy [buttons] [wheel] [pan]`, `msimto x y`;
  test/mouse.ewt. Snaps never show the cursor (by design).
- Flash 73.3%. Builds 2026-10-07. NOT yet run on hardware or with a real mouse.

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
