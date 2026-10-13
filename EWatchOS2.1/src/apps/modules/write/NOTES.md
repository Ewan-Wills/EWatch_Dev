# write module notes

**Write**: type by drawing letters and digits with a finger. Rebuilt 2026-10-13
(Claude Opus 5.5, ewills172) after Ewan found the first version (2026-10-07, $P
template matching against 80 hand-made shapes) recognised none of his letters.

## Using it
Launcher: **Write** (yellow pencil).

```
+--------------------------------+
| hello wor|                     |  text: the tail that fits; the word being
+--------------------------------+  written is blue (it may still be corrected)
|                                |
|     write anywhere below,      |
|     one letter at a time       |
|                                |
| (^)                       (<x) |  shift, delete
+--------------------------------+
```
- **Letters/digits:** draw one at a time, upper or lower case, any stroke order.
  Write letters on top of each other: the whole area is the canvas. The glyph is
  classified as soon as the finger lifts; it is typed **200 ms** later if it is a
  confident shape that is never the start of another letter (A B D E G H K M N P Q W X
  Y 3 4 5 6 8), else after **350 ms** without a new stroke (NVS `ewwrite/gapMs`,
  200-3000; default 350). So a lone stem waits: it may become i, t, l, f, j, k...
- **Full stop / comma:** a small dot (or a tap) = `.`, a small single stroke = `,`
  (size relative to the writer's recent letters; a bigger downward stroke right after
  a complete word counts as a comma when the letter would make a non-word). Both end
  the word (it is corrected) and add a space; a space gesture after them adds no
  second space. Inside a number, `.` is a decimal point (3.5).
- **Space:** one straight stroke left to right. **Delete:** right to left, or the
  delete button (tap = one character; hold = repeat, then whole words). Tapping delete
  while ink is still on screen cancels that ink. One delete right after an
  auto-correction puts back what was written.
- **Shift:** tap = next letter upper case; double-tap = caps lock; tap again = lower.
  A sentence starts upper case by itself (the button lights up). A touch on a button
  that moves is ink; tapping shift with ink pending types that letter first.
- **Last word:** corrected when you write a space or punctuation, or after 2.5 s
  without writing. A corrected word flashes green for 0.8 s.
- **Feel:** the last letter's ink fades out (3 steps of 45 ms); haptics differ for a
  letter (light tick), space, punctuation, correction (stronger) and delete.
- Nothing is learned per user (public app): the only adaptation is the running
  letter size used to judge punctuation.
- **Side button** = back. Swipe-right is *not* "back" in Write (controller.cpp).
- No suggestion chips: the recogniser plus English context decide (Ewan's call,
  2026-10-13: "be confident about the training").

## How it works
1. **Ink** (`write_view.cpp`): touch samples are stamped with a round brush onto the
   frame canvas and only the changed rectangle is pushed to the panel. (Redrawing
   and flushing the whole screen per touch sample took ~280 ms per sample and
   starved other tasks.)
2. **Recognition** (`write_cnn.{h,cpp}`, weights `write_cnn_weights.h`): the strokes
   are drawn into a 28x28 image (box-fit: longest side 20 px, centred; brush radius
   1.1 px, anti-aliased by distance) and classified by a CNN:
   conv3x3 1->16, pool, 16->32, pool, 32->64, pool, dense 576->128->36.
   102k parameters, **int8 weights (104 KB) read straight from flash**, float
   activations (~40 KB internal RAM). Classes: A-Z (upper and lower case merged) +
   0-9. 70 ms per glyph on the watch at 240 MHz (c1 7, c2 27, c3 29, fc 5 ms).
3. **English context** (`write_lm.{h,cpp}`, tables `write_lm_data.h`, 175 KB):
   - while writing: CNN log-prob + 0.5 x letter-trigram log-prob (given the word so
     far; floored at log 0.002 so a rare pair never beats a confident CNN). Digits:
     prior log(0.25) at a word start, log(0.01) inside a word; inside a number,
     letters get log(0.01).
   - when the word ends (space): every dictionary word of the same length (top 20k
     words of Norvig's count_1w) is scored as sum of the stored per-letter CNN
     log-probs + 0.25 x log-frequency; it replaces the typed word only if it beats it
     by 2 nats (no margin when the typed word has a stray digit, "1APPY" -> HAPPY).
     Each letter keeps its case. Logged as `correct from= to=`. Also run after 2.5 s
     without writing, so the last word of a message gets corrected.
4. **Edit gestures** (`write_recog.cpp`, `editGesture`): a single stroke whose chord
   is >= 90 % of its length, bends < 14 %, >= 50 px, within 25 degrees of horizontal.
   The rest of write_recog ($P matcher + `write_templates.h`) is no longer used.
5. CPU: Write keeps 240 MHz while open (`desiredFrameMs` never 0). Each 80<->240 MHz
   switch made the USB console go quiet for a while, and recognition must be fast.

## Where the model comes from
Pipeline: `companions/write/ml/` (data lives outside the repo, see its README).
- **EMNIST byclass** (NIST, 814k handwritten characters): re-fitted to the same
  box as the watch, capped at 6000 per merged class (213k images).
- **UJIpenchars2** (UCI #177): real pen strokes, 60 writers, upper + lower + digits.
  Rendered with finger-style variation: rotation +-12 deg, shear +-0.3, aspect +-20 %,
  per-stroke offset, sparse sampling, jitter, brush radius 0.75-1.7 (40 per sample).
- Trained 12-14 epochs (AdamW, one-cycle, label smoothing, small random affine).
  BN folded, per-channel int8. `train.py` checks the int8 path in numpy
  (`ref_infer`, same loops as the C code).
- Tried: 157 macOS fonts, skeletonised and redrawn with the watch brush (58k glyphs,
  `font_glyphs.py`): 91.3 % per letter but 92.0 % vs 94.2 % of words on 180 phrases:
  not used.
- Shipped model: trained on **all 60 UJI writers**. Accuracy figures below are from
  the same recipe trained on the 40 "trn" writers and tested on the 20 "tst" writers
  it never saw.
- Tried and not better: wider net (24/48/96 channels, 170 KB): 90.9 %; 62 classes
  (a-z separate, merged at the end): 90.8 %. The remaining errors are mostly shapes
  that are the same drawing (0/O, 1/I/l, 5/S, q/9); context resolves those.

## Verification (2026-10-13)
"Unseen writers" = the 20 UJIpenchars2 test writers; the model measured is the same
recipe trained without them. Host numbers use the firmware's C code.
| check | result |
|---|---|
| per letter, 2480 glyphs, host, placed + resampled as on the watch | top-1 90.8 %, top-3 97.7 % |
| C rasteriser vs Python | max pixel diff 4e-6; 400/400 identical predictions |
| C language model vs Python | 3000/3000 identical (incl. 27 words with digits) |
| **180 phrases**, letters of unseen writers, host replica of the watch pipeline | **94.2 % of words**, 153/180 phrases exact (CNN alone 80.2 %) |
| word simulation: 2400 common words / 300 unusual / 300 numbers | 94.0 % / 69.3 % / 85.7 % (CNN alone 76.9 / 73.3 / 63.7) |
| **on the watch**, 18 phrases by unseen writers (`drive_words`) | **14/18 exact, 88 % of words, 3.6 % char error**; host replica of the same strokes: 14/18, 90 % |
| on the watch, alphabet sweep (A-Z x 2 upper + 2 lower writers, 0-9 x 4), one letter at a time, no word context | **123/144 = 85.4 %** (88.9 % counting 0/O, 1/I as one; top-3 95.8 %); every letter and digit right for at least 2 of its 4 writers; 0 harness failures |
| speed on the watch | 69-70 ms per letter at 240 MHz; word correction 0.8-1.3 ms |
| `test/write/write.ewt` / `write_mustfail.ewt` | PASS / FAIL at step 25 (as it must), watch and ewsim_host |

### 2026-10-13, second pass (ewsim_host + host; the watch was off: NOT yet run on the watch)
| check | result |
|---|---|
| early commit rule (`prefix.py`): classify the first strokes of every multi-stroke UJI letter | held-out writers: 37 % of letters typed at 200 ms, 0.3 % of unfinished letters cut |
| same, ewsim_host, 18 phrases | 83/190 letters (44 %) typed early; O T I L C S R U always wait |
| `.` and `,` rule on all 120+120 UJI punctuation samples, each writer's scale | `.` 118/120, `,` 76/120 (size+shape), letters taken for punctuation 11/7440 |
| 24 sentences with `.` and `,` by unseen writers, ewsim_host (shipping model) | 18/24 exact, 44/48 punctuation marks (13/24 and 40/48 before the context comma rule) |
| 18 plain phrases, ewsim_host | 17/18 exact |
| 180 phrases, host replica, held-out model | 94.2 % of words (unchanged by the new LM rules) |
| correction then one delete (ewsim_host, "yon" -> "you" -> "yon") | undo restores the written word; next delete deletes normally |
| LM C vs Python incl. all-digit words ("701" -> "you" needs a 2-nat extra margin at 3+ digits, 7 at 2 digits so "10" stays) | 3000/3000 identical |
| smoke / must-fail (now covers early commit, '.', ',', kept space) | PASS / FAIL (ewsim_host) |

Bugs found and fixed on the way (all measured on the watch):
- The touch controller sometimes returns an all-0xFF read: a touch at (4095,4095)
  joined the glyph and squashed the ink to a dot. controller.cpp now repeats the last
  good sample instead (logs `TOUCH bad_read`, at most every 5 s).
- Swipe-right "back" fired on left-to-right strokes (suppression lost in the module move).
- Full-screen redraw per touch sample (~280 ms each); render task starving loopTask
  (task WDT reboot); list drag loop doing the same (launcher.cpp).
- The pause correction fired straight after every letter (unsigned millis wrap).
- Test harness: lost USB replies, multi-stroke letters split by serial latency (the
  `stroke` step now plays a whole glyph on the watch), log lines lost (the test reads
  the text with the `write text` console command). See the KB doc ewatch-test-harness.md.


### Speed (measured on the watch, 2026-10-13, timing log `WRITE timing`)
| step | before | now |
|---|---|---|
| finger-up -> letter typed | 410 ms (all letters) | 205 ms (early classes, 44 % of letters) / ~360 ms |
| committing a letter (logging) | 85-138 ms | 7-20 ms: ewlog no longer writes LittleFS on the caller's task (RAM buffer + `ewlogfs` task every 500 ms; RTC mirror unchanged) |
| screen update per letter | 61 ms full flush | 18 ms: only the text line + buttons are pushed |
| CNN (70 ms) | on the render task (ink froze) | `wcnn` worker on core 0; ink keeps 60 fps |

## Tests
```
EWT="python3 $HOME/Documents/Claude Knowledge Base/firmware/knowledge/ewt.py"
EW_APPS="write ..." $EWT -P . flash -e ewatch_v20
$EWT -P . run test/write/write.ewt
cd companions/write/ml   # alphabet + phrase sweeps on the watch, unseen writers
python watch_tests.py ewt data/alpha.ewt 4 && python watch_tests.py drive "$EWT|-P|<proj>" data/alpha.ewt out.txt
python watch_tests.py score out.txt data/alpha.ewt.labels
python watch_tests.py words data/words.json && python watch_tests.py drive_words "$EWT|-P|<proj>" data/words.json words.txt
python phrase_host.py data/words.json      # same strokes through the C CNN + LM on the Mac
```
Host benches (`test/write/host`): `cnn_bench SAMPLES [-p]`, `lm_bench IN`.

## Limits / next
- Real-finger accuracy on Ewan's own writing is the open question: all numbers above
  are pen data from other people, replayed as finger strokes.
- Multi-stroke letters need the next stroke within 450 ms; slow writers can raise `gapMs`.
- No punctuation, no newline gesture (the old `/` = enter was dropped: a slanted 1
  could trigger it). Lower case by default; no auto-capitalisation.
- Word correction can change a deliberate unusual word that looks like a common one;
  delete and rewrite it (no undo yet).
