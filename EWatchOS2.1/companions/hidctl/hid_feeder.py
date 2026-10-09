#!/usr/bin/env python3
"""hidctl serial feeder — stream the Mac's real keyboard + mouse to the watch.

This is the FALLBACK transport for the hidctl app (see ../NOTES.md). USB-host
is not wired on the watch and a BLE-HID central was out of scope for the MVP,
so the on-watch Notes editor + cursor overlay are driven over USB-CDC instead.
This script reads your keyboard and mouse with `pynput` and sends the watch's
tiny newline-framed protocol:

    KC <n>        key char: decimal ASCII 32..126      (e.g. "KC 65" -> 'A')
    KS <NAME>     special: ENTER BACK TAB ESC LEFT RIGHT UP DOWN HOME END
    MM <dx> <dy>  relative mouse move, signed
    MB <0|1>      left button: 1 = press (click), 0 = release

Open the Notes app on the watch first (swipe up -> "Notes"). The watch prints
">> HIDCTL ready" once when the screen opens.

macOS: pynput needs Accessibility AND Input Monitoring permission for your
terminal app. System Settings -> Privacy & Security -> Accessibility (and
-> Input Monitoring) -> enable your terminal (Terminal / iTerm). See README.md.

Usage:
    python3 hid_feeder.py [--port /dev/cu.usbmodemXXXX] [--no-mouse]
                          [--no-keyboard] [--exclusive] [--mouse-scale 1.0]

Ctrl-C in this terminal stops the feeder. If you run with --exclusive the
keystrokes are swallowed from the rest of the Mac (so you only type into the
watch); without it, what you type also goes to whatever app is focused.
"""
import argparse
import glob
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed. Run: pip install -r requirements.txt")

try:
    from pynput import keyboard, mouse
except ImportError:
    sys.exit("pynput not installed. Run: pip install -r requirements.txt")


def autodetect_port():
    cands = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
                   + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return cands[0] if cands else None


SPECIALS = {
    keyboard.Key.enter: "ENTER",
    keyboard.Key.backspace: "BACK",
    keyboard.Key.tab: "TAB",
    keyboard.Key.esc: "ESC",
    keyboard.Key.left: "LEFT",
    keyboard.Key.right: "RIGHT",
    keyboard.Key.up: "UP",
    keyboard.Key.down: "DOWN",
    keyboard.Key.home: "HOME",
    keyboard.Key.end: "END",
    keyboard.Key.space: None,   # handled as a printable char (ASCII 32)
}


class Feeder:
    def __init__(self, ser, mouse_scale):
        self.ser = ser
        self.lock = threading.Lock()
        self.mouse_scale = mouse_scale
        self._last_mouse = None
        self._accum = [0.0, 0.0]
        self._stop = False

    def send(self, line):
        with self.lock:
            try:
                self.ser.write((line + "\n").encode("ascii", "ignore"))
            except serial.SerialException as e:
                print(f"serial write failed: {e}", file=sys.stderr)
                self._stop = True

    # ---- keyboard ----
    def on_press(self, key):
        if isinstance(key, keyboard.KeyCode) and key.char is not None:
            ch = key.char
            if len(ch) == 1 and 32 <= ord(ch) <= 126:
                self.send(f"KC {ord(ch)}")
            return
        if key == keyboard.Key.space:
            self.send("KC 32")
            return
        name = SPECIALS.get(key)
        if name:
            self.send(f"KS {name}")

    # ---- mouse ----
    def on_move(self, x, y):
        if self._last_mouse is None:
            self._last_mouse = (x, y)
            return
        dx = (x - self._last_mouse[0]) * self.mouse_scale
        dy = (y - self._last_mouse[1]) * self.mouse_scale
        self._last_mouse = (x, y)
        self._accum[0] += dx
        self._accum[1] += dy

    def flush_mouse(self):
        # Coalesce accumulated motion ~60 Hz so we don't flood the link.
        while not self._stop:
            time.sleep(1.0 / 60.0)
            idx = int(round(self._accum[0]))
            idy = int(round(self._accum[1]))
            if idx or idy:
                # clamp per-packet so one big jump doesn't warp the cursor
                idx = max(-64, min(64, idx))
                idy = max(-64, min(64, idy))
                self._accum[0] -= idx
                self._accum[1] -= idy
                self.send(f"MM {idx} {idy}")

    def on_click(self, x, y, button, pressed):
        if button == mouse.Button.left:
            self.send(f"MB {1 if pressed else 0}")


def main():
    ap = argparse.ArgumentParser(description="Stream keyboard+mouse to the EWatch hidctl app.")
    ap.add_argument("--port", help="serial port (default: auto-detect /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=115200, help="baud (CDC ignores it; default 115200)")
    ap.add_argument("--no-mouse", action="store_true", help="don't forward the mouse")
    ap.add_argument("--no-keyboard", action="store_true", help="don't forward the keyboard")
    ap.add_argument("--exclusive", action="store_true",
                    help="swallow keystrokes from the rest of the Mac (keyboard only)")
    ap.add_argument("--mouse-scale", type=float, default=1.0, help="mouse sensitivity (default 1.0)")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if not port:
        sys.exit("No serial port found. Plug in the watch and/or pass --port.")
    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}")
    print(f"Feeding keyboard+mouse to {port}. Open 'Notes' on the watch. Ctrl-C to stop.")

    fe = Feeder(ser, args.mouse_scale)

    listeners = []
    if not args.no_keyboard:
        listeners.append(keyboard.Listener(on_press=fe.on_press, suppress=args.exclusive))
    mouse_thread = None
    if not args.no_mouse:
        listeners.append(mouse.Listener(on_move=fe.on_move, on_click=fe.on_click))
        mouse_thread = threading.Thread(target=fe.flush_mouse, daemon=True)
        mouse_thread.start()

    for l in listeners:
        l.start()
    try:
        while not fe._stop:
            time.sleep(0.2)
    except KeyboardInterrupt:
        print("\nstopping.")
    finally:
        fe._stop = True
        for l in listeners:
            l.stop()
        ser.close()


if __name__ == "__main__":
    main()
