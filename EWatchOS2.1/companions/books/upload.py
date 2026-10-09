#!/usr/bin/env python3
"""
bookreader uploader — send a plain-text (.txt) book to an EWatch over USB.

The watch stores it in LittleFS under /books/ and it then appears in the
on-watch Library. Also supports listing and deleting stored books.

Wire protocol (PC -> watch), all integers little-endian:
    magic "EWBK" | cmd
      cmd 'U' upload : u16 nameLen | u32 dataLen | name | data | u32 crc32(data)
      cmd 'L' list   : (no args)
      cmd 'D' delete : u16 nameLen | name
The watch answers with newline ASCII lines prefixed ">>BOOK ".

Usage:
    python3 upload.py mybook.txt                 # auto-detect the port
    python3 upload.py --port /dev/cu.usbmodem101 mybook.txt
    python3 upload.py --list
    python3 upload.py --delete mybook.txt

IMPORTANT: open the "Books" app on the watch first — opening it starts the
serial listener. Close any serial monitor (e.g. pio device monitor) so this
script can own the port.
"""
import argparse
import glob
import os
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed — run:  pip install -r requirements.txt")

MAGIC = b"EWBK"
BAUD = 115200  # native USB-CDC ignores this, but pyserial wants a value


def autodetect_port():
    pats = ["/dev/cu.usbmodem*", "/dev/tty.usbmodem*",  # macOS
            "/dev/ttyACM*", "/dev/ttyUSB*",             # Linux
            "COM*"]                                      # Windows (rough)
    for p in pats:
        hits = sorted(glob.glob(p))
        if hits:
            return hits[0]
    return None


def open_port(port):
    if not port:
        port = autodetect_port()
    if not port:
        sys.exit("No serial port found. Pass --port explicitly.")
    try:
        ser = serial.Serial(port, BAUD, timeout=0.3)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}\n"
                 "Is a serial monitor still attached? Close it and retry.")
    print(f"Using port: {port}")
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def drain(ser, seconds, want_prefix=b">>BOOK"):
    """Read lines for up to `seconds`, echo >>BOOK replies, return them."""
    out = []
    t0 = time.time()
    buf = b""
    while time.time() - t0 < seconds:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line.startswith(want_prefix):
                    text = line.decode("ascii", "replace")
                    print("  " + text)
                    out.append(text)
        else:
            time.sleep(0.02)
    return out


BLOCK = 2048  # must match kBlock in bookreader.cpp (and fit the watch's RX queue)


def wait_reply(ser, prefixes, timeout, buf):
    """Read until a >>BOOK line starting with one of `prefixes` (or ERR)
    arrives. Returns (line or None, leftover bytes)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        chunk = ser.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.strip().decode("ascii", "replace")
            if text.startswith(">>BOOK ERR"):
                return text, buf
            if any(text.startswith(p) for p in prefixes):
                return text, buf
    return None, buf


def cmd_upload(ser, path):
    if not os.path.isfile(path):
        sys.exit(f"No such file: {path}")
    with open(path, "rb") as f:
        data = f.read()
    name = os.path.basename(path).encode("utf-8")[:200]
    crc = zlib.crc32(data) & 0xFFFFFFFF
    hdr = MAGIC + b"U" + len(name).to_bytes(2, "little") + len(data).to_bytes(4, "little") + name

    print(f"Uploading {path}  ({len(data)} bytes, crc32=0x{crc:08x}) ...")
    # The watch's USB serial drops bytes when its RX queue is full, so send
    # one block at a time and wait for its ACK (see doUpload in bookreader.cpp).
    ser.write(hdr)
    ser.flush()
    buf = b""
    line, buf = wait_reply(ser, (">>BOOK RECV",), 10.0, buf)
    if not line or line.startswith(">>BOOK ERR"):
        print(line or "No reply. Make sure the Books app is open on the Library "
              "screen and no other program holds the port.")
        sys.exit(1)
    print("  " + line)

    t0 = time.time()
    sent = 0
    while sent < len(data):
        block = data[sent:sent + BLOCK]
        ser.write(block)
        ser.flush()
        sent += len(block)
        line, buf = wait_reply(ser, (">>BOOK ACK",), 10.0, buf)
        if not line or line.startswith(">>BOOK ERR"):
            print(f"\n  {line or 'no ACK'} at {sent} bytes")
            sys.exit(1)
        pct = sent * 100 // len(data)
        print(f"\r  {sent}/{len(data)} bytes ({pct}%)  {sent / 1024 / max(time.time() - t0, 1e-3):.0f} KB/s",
              end="", flush=True)
    print()

    ser.write(crc.to_bytes(4, "little"))
    ser.flush()
    line, buf = wait_reply(ser, (">>BOOK OK",), 10.0, buf)
    if not line:
        # Older firmware doesn't flush the final line; the watch only keeps a
        # file whose CRC matched, so a listing with the right size is proof.
        ser.write(MAGIC + b"L")
        ser.flush()
        want = f">>BOOK FILE {name.decode('utf-8', 'replace')} {len(data)}"
        line, buf = wait_reply(ser, (want,), 5.0, buf)
        if line and line.startswith(want):
            line = f">>BOOK OK {name.decode('utf-8', 'replace')} {len(data)} (confirmed via list)"
    print("  " + (line or "no final reply"))
    if line and line.startswith(">>BOOK OK"):
        print("Done — the book should now be in the watch Library.")
    else:
        sys.exit(1)


def cmd_list(ser):
    ser.write(MAGIC + b"L")
    ser.flush()
    print("Books on watch:")
    drain(ser, 2.5)


def cmd_delete(ser, name):
    nb = os.path.basename(name).encode("utf-8")[:200]
    frame = MAGIC + b"D" + len(nb).to_bytes(2, "little") + nb
    ser.write(frame)
    ser.flush()
    drain(ser, 2.5)


def main():
    ap = argparse.ArgumentParser(description="Upload a .txt book to EWatch bookreader.")
    ap.add_argument("file", nargs="?", help="the .txt file to upload")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--list", action="store_true", help="list books stored on the watch")
    ap.add_argument("--delete", metavar="NAME", help="delete a book by name")
    args = ap.parse_args()

    ser = open_port(args.port)
    try:
        if args.list:
            cmd_list(ser)
        elif args.delete:
            cmd_delete(ser, args.delete)
        elif args.file:
            cmd_upload(ser, args.file)
        else:
            ap.error("give a file to upload, or use --list / --delete")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
