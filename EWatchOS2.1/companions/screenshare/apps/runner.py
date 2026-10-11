"""Runs an App: a pygame window on the Mac (input + preview) and its frames.

Two ways:
  python -m apps.runner NAME     a client of the running ewbridge (frames to the
                                 watch, watch touch/button/IMU back into Input)
  screenshare.py --app NAME      in-process: Runner.grab() feeds the streamer
pygame wants the main thread on macOS, so the window loop lives there.
"""
import sys
import threading
import time

import numpy as np

from .base import Input

PAD_BUTTONS = {0: "a", 1: "b", 2: "x", 3: "y", 4: "back", 5: "guide", 6: "start",
               9: "lb", 10: "rb", 11: "up", 12: "down", 13: "left", 14: "right"}
PAD_AXES = {0: "lx", 1: "ly", 2: "rx", 3: "ry", 4: "lt", 5: "rt"}


def _watch_event(inp, ev, app):
    """Fold one ewbridge event into this frame's Input."""
    t = ev.get("type")
    if t == "touch":
        x, y = ev["x"], ev["y"]
        if ev["phase"] == "up":
            inp.touch = None
            if app.touch_as_mouse:
                inp.held.discard(1)
        else:
            inp.touch = (x, y)
            if ev["phase"] == "down":
                inp.tapped = True
            if app.touch_as_mouse:
                inp.mouse = (x, y)
                if ev["phase"] == "down":
                    inp.held.add(1)
                    inp.clicked.add(1)
    elif t == "button":
        if ev["phase"] == "down":
            inp.wbutton = True
        elif ev["phase"] == "up":
            inp.wbutton = False
        elif ev["phase"] == "short":
            inp.wpressed = True
    elif t == "imu":
        inp.tilt = (ev["ax"], ev["ay"], ev["az"])
    elif t == "gesture":
        inp.gestures.append(ev["name"])


def run_loop(cls, w, h, scale, publish, stop, client=None):
    """The window + app loop. publish(frame) gets every frame (h x w x 3)."""
    import pygame
    pygame.init()
    pygame.joystick.init()
    app = cls(w, h)
    if client:
        app._haptic = client.haptic
    win = pygame.display.set_mode((w * scale, h * scale))
    app.surface = pygame.Surface((w, h))
    pads = {}
    inp = Input()
    clock = pygame.time.Clock()
    last = time.perf_counter()
    mac_mouse = None
    while not stop.is_set():
        inp.clicked, inp.pressed, inp.wheel = set(), set(), 0
        inp.tapped, inp.wpressed, inp.gestures = False, False, []
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                stop.set()
            elif e.type == pygame.MOUSEBUTTONDOWN and e.button in (1, 2, 3):
                inp.held.add(e.button)
                inp.clicked.add(e.button)
            elif e.type == pygame.MOUSEBUTTONUP:
                inp.held.discard(e.button)
            elif e.type == pygame.MOUSEWHEEL:
                inp.wheel += e.y
            elif e.type == pygame.KEYDOWN:
                k = pygame.key.name(e.key)
                inp.keys.add(k)
                inp.pressed.add(k)
            elif e.type == pygame.KEYUP:
                inp.keys.discard(pygame.key.name(e.key))
            elif e.type == pygame.JOYDEVICEADDED:
                j = pygame.joystick.Joystick(e.device_index)
                pads[j.get_instance_id()] = j
            elif e.type == pygame.JOYDEVICEREMOVED:
                pads.pop(e.instance_id, None)
            elif e.type == pygame.JOYBUTTONDOWN and e.button in PAD_BUTTONS:
                inp.pad.add(PAD_BUTTONS[e.button])
            elif e.type == pygame.JOYBUTTONUP and e.button in PAD_BUTTONS:
                inp.pad.discard(PAD_BUTTONS[e.button])
            elif e.type == pygame.JOYAXISMOTION and e.axis in PAD_AXES:
                inp.axes[PAD_AXES[e.axis]] = e.value
        # Mac mouse: only when it actually moves, so a watch touch isn't overridden.
        if pygame.mouse.get_focused():
            mx, my = pygame.mouse.get_pos()
            m = (mx / scale, my / scale)
            if m != mac_mouse:
                inp.mouse = mac_mouse = m
        if client:
            for ev in client.poll():
                if ev.get("type") == "config" and (ev["w"], ev["h"]) != (w, h):
                    w, h = ev["w"], ev["h"]          # orientation changed: restart the app at the new size
                    app = cls(w, h)
                    app._haptic = client.haptic
                    win = pygame.display.set_mode((w * scale, h * scale))
                    app.surface = pygame.Surface((w, h))
                elif ev.get("type") == "closed":
                    stop.set()
                else:
                    _watch_event(inp, ev, app)
            on = "on the watch" if client.focused else "not on the watch"
            pygame.display.set_caption(f"{app.title} - EWatch ({on})")
        else:
            pygame.display.set_caption(f"{app.title} - EWatch")

        now = time.perf_counter()
        dt, last = min(now - last, 0.1), now
        app.update(dt, inp)
        out = app.draw()
        if out is None:
            out = pygame.surfarray.array3d(app.surface).swapaxes(0, 1)
            surf = app.surface
        else:
            surf = pygame.surfarray.make_surface(np.ascontiguousarray(out).swapaxes(0, 1))
        publish(np.ascontiguousarray(out, dtype=np.uint8))
        win.blit(pygame.transform.scale(surf, win.get_size()), (0, 0))
        pygame.display.flip()
        clock.tick(app.fps)
    pygame.quit()


class Runner:
    """In-process (screenshare.py --app): grab() is the streamer's source."""

    def __init__(self, name, w, h, scale=3):
        from . import load
        self.cls = load(name)
        self.w, self.h, self.scale = w, h, max(1, scale)
        self.frame = None
        self.lock = threading.Lock()
        self.ready = threading.Event()

    def grab(self):
        """Latest frame (h x w x 3 uint8). Waits for the first one."""
        if not self.ready.wait(10):
            raise RuntimeError("app produced no frame in 10 s")
        with self.lock:
            return self.frame

    def _publish(self, f):
        with self.lock:
            self.frame = f
        self.ready.set()

    def run(self, stop):
        run_loop(self.cls, self.w, self.h, self.scale, self._publish, stop)


def main():
    """python -m apps.runner NAME [--scale N]: run NAME as an ewbridge client."""
    import argparse
    from . import load
    from ewbridge.client import Client
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--watch", help="watch id to show on (default: the first)")
    a = ap.parse_args()
    cls = load(a.name)
    try:
        c = Client(a.name, cls.title, watch=a.watch)
    except OSError:
        sys.exit("ewbridge isn't running (start it: ./ewbridge up)")
    w, h = c.size
    try:
        run_loop(cls, w, h, a.scale, c.frame, threading.Event(), client=c)
    except KeyboardInterrupt:
        pass
    c.close()


if __name__ == "__main__":
    main()
