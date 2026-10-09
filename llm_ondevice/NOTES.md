# llm_ondevice — flashing & what to expect

A **standalone** feasibility sketch: a tiny LLM (llama2.c `stories260K`) running
entirely on the EWatch (ESP32-S3FH4R2, 4 MB flash / 2 MB PSRAM). No PC, no WiFi.
It is NOT an OS app — it is a single-purpose firmware that, on boot, generates a
short story and streams it to Serial and the display, then idles. Flashing this
**replaces** the watch OS until you reflash EWatchOS2.1.

See `FEASIBILITY.md` for the numbers and the honest caveats.

## Build (verified — ends in [SUCCESS])

```bash
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/llm_ondevice
~/.platformio/penv/bin/pio run
```

## Flash it (YOUR step — I did not flash)

Put the watch in download mode / plug it in over USB, then:

```bash
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
cd /Users/ewills/Documents/EWatch/EWatch_Firmware/ShortsApps/llm_ondevice
~/.platformio/penv/bin/pio run -t upload          # add --upload-port /dev/cu.usbmodemXXXX if needed
~/.platformio/penv/bin/pio device monitor -b 115200   # watch the story stream in
```

To return the watch to normal, reflash the OS from
`EWatch_Firmware/EWatchOS2.1` (`pio run -e ewatch -t upload`).

## What you should see

- **Screen (ST7789 240×280):** a cyan header `EWatch LLM loading...`, briefly
  `Mapping model from flash...`, then the backlight comes on and under a header
  `EWatch LLM generating` the story streams in a word at a time starting from the
  yellow prompt `Once upon a time`. When done, the header shows the token count
  and the measured tok/s, e.g. `EWatch LLM 220 tok 16.4 tok/s`. If the text
  reaches the bottom it clears and continues from the top.
- **Serial (115200):** a banner, `heap free` / `psram free`, the model stats
  (`dim=64 layers=5 vocab=512 seq_len=512 params=264128`), the streamed story,
  and a final `-- done: N tokens, X.XX tok/s --`. **This serial line is the
  real, first-person tok/s measurement** — the figure in FEASIBILITY.md is only
  an estimate until you read this.

## Honest caveats

- **The story will be simple and often garbled.** `stories260K` is a ~264K-param
  toy. It cannot answer questions or follow instructions. That is the expected,
  honest result of the biggest model that fits in 4 MB / 2 MB.
- **tok/s in FEASIBILITY.md is an ESTIMATE** (~12–20 tok/s, range 8–30). The
  serial output above is what actually measures it. If it's wildly off, the flash
  read-bandwidth assumption is the thing to revisit.
- **`src/model_blob.S` uses ABSOLUTE `.incbin` paths** to
  `src/assets/stories260K.bin` and `src/assets/tok512.bin`. If you move this
  folder, edit those two paths or the build will fail to find the model.
- **Model provenance:** downloaded from HuggingFace `karpathy/tinyllamas`
  (`stories260K/stories260K.bin`, `tok512.bin`). MIT-licensed llama2.c runtime.
  Pretrained — nothing was trained here.
- Backlight is driven as a plain GPIO HIGH (no PWM dimming) — this sketch doesn't
  pull in the OS power manager. Fine for a demo.
- If `psram free` reads ~0 or `llm_setup FAILED` shows on screen, PSRAM didn't
  come up — check `board_build.psram_type = qspi` and `-DBOARD_HAS_PSRAM` survived.

## Files

- `platformio.ini` — standalone env, stock `huge_app.csv`, GFX reused offline
  from the bookreader libdeps.
- `src/main.cpp` — display bring-up (mirrors EWatchOS2.1 `drivers/display.cpp`) +
  generate-on-boot + Serial/screen streaming.
- `src/llm.c` / `src/llm.h` — ported llama2.c inference (flash-mmap weights,
  PSRAM state, callback output). No file I/O, no CLI.
- `src/model_blob.S` — embeds the model + tokenizer into flash `.rodata`.
- `src/assets/stories260K.bin`, `src/assets/tok512.bin` — the model + tokenizer.
