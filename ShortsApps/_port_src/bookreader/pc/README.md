# bookreader PC uploader

Send a plain-text `.txt` book to the EWatch over USB. The watch saves it to
LittleFS and it appears in the on-watch **Books** library.

## Setup (once)

```sh
cd ShortsApps/bookreader/pc
python3 -m venv .venv && source .venv/bin/activate   # optional but tidy
pip install -r requirements.txt
```

## Upload a book

1. **On the watch:** swipe up → open **Books**. Opening the app starts the
   serial listener (you'll see `>>BOOK READY` on the port). Keep it on the
   Library screen.
2. **Close any serial monitor** (the PlatformIO monitor, `screen`, etc.) — only
   one program can own the USB port at a time.
3. Run:

```sh
python3 upload.py mybook.txt
# or name the port explicitly:
python3 upload.py --port /dev/cu.usbmodem101 mybook.txt
```

On success you'll see `>>BOOK OK mybook.txt <bytes>` and the book shows up in
the Library (tap it to read).

## List / delete

```sh
python3 upload.py --list
python3 upload.py --delete mybook.txt
```

## Notes

- Port auto-detection picks the first `cu.usbmodem*` (macOS) / `ttyACM*`
  (Linux). Use `--port` if you have several devices.
- Native USB-CDC ignores the baud rate, so transfer runs at full USB speed.
- Filenames are sanitised on the watch (safe characters only, forced `.txt`
  suffix, basename only). A book over ~2 MB is rejected (flash is ~896 KB
  anyway).
- The upload is CRC32-checked end to end; a corrupted transfer is rejected and
  the partial file is discarded.
