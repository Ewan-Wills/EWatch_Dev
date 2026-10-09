# motion module notes

Carried over from the stand-alone `llm_motion/` folder (before 2026-10-09).
OS-wide parts may be out of date. Training tools are in companions/motion/ (the Python venv
they used is not in git: recreate it from companions/motion/motionlm).

---

# llm_motion: an on-device "motion language model" for the EWatch

EWatchOS2.1 with a **Motion LM** app added (launcher > TOYS > "Motion LM").
It reuses the llama2.c transformer runtime from `../llm_ondevice` (MIT), but the
model is our own: a 24K-parameter transformer trained here on a "motion language".
Wrist movements and touch swipes become tokens, and the model predicts a gesture
label token at the end of each phrase. Recognised gestures can drive OS actions.

Built 2026-10-07 by Claude (Opus 5.5) for Ewan. **Verified in the offline
simulator (ewsim_host) only. Nothing has run on the watch yet**, and real-wrist
accuracy is unknown until real recordings exist (see "Unverified").

Base: rsync copy of `EWatchOS2.1` (2026-10-07, without .pio/.ewt/test/golden).
The copied EWatchOS2.1 notes are in `NOTES_base_EWatchOS2.1.md`.

## How it works

```
I/O task 50 Hz (controller.cpp) --motionFeedSample--> queue --> "motion" task (core 0, prio 2)
MotionView swipes/taps --motionFeedTouch--------------^          |
                                                                 v
   tokenizer (3 samples = 60 ms -> 1 token) -> segmenter -> LM, decoded one token at a time (KV cache)
                                                                 |  at segment end: feed SEP, read label
render task <-- motionServiceRender() <-- pending action <-------+  EWLOGI("MOTION","gesture=... p=... ms=...")
```

- **Tokens** (`src/apps/motion/motion_tok.{h,c}`, integer maths, mirrored bit-for-bit by
  `tools/motionlm/tokenizer.py`). Per 60 ms window of raw MMA8451 counts (4096 = 1 g):
  - `STILL_<orientation>` (6): window mean barely moved (< 0.10 g) and jerk < 0.35 g
  - `MOVE_<dir>_<small|big>_<orientation>` (72): dominant axis and sign of the window-mean change, big if >= 0.45 g
  - `JOLT_<axis>_<level>` (6): jerk >= 1.2 g that a mean change doesn't explain (knocks, very fast shakes)
  - touch: `TAP DTAP LONG SW_U SW_D SW_L SW_R` (CST816S gesture codes)
  - `BOS`, `SEP`, then 15 label tokens. Vocab 108.
- **Segmenter**: idle until a non-still token arrives (2 still tokens of pre-roll are kept).
  The segment closes after 5 still tokens (300 ms) or 40 tokens. Touch phrases close 700 ms
  after the last touch token, or at 6 tokens.
- **LM**: `[BOS] tokens [SEP] -> label`. Trained as a plain next-token LM over the whole
  sequence, with the label position weighted 4x. On the watch the IMU segment is decoded
  incrementally as tokens arrive, so at the end only the SEP step runs. The label = argmax
  over the 15 label logits, and p = their softmax. Below `thr` (0.60) the result is `none`.
  The live page also shows the LM's guess for the next motion token.
- Two independent KV contexts (IMU and touch), activations and KV caches in PSRAM.

## Model (tools/out/report.json)

| | |
|---|---|
| architecture | llama2.c Llama-2: dim 32, 2 layers, 4 heads (no GQA), SwiGLU hidden 64, RoPE, RMSNorm, shared classifier |
| params | **24,096** fp32 (96 KB); file `tools/out/motion_lm.bin` 97,948 B (llama2.c legacy format) |
| in firmware | `src/apps/motion/motion_model_data.c`, a const array in flash (DROM `0x3c1b94bc`, read in place, no RAM copy) |
| RAM | 52,912 B activations + 2 KV contexts (PSRAM first, else heap) + 6 KB task stack + 1.5 KB raw ring |
| flash | firmware.bin 2,416,608 B = 76.8 % of the 3 MB app slot (EWatchOS2.1 is 2,283,408 B), about +133 KB |
| training | 58,800 synthetic sequences, 4,000 steps AdamW, batch 128, about 3 min on the M4 Pro CPU (torch 2.14) |

A 48-dim model (51.5K params) scored the same (98.15 %), so the smaller one ships.

### Accuracy: synthetic data only

Held-out test set (different RNG seed from training), 14,700 sequences:
- **98.1 %** overall (thr 0.6), gestures 97.9 %, `none` false-trigger rate 1.3 %, gestures rejected as none 1.3 %
- per class: none 98.7, flick_out 96.7, flick_in 96.7, twist_l 99.4, twist_r 99.8, shake 95.5,
  double_shake 97.8, raise 99.9, lower 98.8, knock 94.7, circle 98.4, all four touch phrases 100 %

Trace-level, through the **firmware C code** (`crosscheck.py`: fresh seed 4242, 300 traces
per class and 900 none, scored the way the watch runs it: segmentation + LM + threshold):
**97.3 %** of gesture traces give exactly the right gesture. 98.9 % of `none` traces trigger
nothing. Weakest: knock 92.3 % (misses are mostly `none` or no segment at all; the 20 % sample-and-hold augmentation can hide a 1-sample spike), flick_in 94.7 %.

**Real-wrist accuracy: UNKNOWN.** The synthetic generators (`tools/motionlm/synth.py`) are my
physical guesses. They assume a frame where X runs along the forearm, Y across the wrist, Z out
of the face, and the arm hangs with gravity on +-X. Neither the frame nor the gesture shapes have
been checked against a real arm. Expect a large drop on a real wrist until capture + fine-tune
(below) has been done. Left/right naming (twist_l vs twist_r, flick out vs in) may come out
mirrored relative to how it feels on the wrist (the IMU Gestures view negates ax for its ball).

### C vs Python cross-check (tools/motionlm/crosscheck.py)
- 700 random traces (all classes plus 40 adversarial random-noise/clipping traces):
  **0 token mismatches, 0 segment mismatches, 0 label mismatches**, max |p_C - p_torch| = 1.4e-6.
- Checker self-test: `--break-checker` moves one Python threshold by 50 counts, and the run then
  reports 47 mismatching traces and `CROSSCHECK FAIL`. So the comparison can fail.

## Latency

| where | measured | what it means |
|---|---|---|
| Mac M4 Pro, firmware C code, clang -O2 | **5-6 us per forward** | host_eval benchmark, 200k forwards |
| ewsim_host | ~0.00 ms | **meaningless**: the simulator charges no CPU time. The UI tags it "sim" |
| ESP32-S3 watch | **NOT MEASURED** | run `motion bench` (below) |

Hardware estimate (UNVERIFIED): about 24K MACs per token. Compute-bound that is ~0.5 ms at
240 MHz. If it's bound by flash streaming like stories260K in llm_ondevice (the ~15 tok/s figure
for 264K params gives ~0.25 us per weight), it's ~6 ms. So expect 0.5-6 ms per token at 240 MHz
(the Motion app runs at 240 MHz because it animates). That is <= 10 % of core 0 at the 16.7
tokens/s a moving wrist produces, and nothing runs while the wrist is still. In background
mode on a static screen the CPU may sit at 80 MHz, so up to ~3x slower.
User-visible latency is dominated by the segmenter, not the model: a gesture is reported
300 ms after the wrist goes still, and a swipe phrase 700 ms after the last swipe.

## Gestures and actions (dry run is ON by default: actions are only logged)

| label | what (synthetic definition) | action when LIVE |
|---|---|---|
| flick_out / flick_in | fast 0.12-0.26 s acceleration pulse along +Y / -Y (0.7-2 g) | Spotify next / previous track |
| twist_l / twist_r | roll about the forearm 60-120 deg and back, 0.3-0.9 s | Torch on/off (returns to the previous screen) |
| shake | 3-8 Hz (15 %: 10-24 Hz) oscillation, 0.8-1.9 g, 0.4-1.2 s | Back (posts the view's own ButtonShort; ignored on the face) |
| double_shake | two shake bursts with a 0.12-0.22 s pause | Home (watch face) |
| raise | arm hanging (gravity on +-X) -> face up, 0.4-0.9 s | Wake + watch face |
| lower | face up -> hanging | Screen off (powerRequestSleep) |
| knock | 1-2 sample spike of 1.2-3 g plus ringing | Spotify play/pause |
| circle | 1-1.75 turns of 1.2-2.5 Hz circular acceleration | Quick settings |
| swipe_uu | swipe up, swipe up | App launcher |
| swipe_lrl | swipe left, right, left | Silent mode on/off (same as the Quick Settings tile) |
| swipe_dd | swipe down, swipe down | Home (watch face) |
| tap_tap_r | tap, tap (or double tap), swipe right | Stopwatch |
| none | walking bounce + arm swing, slow drifts, small bumps, fidgets, weak flicks, random walks; any other touch phrase | - |

Spotify actions only do something when Spotify is signed in (WiFi). In the sim, and without
Spotify, they are no-ops (`done=1` still logged when the command was posted).

UI: tap the title to switch to the action map. Tap the DRY/LIVE chip to arm actions. **BG**
keeps the engine running on every screen ("global" mode; IMU gestures only, touch phrases only
inside the app). **REC** cycles a capture label. On the Motion screen, swipe-right is a token,
not "back" (controller.cpp exception). Leave with the side button or the back chevron.
The screen stays awake while the app is open (power_mgr.cpp).

## Console (ewlog, works with `ewt raw "..."`)
```
motion                         status (ready, running, dry, global, cap, thr, forwards, segments, avg ms)
motion classes                 label ids and actions
motion inject <class> [0-2]    replay a canned synthetic trace (held-out seed 777, unfiltered) through
                               the SAME sample->tokenizer->segmenter->LM path, in place of the live accel
motion dry 0|1                 1 = only log actions (default)
motion global 0|1              keep the engine running on every screen
motion cap <label>|off         capture mode: every segment is printed as MCAP/MCAPRAW lines with this label
motion thr <1-99>              confidence threshold in % (default 60)
motion bench [n]               n forward passes on the engine task -> MOTION: bench us_per_fwd=...
motion verbose 0|1             print every token (MTOK ...)
```
Log lines tests can assert: `MOTION: gesture=<label> p=<0..1> ms=<SEP+softmax ms> seg_ms=... src=imu|touch n=<tokens> best=<pre-threshold label>`
and `MOTION: action="<text>" label=<label> dry=0|1 [done=0|1]`.

## Simulator verification (ewsim_host, 2026-10-07, first-person)

`cd ~/Documents/EWatch/EWatch_Firmware/ewsim_host && ./ewsimrec llm_motion --snap /tmp/x ../llm_motion/test/<t>.ewt`

| script | path exercised | result |
|---|---|---|
| test/motion_inject.ewt | `motion inject` of all 10 IMU classes + a none trace | PASS: each recognised as its class, dry action logged, none -> no gesture |
| test/motion_sensor.ewt | **real accel path**: ewsim `shake` and `tilt` staircases inside the I/O loop | PASS: shake, twist_l, twist_r, raise, lower recognised. The abrupt flip, return-to-flat and fidget segments all came out `none` |
| test/motion_touch.ewt | real touch path (CST816S codes -> Gesture events) | PASS: swipe_uu, swipe_lrl, swipe_dd, tap_tap_r, a lone swipe -> none, swipe-right didn't leave the app |
| test/motion_actions.ewt | LIVE actions | PASS: U-U -> AppList, twist -> Torch, BG mode + twist -> back to Motion, shake -> AppList, tap-tap-R -> Stopwatch |
| test/motion_bench.ewt | console bench | PASS (number meaningless in the sim) |
| test/motion_mustfail.ewt | wrong class expected | **FAILS as it must** (exit 1) |
| test/motion_mustfail_nolog.ewt | negative check after a real shake | **FAILS as it must** (exit 1) |

Footage: `docs/motion_demo.mp4` (960x1120, 21.9 s, 30 fps) + `docs/motion_demo.sheet.png`,
rendered by `./ewsimrec llm_motion docs/motion_demo.mp4 test/motion_demo.ewt`.
It shows a real-path shake and twist, injected flick and circle, the U-U and L-R-L phrases, and the action map.

Simulator caveats: `tilt` holds each value for ~60-110 ms, so tilt gestures are staircases.
The training data includes 20 % sample-and-hold "staircase" augmentation, which covers this (and
a low effective sample rate). ewsim's `shake` is a ±1.7 g 25 Hz square wave, which no wrist
makes, and the 10-24 Hz part of the shake class exists partly so that it counts. Fast gestures
(flick, knock, circle) can't be expressed with tilt steps. That's why `motion inject` exists:
it replaces the live sample stream with a canned trace and resets the tokenizer at both ends.
In the sim, two identical gestures less than 400 ms apart get merged by the controller's
gesture dedup (OS behaviour). The test scripts wait 250 ms between same-direction swipes and
use `dtap` for tap-tap.

## For the hardware session (I did not touch the watch)
```
EWT="python3 $HOME/Documents/Claude Knowledge Base/firmware/knowledge/ewt.py"
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
$EWT flash llm_motion -e ewatch_v20          # or -e ewatch for V2.1 boards (both build, 76.8 %)
$EWT -P llm_motion run test/motion_bench.ewt     # FIRST: real ms per forward -> update this file
$EWT -P llm_motion run test/motion_inject.ewt    # full on-device path with canned traces
$EWT -P llm_motion run test/motion_touch.ewt
$EWT -P llm_motion run test/motion_sensor.ewt    # tilt/shake override path (timing differs over USB)
$EWT -P llm_motion run test/motion_actions.ewt   # LIVE actions (restores dry=1)
$EWT -P llm_motion run test/motion_mustfail.ewt  # must FAIL
$EWT -P llm_motion record docs/motion_watch.mp4 "goto Motion; wait 1000; raw motion inject shake; wait 2500"
```
Then the real test: wear it, open Motion LM, do each gesture, and read the label and p on the screen
(`$EWT tail 60 -g MOTION`). Check `motion` status for `avg_fwd_ms`.
Motion = screen id 34 (it sits before the Island `#if` gate, so ewt's gate-blind name list agrees).

## Retraining with real data
```
cd llm_motion/tools                          # venv: tools/.venv (python3.12 + torch 2.14 + numpy)
# 1. record: on the watch `motion cap flick_out` (or the REC chip), do the gesture ~20x with
#    pauses, repeat per class, include `motion cap none` with ordinary wrist use; log serial:
$EWT -P llm_motion tail 600 -g MCAP > data/caps_ewan.log
# 2. to training data (re-tokenizes MCAPRAW and checks it against the watch's tokens):
.venv/bin/python -m motionlm.capture_to_dataset data/caps_ewan.log -o data/real.jsonl --show
# 3. fine-tune (80/20 split of the real data; held-out real accuracy goes in out/report.json):
.venv/bin/python -m motionlm.train --init out/motion_lm.pt --steps 1500 --lr 1e-3 --real data/real.jsonl
# 4. check C == Python, then rebuild + flash:
.venv/bin/python -m motionlm.crosscheck
```
`train.py` without `--init` retrains from scratch (synthetic data cached in tools/data/; `--regen`
rebuilds it). It rewrites `src/apps/motion/motion_model_data.c` unless given `--no-firmware`.
`python -m motionlm.export traces` regenerates the inject traces. Capture was checked in the
sim (2 injected flicks + 1 injected and 1 tilt-path twist: 4/4 raw re-tokenizations matched the
watch's tokens), and a 20-step fine-tune ran end to end (`--outdir out/smoke_finetune`).
A capture label applies to every segment while it's set, so keep still between repetitions.

## Files
- `src/apps/motion/motion_tok.{h,c}`: tokenizer + segmenter (pure C)
- `src/apps/motion/motion_lm.{h,c}`: transformer runtime (from llm_ondevice/llm.c, llama2.c MIT)
- `src/apps/motion/motion_model_data.c`: GENERATED weights; `motion_traces.{h,c}`: GENERATED inject traces
- `src/apps/motion/motion_engine.{h,cpp}`: task, actions, console, capture
- `src/apps/motion/motion_view.{h,cpp}`: the app
- OS edits (all marked "Motion"/"llm_motion"): `core/model.h` (Screen::Motion), `apps/system/registry.cpp`
  (include, launcher row, viewFor), `core/power_mgr.cpp` (keep awake), `core/controller.cpp`
  (sample feed, render hook, swipe-right is a Gesture on Motion), `core/ewlog.cpp` (console hook, help)
- `tools/motionlm/`: vocab, tokenizer, synth, dataset, model (+ llama2.c exporter), train, export,
  crosscheck, capture_to_dataset. `tools/host/host_eval.c` runs the firmware C code on the Mac.
- `test/motion_*.ewt`, `docs/motion_demo.mp4`

## Unverified / open
- Everything on real hardware: speed, RAM, that the task coexists with WiFi/Spotify, and touch
  swipes from the real CST816S (swipe codes vs physical direction).
- Real-wrist accuracy and the assumed axis frame. This needs the capture + fine-tune loop.
- Raise-to-wake from screen-off doesn't work: the I/O task is suspended in light sleep, so the
  engine sees no samples. "raise" only acts while the screen is on (or dimmed).
- Running (jogging) can look like a shake. The none class only covers walking up to 0.4 g.
- Not committed to the EWatch_Firmware repo (on purpose; that repo holds other sessions' work).
