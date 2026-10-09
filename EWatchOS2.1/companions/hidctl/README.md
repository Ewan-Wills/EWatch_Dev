# hidctl PC feeder

`hid_feeder.py` streams your Mac's real keyboard and mouse to the watch's
`hidctl` (Notes) app over USB serial. This is the **fallback transport** — the
watch cannot be a USB host and a BLE-HID central was out of scope for the MVP
(see `../NOTES.md`). The on-watch UI (Notes editor + mouse cursor overlay) is
the real deliverable; this script just feeds it input so the demo works today.

## Setup

```sh
cd pc
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

### macOS permissions (required)

`pynput` reads global input, so macOS must grant your **terminal app** two
permissions. System Settings -> Privacy & Security:

1. **Accessibility** -> enable Terminal (or iTerm).
2. **Input Monitoring** -> enable Terminal (or iTerm).

Quit and reopen the terminal after granting. Without these the script runs but
sees no key/mouse events.

## Run

1. Flash the watch (see `../NOTES.md`) and plug it in over USB.
2. On the watch: swipe up from the clock -> tap **Notes**. The watch prints
   `>> HIDCTL ready` on the serial line when the screen opens.
3. Start the feeder:

```sh
python3 hid_feeder.py                    # auto-detects /dev/cu.usbmodem*
python3 hid_feeder.py --port /dev/cu.usbmodem1101
python3 hid_feeder.py --no-mouse         # keyboard only
python3 hid_feeder.py --exclusive        # keystrokes go ONLY to the watch
python3 hid_feeder.py --mouse-scale 0.6  # slower cursor
```

Type on your keyboard -> characters appear in the watch's Notes buffer. Move
the mouse -> the on-screen cursor moves. Left-click -> the cursor "taps" the UI
(the `Clear` / `Save` buttons and the back chevron are clickable).

Press **Ctrl-C** in the terminal to stop.

## Notes

- Without `--exclusive`, whatever you type also goes to the focused Mac app
  (expected — it is a global listener). Use `--exclusive` to swallow keystrokes
  so they only reach the watch. `--exclusive` affects the keyboard only.
- The same serial port is the flashing/monitor channel. Close any open
  `pio device monitor` before running the feeder, or they will fight over the port.
- Protocol (one command per line, `\n`-terminated ASCII), if you want to drive
  the watch from your own script instead:
  `KC <ascii>`, `KS <ENTER|BACK|TAB|ESC|LEFT|RIGHT|UP|DOWN|HOME|END>`,
  `MM <dx> <dy>`, `MB <0|1>`. Unknown lines are ignored by the watch.
