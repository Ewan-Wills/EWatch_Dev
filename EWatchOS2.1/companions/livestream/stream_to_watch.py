#!/usr/bin/env python3
"""WiFi screen mirror for the Stream app (EW_APPS=livestream).

The Stream app now speaks the same packets as the USB Screenshare app (see
src/core/ewstream.h), so this is a thin wrapper around
../screenshare/screenshare.py --wifi HOST. All its flags work here.

    python3 stream_to_watch.py [--host ewatch.local] [screenshare.py flags...]
"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "screenshare"))
args = sys.argv[1:]
host = "ewatch.local"
if "--host" in args:
    i = args.index("--host")
    host = args[i + 1]
    del args[i:i + 2]
sys.argv = [sys.argv[0], "--wifi", host] + args
import screenshare  # noqa: E402
screenshare.main()
