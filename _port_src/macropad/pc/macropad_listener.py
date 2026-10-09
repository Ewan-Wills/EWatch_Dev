#!/usr/bin/env python3
"""macropad_listener.py — EWatch Macropad PC companion (macOS).

Listens on the watch's USB-CDC serial port for `MACRO <id>` lines and runs the
action mapped to that id in macros.json (cmd+c, launch an app, a hotkey, ...).

Serial protocol (watch -> PC), newline-terminated ASCII, one per line:
    MACRO <id>\n        id = 0-based tile index, row-major.
Any other line (boot banners, firmware logs) is ignored.

Usage:
    python3 macropad_listener.py                      # auto-detect port
    python3 macropad_listener.py --port /dev/cu.usbmodem1101
    python3 macropad_listener.py --config macros.json
    python3 macropad_listener.py --list-ports
    python3 macropad_listener.py --dry-run            # print, don't execute

macOS: the first time an action fires, the Terminal/Python app will need
Accessibility (for keystrokes/hotkeys) and possibly Input Monitoring
permission. See README.md.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed. Run:  pip install -r requirements.txt")

BAUD = 115200          # native USB CDC ignores baud, but pyserial wants a value
RECONNECT_SECS = 2.0


# --------------------------------------------------------------------------- #
# Port discovery
# --------------------------------------------------------------------------- #
def discover_ports():
    """Return candidate macOS CDC ports, most-likely first."""
    ports = [p.device for p in list_ports.comports()]
    ports += glob.glob("/dev/cu.usbmodem*")
    # de-dup, keep usbmodem first
    seen, ordered = set(), []
    for p in ports:
        if p in seen:
            continue
        seen.add(p)
        ordered.append(p)
    ordered.sort(key=lambda p: (0 if "usbmodem" in p else 1, p))
    return ordered


def pick_port(explicit):
    if explicit:
        return explicit
    cands = discover_ports()
    if not cands:
        sys.exit("No serial ports found. Plug in the watch, or pass --port. "
                 "List them with --list-ports.")
    print(f"[macropad] auto-selected port {cands[0]}  "
          f"(override with --port; see all with --list-ports)")
    return cands[0]


# --------------------------------------------------------------------------- #
# Actions
# --------------------------------------------------------------------------- #
_pyautogui = None


def get_pyautogui():
    """Import pyautogui lazily — importing it on a headless box can fail, and we
    only need it for key/type actions, not app/shell ones."""
    global _pyautogui
    if _pyautogui is None:
        import pyautogui
        pyautogui.FAILSAFE = False
        _pyautogui = pyautogui
    return _pyautogui


def run_action(action, dry_run=False):
    kind = action.get("type")
    label = action.get("label", "")
    desc = f"{kind} {action.get('keys') or action.get('text') or action.get('name') or action.get('cmd') or ''}"
    print(f"[macropad] -> {label or desc}")
    if dry_run:
        return
    try:
        if kind == "hotkey":
            get_pyautogui().hotkey(*action["keys"])
        elif kind == "keys":
            for k in action["keys"]:
                get_pyautogui().press(k)
        elif kind == "type":
            get_pyautogui().typewrite(action["text"],
                                      interval=action.get("interval", 0.0))
        elif kind == "app":
            subprocess.run(["open", "-a", action["name"]], check=False)
        elif kind == "shell":
            subprocess.run(action["cmd"], shell=True, check=False)
        else:
            print(f"[macropad] ! unknown action type: {kind!r}")
    except Exception as e:  # noqa: BLE001 — keep the listener alive
        print(f"[macropad] ! action failed: {e}")


# --------------------------------------------------------------------------- #
# Main loop
# --------------------------------------------------------------------------- #
def load_config(path):
    with open(path, "r") as f:
        raw = json.load(f)
    # keys are strings in JSON; keep them as strings and match on str(id)
    return {k: v for k, v in raw.items() if not k.startswith("_")}


def serve(port, config, dry_run):
    print(f"[macropad] listening on {port} @ {BAUD}  "
          f"({len(config)} macros mapped){'  [DRY RUN]' if dry_run else ''}")
    print("[macropad] tap a tile on the watch; Ctrl-C to quit.")
    while True:
        try:
            with serial.Serial(port, BAUD, timeout=1) as ser:
                while True:
                    line = ser.readline().decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    if not line.startswith("MACRO"):
                        continue  # ignore boot/log noise
                    parts = line.split()
                    if len(parts) != 2 or not parts[1].isdigit():
                        print(f"[macropad] ? malformed: {line!r}")
                        continue
                    mid = parts[1]
                    action = config.get(mid)
                    if action is None:
                        print(f"[macropad] ? no mapping for id {mid}")
                        continue
                    run_action(action, dry_run)
        except serial.SerialException as e:
            print(f"[macropad] serial error: {e}; retrying in {RECONNECT_SECS}s")
            time.sleep(RECONNECT_SECS)


def main():
    ap = argparse.ArgumentParser(description="EWatch Macropad PC listener")
    ap.add_argument("--port", help="serial port (default: auto-detect usbmodem)")
    ap.add_argument("--config",
                    default=os.path.join(os.path.dirname(__file__), "macros.json"),
                    help="id->action JSON map (default: macros.json)")
    ap.add_argument("--list-ports", action="store_true",
                    help="list candidate serial ports and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the action but do not execute it")
    args = ap.parse_args()

    if args.list_ports:
        for p in discover_ports():
            print(p)
        return

    config = load_config(args.config)
    port = pick_port(args.port)
    try:
        serve(port, config, args.dry_run)
    except KeyboardInterrupt:
        print("\n[macropad] bye.")


if __name__ == "__main__":
    main()
