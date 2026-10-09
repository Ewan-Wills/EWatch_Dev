#!/usr/bin/env python3
"""Find the EWatch on USB and flash it, autonomously and robustly.

Why this exists: the watch has no hard power switch and esptool's reset powers
it off after a flash, so flashing by hand is fiddly — you often have to hold the
button at the right moment. This tool waits for the port to appear, flashes, and
retries (so a first attempt where you forgot to hold the button just succeeds on
the next try when you do).

Usage:
    python3 flash.py <app>            # <app> = folder name under ShortsApps/, or a path
    python3 flash.py bookreader
    python3 flash.py . --find 60 --retry 120

Behaviour:
  1. Scan for a serial port (default /dev/cu.usbmodem*) for up to --find seconds.
  2. Run `pio run -e <env> -t upload` against it.
  3. If it fails (port vanished, button not held, sync error), re-scan and retry
     until --retry seconds have elapsed, then give up non-zero.

Output is intentionally terse (one line per phase) so it's cheap to run from an
agent. Use -v for the full pio/esptool log on failures.

Exit codes: 0 flashed, 2 no port found, 3 flashing failed within retry window.
"""
import argparse
import glob
import os
import subprocess
import sys
import time

DEF_CORE = "/Users/ewills/Documents/EWatch/.pio-core-shortsapps"
PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
PORT_GLOB = "/dev/cu.usbmodem*"
# esptool/pio strings that mean the flash genuinely completed.
OK_MARKERS = ("Hash of data verified", "[SUCCESS]")
# Strings that mean "try again" (device not ready / button not held).
RETRY_MARKERS = (
    "Failed to connect", "No serial data received", "Wrong boot mode",
    "Timed out waiting", "could not open", "A fatal error occurred",
    "Errno", "resource busy", "Serial port", "No such file",
)


def log(msg):
    print(f"[flash] {msg}", flush=True)


def find_port(timeout):
    """Return the first matching serial port, waiting up to `timeout` s."""
    deadline = time.time() + timeout
    announced = False
    while time.time() < deadline:
        ports = sorted(glob.glob(PORT_GLOB))
        if ports:
            return ports[0]
        if not announced:
            log(f"waiting for watch on {PORT_GLOB} (up to {timeout}s)…")
            announced = True
        time.sleep(1)
    return None


def attempt_flash(project, env, port, core_dir, verbose):
    """One pio upload attempt. Return (ok: bool, tail: str)."""
    envv = dict(os.environ, PLATFORMIO_CORE_DIR=core_dir)
    cmd = [PIO, "run", "-e", env, "-t", "upload", "--upload-port", port]
    try:
        p = subprocess.run(cmd, cwd=project, env=envv,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=240)
        out = p.stdout or ""
    except subprocess.TimeoutExpired:
        return False, "pio upload timed out (240s)"
    ok = p.returncode == 0 and any(m in out for m in OK_MARKERS)
    if verbose and not ok:
        sys.stdout.write(out)
    # Terse tail: the most informative line.
    tail = ""
    for line in reversed(out.splitlines()):
        if any(m in line for m in OK_MARKERS + RETRY_MARKERS) or "Error" in line:
            tail = line.strip()
            break
    return ok, tail


def main():
    ap = argparse.ArgumentParser(description="Find and flash the EWatch, with retries.")
    ap.add_argument("app", help="folder under ShortsApps/ (e.g. bookreader) or a project path")
    ap.add_argument("-e", "--env", default="ewatch", help="PlatformIO env (default: ewatch)")
    ap.add_argument("--find", type=int, default=60, help="seconds to wait for the port (default 60)")
    ap.add_argument("--retry", type=int, default=120, help="seconds to keep retrying the flash (default 120)")
    ap.add_argument("--core-dir", default=DEF_CORE, help="PLATFORMIO_CORE_DIR")
    ap.add_argument("-v", "--verbose", action="store_true", help="print full log on failed attempts")
    args = ap.parse_args()

    # Resolve project dir: a real path, else a sibling folder of this script.
    here = os.path.dirname(os.path.abspath(__file__))
    project = args.app if os.path.isdir(args.app) else os.path.join(here, args.app)
    if not os.path.isfile(os.path.join(project, "platformio.ini")):
        log(f"no platformio.ini in {project!r} — is '{args.app}' the right app?")
        return 2

    port = find_port(args.find)
    if not port:
        log(f"no watch found after {args.find}s. Plug it in (hold the button) and retry.")
        return 2
    log(f"found {port}; flashing {os.path.basename(project)} (env {args.env}). "
        f"Hold the watch button if it doesn't take.")

    deadline = time.time() + args.retry
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        # Re-scan each attempt: a failed sync often re-enumerates the port.
        port = find_port(min(15, args.find)) or port
        ok, tail = attempt_flash(project, args.env, port, args.core_dir, args.verbose)
        if ok:
            log(f"SUCCESS on attempt {attempt}. Press the watch button to boot it.")
            return 0
        remaining = int(deadline - time.time())
        log(f"attempt {attempt} failed ({tail or 'unknown'}); "
            f"{remaining}s left — hold the button…")
        time.sleep(3)
    log(f"gave up after {args.retry}s. Last: {tail or 'unknown'}")
    return 3


if __name__ == "__main__":
    sys.exit(main())
