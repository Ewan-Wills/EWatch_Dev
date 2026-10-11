"""Client library for programs that draw on the watch through ewbridge.

    from ewbridge.client import Client
    c = Client("mygame")                 # connects to the running bridge
    w, h = c.size                        # the watch picture: 280x240 landscape / 240x280 portrait
    while True:
        for ev in c.poll():              # watch input: touch / button / imu / gesture / config / focus
            ...
        c.frame(rgb)                     # h x w x 3 uint8 numpy array (or bytes)
        c.haptic(40, 200)                # buzz the watch: ms, strength 0-255

Wire format (TCP 127.0.0.1:7482), so any language can do this: each message is
uint32 LE length (of what follows), one kind byte, then the body.
  client -> bridge   'H' JSON {"name", "title", "watch"}  hello (first message; watch optional)
                     'F' uint16 w, uint16 h, w*h*3 RGB  a frame
                     'J' JSON {"cmd": "haptic", "ms", "strength"} | {"cmd": "focus", "watch"}
  bridge -> client   'J' JSON event (see ewbridge/core.py: Bridge._input)
"""
import json
import socket
import struct
import threading
import queue

import numpy as np

APP_PORT = 7482


class Client:
    def __init__(self, name, title=None, watch=None, host="127.0.0.1", port=APP_PORT, timeout=5.0):
        """watch: id of the watch to show on (None: the first one)."""
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(None)
        self.events = queue.Queue(maxsize=2000)
        self.size = (280, 240)
        self.focused = False
        self.closed = False
        self._wlock = threading.Lock()
        self._send(b"H", json.dumps({"name": name, "title": title or name, "watch": watch}).encode())
        self._got_config = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()
        self._got_config.wait(timeout)

    # -- outgoing -------------------------------------------------------------
    def _send(self, kind, body):
        with self._wlock:
            self.sock.sendall(struct.pack("<I", len(body) + 1) + kind + body)

    def frame(self, rgb):
        """Send one frame: h x w x 3 uint8 (numpy array or bytes)."""
        a = np.ascontiguousarray(rgb, dtype=np.uint8)
        h, w = a.shape[:2]
        self._send(b"F", struct.pack("<HH", w, h) + a.tobytes())

    def haptic(self, ms=40, strength=200):
        self._send(b"J", json.dumps({"cmd": "haptic", "ms": int(ms), "strength": int(strength)}).encode())

    def focus(self, watch=None):
        """Ask the bridge to show this client on a watch (default: its own)."""
        self._send(b"J", json.dumps({"cmd": "focus", "watch": watch}).encode())

    # -- incoming -------------------------------------------------------------
    def poll(self):
        """Events since the last call (list of dicts)."""
        out = []
        while True:
            try:
                out.append(self.events.get_nowait())
            except queue.Empty:
                return out

    def _reader(self):
        buf = b""
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                buf += d
                while len(buf) >= 5:
                    n = struct.unpack("<I", buf[:4])[0]
                    if len(buf) < 4 + n:
                        break
                    kind, body = buf[4:5], buf[5:4 + n]
                    buf = buf[4 + n:]
                    if kind == b"J":
                        ev = json.loads(body)
                        if ev.get("type") == "config":
                            self.size = (ev["w"], ev["h"])
                            self._got_config.set()
                        elif ev.get("type") == "focus":
                            self.focused = ev["active"]
                        try:
                            self.events.put_nowait(ev)
                        except queue.Full:
                            pass
        except OSError:
            pass
        self.closed = True
        try:
            self.events.put_nowait({"type": "closed"})
        except queue.Full:
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
