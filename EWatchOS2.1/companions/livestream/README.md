# livestream companion

The WiFi twin of Screenshare. Build the watch with `EW_APPS=livestream`, turn
WiFi on (Client mode, same network as the Mac), open **Stream** on the watch,
then:

```sh
python3 stream_to_watch.py                     # = ../screenshare/screenshare.py --wifi ewatch.local
python3 stream_to_watch.py --host 192.168.1.42 --window Safari
```

Same packets, pacing and stats as the USB app (`src/core/ewstream.h`); see
`../screenshare/README.md` for flags. Install deps from `../screenshare/requirements.txt`.
