"""Helper process that owns the virtual second monitor (CGVirtualDisplay).

    python -m ewbridge.vdisplay W H [left|right]

Prints one JSON line ({"did", "desc"} or {"error"}) and keeps the display
alive until its stdin closes (the bridge exits or removes the monitor). Kept
out of the bridge process because macOS won't bring a new virtual display
online in a process that has just released one; a fresh process (after a
~15 s system cooldown) can.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))


def main():
    w, h = int(sys.argv[1]), int(sys.argv[2])
    side = (sys.argv[3] if len(sys.argv) > 3 else "") or None
    real_argv, sys.argv = sys.argv, ["vdisplay"]
    import screenshare as ss
    try:
        m = ss.MonitorSource(w, h, "fit", 1.0, side)
    except SystemExit as e:
        print(json.dumps({"error": str(e)}), flush=True)
        return 1
    print(json.dumps({"did": int(m.did), "desc": m.desc.split(", whole monitor")[0]}), flush=True)
    sys.stdin.read()                       # until the bridge closes our stdin (or dies)
    m.vd = None
    return 0


if __name__ == "__main__":
    sys.exit(main())
