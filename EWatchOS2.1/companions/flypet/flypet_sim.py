#!/usr/bin/env python3
"""flypet_sim — a Mac-side fly "brain" that drives the watch pet over USB.

This is a small, self-contained, *behaviour-inspired* state machine. It is a
nod to the Drosophila connectome work, NOT the real connectome: there are no
datasets here, just a handful of behaviours (walk / groom / rest / feed) and
drives (hunger / energy / mood) wired into a tiny finite-state model. The watch
draws whatever we send; the watch can also run itself when this script is not
connected.

Protocol (newline-terminated ASCII, 115200 8N1):
    PC -> watch :  FLY action=<walk|groom|rest|feed|wander>
                       x=<0..100> y=<0..100>
                       hunger=<0..100> energy=<0..100> mood=<0..100>
                       [state=<label>]
    watch -> PC :  lines prefixed with ">>" are telemetry/events:
                       >>HELLO flypet v1
                       >>STAT hunger=.. energy=.. mood=.. action=.. x=.. y=..
                       >>FEED x=.. y=..            (user tapped to feed)
                       >>BYE flypet
    (any other watch output — boot logs etc. — is ignored.)

Run:   python3 flypet_sim.py            # auto-detect the watch port
       python3 flypet_sim.py --port /dev/cu.usbmodem1101
"""
import argparse
import glob
import random
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed — run:  pip install -r requirements.txt")


# --------------------------------------------------------------------------
# The "brain": drives + a behaviour state machine.
# --------------------------------------------------------------------------
class FlyBrain:
    ACTIONS = ("walk", "groom", "rest", "feed", "wander")

    def __init__(self):
        self.x = 50.0
        self.y = 55.0
        self.tx = 50.0
        self.ty = 55.0
        self.action = "wander"
        self.state = "idle"
        self.hunger = 30.0     # 0 full .. 100 starving
        self.energy = 85.0     # 100 full .. 0 exhausted
        self.mood = 70.0       # 100 happy .. 0 miserable
        self.food = None       # (x, y) crumb dropped by the user
        self._until = 0.0      # when the current behaviour ends

    # choose a new behaviour based on the current drives
    def _choose(self, now):
        if self.food is not None and self.hunger > 25:
            self.action, self.state = "feed", "seeking food"
            self.tx, self.ty = self.food
            self._until = now + 4.0
            return
        if self.energy < 20:
            self.action, self.state = "rest", "exhausted"
            self._until = now + random.uniform(3, 6)
            return
        if self.hunger > 70:
            # hungry and nothing to eat — restless pacing
            self.action, self.state = "walk", "hungry"
            self.tx, self.ty = random.uniform(8, 92), random.uniform(10, 90)
            self._until = now + random.uniform(1.0, 2.0)
            return
        r = random.random()
        if r < 0.5:
            self.action, self.state = "walk", "exploring"
            self.tx, self.ty = random.uniform(8, 92), random.uniform(10, 90)
            self._until = now + random.uniform(1.5, 3.5)
        elif r < 0.78:
            self.action, self.state = "groom", "preening"
            self._until = now + random.uniform(1.5, 3.0)
        else:
            self.action, self.state = "rest", "basking"
            self._until = now + random.uniform(1.2, 2.5)

    def tick(self, now, dt):
        if now >= self._until:
            self._choose(now)

        # move toward target when walking/feeding
        if self.action in ("walk", "feed"):
            dx, dy = self.tx - self.x, self.ty - self.y
            dist = (dx * dx + dy * dy) ** 0.5
            if dist > 1.0:
                spd = 34.0 if self.action == "feed" else 24.0
                step = min(spd * dt, dist)
                self.x += dx / dist * step
                self.y += dy / dist * step
            elif self.action == "feed" and self.food is not None:
                # arrived at the crumb — eat it down
                self.hunger = max(0.0, self.hunger - 40 * dt)
                if self.hunger <= 4:
                    self.food = None
                    self.mood = min(100.0, self.mood + 10)

        # drives drift over time
        self.hunger = min(100.0, self.hunger + 1.5 * dt)
        if self.action == "walk":
            self.energy = max(0.0, self.energy - 1.2 * dt)
        elif self.action in ("rest", "groom"):
            self.energy = min(100.0, self.energy + 2.5 * dt)
        target_mood = 100 - self.hunger / 2 - (100 - self.energy) / 3
        self.mood += (target_mood - self.mood) * min(1.0, dt * 0.5)
        self.mood = max(0.0, min(100.0, self.mood))

    def command(self):
        return ("FLY action={a} x={x} y={y} hunger={h} energy={e} mood={m} "
                "state={s}\n").format(
            a=self.action, x=int(self.x), y=int(self.y),
            h=int(self.hunger), e=int(self.energy), m=int(self.mood),
            s=self.state.replace(" ", "_"))

    # user tapped the watch to feed → plant a crumb
    def feed(self, x, y):
        self.food = (float(x), float(y))
        self.state = "fed by hand"


# --------------------------------------------------------------------------
# Serial glue
# --------------------------------------------------------------------------
def autodetect_port():
    cands = sorted(glob.glob("/dev/cu.usbmodem*") +
                   glob.glob("/dev/cu.usbserial*") +
                   glob.glob("/dev/ttyACM*") +
                   glob.glob("/dev/ttyUSB*"))
    return cands[0] if cands else None


def main():
    ap = argparse.ArgumentParser(description="Mac-side fly brain for the EWatch flypet app.")
    ap.add_argument("--port", help="serial port (default: auto-detect watch)")
    ap.add_argument("--rate", type=float, default=10.0,
                    help="commands per second to stream (default 10)")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed for repeatable runs")
    ap.add_argument("--quiet", action="store_true", help="don't echo watch telemetry")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    port = args.port or autodetect_port()
    if not port:
        sys.exit("No serial port found. Plug in the watch or pass --port.")

    try:
        ser = serial.Serial(port, 115200, timeout=0)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}")

    print(f"flypet brain -> {port}  ({args.rate:.0f} cmd/s)  Ctrl-C to stop")
    brain = FlyBrain()
    period = 1.0 / args.rate
    last = time.monotonic()
    rxbuf = b""

    try:
        while True:
            now = time.monotonic()
            dt = now - last
            last = now

            # read any telemetry/events from the watch
            try:
                data = ser.read(256)
            except serial.SerialException:
                print("serial read error — watch unplugged?")
                break
            if data:
                rxbuf += data
                while b"\n" in rxbuf:
                    line, rxbuf = rxbuf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if not text.startswith(">>"):
                        continue  # ignore boot logs / noise
                    if not args.quiet:
                        print("  watch:", text)
                    if text.startswith(">>FEED"):
                        fx = fy = None
                        for tok in text.split():
                            if tok.startswith("x="):
                                fx = tok[2:]
                            elif tok.startswith("y="):
                                fy = tok[2:]
                        if fx is not None and fy is not None:
                            try:
                                brain.feed(float(fx), float(fy))
                            except ValueError:
                                pass

            brain.tick(now, dt)
            try:
                ser.write(brain.command().encode("ascii"))
            except serial.SerialException:
                print("serial write error — watch unplugged?")
                break
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nstopping.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
