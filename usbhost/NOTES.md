# usbhost — USB Host scanner (step 1 towards a USB keyboard on the watch)

Copy of EWatchOS2.1 plus `src/apps/usbhost.{h,cpp}`, `Screen::UsbHost`, launcher
"USB Host". Opening it switches the USB-C port to USB host mode for a 30 s window
(starts 1.5 s after entering) and lists every device that enumerates. Results are
logged as `USBH` lines (VID:PID, USB version, class, speed, strings, every
interface) and stay on screen after the window. Side button ends the window
early; tap rescans. Tests: `test/README-usbhost.md`.

## How it works / why it's built this way
- The S3 has one internal USB PHY. Normally USB-Serial/JTAG owns it (console,
  ewt, flashing). The scan gives it to the OTG controller in host mode (ESP-IDF
  4.4 USB Host Library, `usb_new_phy` + `usb_host_install(skip_phy_setup)`),
  then uninstalls and hands it back (clears the USB_WRAP pull overrides, then the
  `usb_phy_ll_int_jtag_enable` register writes, done by hand because that header
  is C-only). So the Mac loses the serial port during the window only.
- If the port does NOT come back after a window: power-cycle (boot ROM restores
  Serial/JTAG) and note it here.

## Hardware results (2026-10-06)
- Serial/JTAG restore VERIFIED once: port gone 1.5 s after entering, back 30 s
  later with no reset (`state` up=89 s, still on UsbHost, after.ewt PASS).
  A second window in the same boot was not measured (Ewan swapped the Mac cable
  for the hub mid-test), so repeat-cycle restore is still UNVERIFIED.
- Host mode works through the watch's own USB-C: Ewan's powered hub enumerated as
  `Hub 2109:2817 FS` (VIA Labs VL817, USB2 side). Its keyboard was invisible, as
  expected with no hub driver.
- Keyboard driver (HID boot protocol, direct attach only) added after that: claims
  the 3/1/1 interface, SET_PROTOCOL(boot), polls the interrupt IN endpoint, US
  layout, typed text box on screen, `USBH key ...` log lines, host window stays
  open while a keyboard is attached. Builds; NOT yet run on hardware.
- 2026-10-06 later: rewrote the host on the HCD layer (`src/apps/idf447_hcd.h`,
  vendored from esp-idf v4.4.7; `usb_kbd_host.{h,cpp}` service): root device +
  one level of external hub (ports polled with GET_PORT_STATUS every 250 ms),
  LS-behind-FS-hub via the HCD's ls_via_fs_hub. VERIFIED on hardware: serial
  restore after a window (port back at 32 s, no reboot), and Ewan's keyboard
  typed through his VL817 hub ("that works", 2026-10-06).
- Bluetooth remote combo (built, flashed state below): Bluetooth app "Kbd" button
  starts the USB keyboard host; Space/Enter play-pause, Left/Right (N/P) skip,
  Up/Down volume, M mute -> BLE HID media keys.
- Control Centre: Bluetooth tile; while on, prev/play/next row at the top and
  "Connected / Not paired" in the header. The remote is now a background
  service (`btRemoteSetOn`, svc task re-advertises); opening the Bluetooth app
  turns it on, leaving keeps it on. While on: no real light sleep (fake sleep,
  like an active WiFi radio), WiFi suspended (BLE needs its RAM), CPU pinned at
  240 MHz -> noticeably more battery drain. RAM-only: off after reboot.
- Superseded below: the IDF-library-era notes.
- Hub options if direct attach isn't acceptable (now done via HCD): own hub driver on the HCD layer
  (hcd_* symbols are in libusb.a, header not shipped; LS-behind-FS-hub needs PRE
  support) or arduino-esp32 3.x / IDF 5.3+ with CONFIG_USB_HOST_HUBS_SUPPORTED.

## Hardware limits (decide the keyboard plan from these)
- The watch can't put 5 V on VBUS (no boost/load switch; hidctl/NOTES.md). The
  keyboard needs power from somewhere else.
- ESP-IDF 4.4 (arduino-esp32 2.0.17, our toolchain) has NO hub driver. A hub
  enumerates as one class-0x09 device; the keyboard behind it is invisible. Hub
  support arrived in IDF 5.3+ (arduino-esp32 3.x), which this codebase isn't on.
- Many self-powered hubs only connect their upstream port when they see VBUS from
  the host, which the watch doesn't provide. A USB-C hub with PD pass-through and
  a charger plugged in does put VBUS on its upstream plug (it charges the watch),
  so it's the hub most likely to show up at all, but it'll still show as "Hub".
- Most likely to work for the keyboard itself: a USB-C OTG adapter/Y-cable with a
  power input (C plug -> USB-A socket + charging port), keyboard plugged straight
  in, charger on the power input. Data goes directly watch <-> keyboard.

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
