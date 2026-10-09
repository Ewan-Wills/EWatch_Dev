# Macropad — PC companion (macOS)

Listens on the watch's USB-serial port and turns each `MACRO <id>` line into a
real macOS action (copy, paste, a hotkey, launch an app, run a command).

## Install

```sh
cd pc
python3 -m venv .venv && source .venv/bin/activate   # optional but tidy
pip install -r requirements.txt
```

## Run

```sh
python3 macropad_listener.py                 # auto-detect the watch port
python3 macropad_listener.py --list-ports    # see candidate ports
python3 macropad_listener.py --port /dev/cu.usbmodem1101
python3 macropad_listener.py --dry-run       # print actions without firing them
```

Keep it running, then tap tiles on the watch. `Ctrl-C` to quit. If the watch is
unplugged/replugged the listener auto-reconnects.

> The watch's USB port is also the flash/monitor channel. Close any serial
> monitor (`pio ... -t monitor`, Arduino IDE, `screen`) before running the
> listener — only one process can own the port.

## macOS permissions (required for keystrokes)

`pyautogui` synthesises keyboard/mouse events, which macOS blocks until you
grant permission to the app that runs Python (usually **Terminal**, or your
IDE):

1. **System Settings -> Privacy & Security -> Accessibility** -> enable
   Terminal (or iTerm / VS Code — whatever you launched Python from).
2. If keys still don't register, also enable that same app under
   **Privacy & Security -> Input Monitoring**.
3. Quit and reopen the terminal app after granting, then run the listener
   again.

`app` actions (`open -a <Name>`) and `shell` actions do **not** need these
permissions; only `hotkey` / `keys` / `type` do. Use `--dry-run` first to
confirm the watch and mapping work before granting anything.

## Editing the macros

Edit `macros.json`. Keys are the macro ids (strings, `"0"`..`"N-1"`), in the
same order as `kMacroLabels` in `../src/apps/macropad_config.h`. Action types:

| type     | fields              | effect                                   |
|----------|---------------------|------------------------------------------|
| `hotkey` | `keys: [..]`        | press keys together (e.g. `command`,`c`) |
| `keys`   | `keys: [..]`        | press keys one after another             |
| `type`   | `text`, `interval?` | type literal text                        |
| `app`    | `name`              | `open -a <name>` (launch / focus)        |
| `shell`  | `cmd`               | run a shell command                      |

Key names are pyautogui names: `command`, `ctrl`, `option`/`alt`, `shift`,
`space`, `tab`, `enter`, `left`/`right`/`up`/`down`, `f1`..`f12`, letters, etc.

If you change the grid size or labels on the watch, update both
`macropad_config.h` (and reflash) and `macros.json` so the ids line up.
