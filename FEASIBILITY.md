# On-device LLM on the EWatch — feasibility (hard numbers)

**Verdict: YES — a tiny LLM fits and runs entirely on the EWatch.** It is a
*toy*: Karpathy's `stories260K` (~264K parameters), which writes garbled/simple
"Once upon a time" story fragments and nothing more. This is an honest demo of
the architecture, not a useful assistant. The compile is verified; the tok/s
figure below is a reasoned estimate (I cannot flash).

Compile status (final line of `pio run`):
```
RAM:   [=         ]   6.0% (used 19664 bytes from 327680 bytes)
Flash: [====      ]  44.6% (used 1402481 bytes from 3145728 bytes)
========================= [SUCCESS] Took 16.94 seconds =========================
```

---

## 1. The hard constraint, stated honestly

The viral Reddit build (github.com/slvDev/esp32-ai) runs a **28.9M-param** model,
**14.9 MB at 4-bit**, on an ESP32-S3 **N16R8 (16 MB flash / 8 MB PSRAM)** using
Per-Layer-Embeddings with the 25M-param embedding table memory-mapped in flash.

**The EWatch is an ESP32-S3FH4R2: 4 MB flash / 2 MB PSRAM / 512 KB SRAM.** A
~15 MB model physically cannot be stored — the whole flash is 4 MB. **That model
cannot fit, and no partition trick changes that.** So the goal here is the
*largest model that genuinely fits*, which is ~100× smaller.

## 2. Real flash budget on a 4 MB chip

Partition table = stock `huge_app.csv` (same as the shipping OS):

| Region    | Size      | Notes                          |
| --------- | --------- | ------------------------------ |
| bootloader + partition table | ~0x10000 | fixed |
| nvs       | 20 KB     | 0x9000                         |
| otadata   | 8 KB      | 0xe000                         |
| **app0**  | **3 MB**  | 0x10000, size 0x300000 — the only app slot (no OTA) |
| spiffs    | 896 KB    | 0x310000 (unused by this spike) |
| coredump  | 64 KB     | 0x3F0000                       |

The app image must hold firmware **and** the embedded model. Measured:

| Item                              | Bytes       |
| --------------------------------- | ----------- |
| App slot (huge_app `app0`)        | 3,145,728   |
| — Arduino core + GFX + our code   | ~339,714    |
| — Embedded model `stories260K.bin`| 1,056,540   |
| — Embedded tokenizer `tok512.bin` | 6,227       |
| **Total app image used**          | **1,402,481 (44.6%)** |
| **Free in app slot**              | **~1,743,247 (~1.66 MB)** |

So on 4 MB flash the *honest* model budget, with a ~1.7 MB watch-sized firmware
and no custom partition, is **up to ~2.5–2.8 MB of model** if one existed. The
only *pretrained, no-training-required* checkpoint that safely fits is
`stories260K` (1.06 MB). `stories15M` (~60 MB fp32) is ~15× too big for the
entire chip; there is no official Karpathy checkpoint between 260K and 15M.

## 3. Model chosen

| Field        | Value                                                      |
| ------------ | ---------------------------------------------------------- |
| Model        | llama2.c `stories260K` (TinyStories), **pretrained, not trained here** |
| Params       | 264,128 fp32 weights (verified from the file, see below)   |
| Architecture | dim 64, hidden_dim 172, n_layers 5, n_heads 8, n_kv_heads 4 (GQA), vocab 512, seq_len 512 |
| File size    | 1,056,540 B = 28 B config header + 264,128 × 4 B weights   |
| Weights      | **shared** (classifier = token-embedding table)           |
| Tokenizer    | `tok512.bin`, 512-entry SentencePiece BPE, 6,227 B         |

Param count cross-check (shared weights): token_emb 32,768 + rms 320+320+64 +
wq 20,480 + wk 10,240 + wv 10,240 + wo 20,480 + w1/w2/w3 55,040×3 + skipped
RoPE freq 4,096 = **264,128**. Matches the file exactly.

## 4. Runtime: ported `run.c`, NOT the PLE/flash-mmap trick

I ported Karpathy's `run.c` (plain C fp32 inference) to a PlatformIO sketch. The
PLE + 4-bit + flash-lookup-table machinery in the esp32-ai build exists **only to
make a 28.9M model fit** — it buys nothing for a 260K model that already fits
whole, and it would be far more code to port and get right. Justification:

- **Weights live in flash and are memory-mapped by the MMU**, which is exactly
  what `run.c`'s `mmap()` did on a PC. The model is embedded via `.incbin`
  (`src/model_blob.S`) into `.rodata`, which the ESP32-S3 linker places in the
  flash-backed DROM region (verified: symbol `stories_bin` at `0x3c04031c`, i.e.
  flash, not RAM). The Transformer weight pointers index straight into it — **zero
  RAM copy of the ~1 MB model**, 4-byte aligned for `float` access.
- **Only the activation / KV-cache state is allocated in PSRAM** (`heap_caps_calloc`
  with `MALLOC_CAP_SPIRAM`, fallback to internal RAM).

## 5. Memory usage (measured + computed)

**Flash:** 1,402,481 B of 3 MB app slot (§2).

**SRAM (internal DRAM):** 19,664 B static (6.0% of 320 KB) from the build report,
plus ~20 KB runtime heap for the tokenizer (512 vocab strings ≈ 6 KB, pointer +
score + sorted + probindex tables ≈ 14 KB) and the small prompt-token buffer.
Comfortably inside 512 KB SRAM. No 134 KB frame canvas is used (text is drawn
directly), so SRAM pressure is low.

**PSRAM (2 MB):** the `RunState` buffers, computed for this config (kv_dim = 32):

| Buffer                | Floats                | Bytes    |
| --------------------- | --------------------- | -------- |
| key_cache + value_cache | 2 × 5 × 512 × 32    | 655,360  |
| att (n_heads × seq_len) | 8 × 512             | 16,384   |
| x, xb, xb2, q (×dim)  | 4 × 64                | 1,024    |
| hb, hb2 (×hidden_dim) | 2 × 172               | 1,376    |
| logits (×vocab)       | 512                   | 2,048    |
| **Total**             |                       | **~676 KB** |

~676 KB of 2 MB PSRAM — **~33% used, 1.35 MB free.** The KV cache dominates and
scales with `seq_len`; capping generation to fewer steps would shrink it further,
but it already fits with margin.

## 6. Estimated tokens/sec (ESTIMATE — not flashed)

This is a reasoned estimate from the architecture and the esp32-ai benchmark; it
is **not** a measured figure. I cannot flash the watch.

**Anchor (esp32-ai, cited):** 28.9M-param model, **9.88 tok/s**, 94.9 ms/token of
compute. In that build only the ~3.9M *dense* params are scanned per token
(4-bit, in PSRAM) ≈ ~1.95 MB read/token; the 25M embedding table contributes only
~450 B/token. So ~94.9 ms to stream ~1.95 MB from PSRAM ⇒ the token cost is
essentially **memory-bandwidth-bound at ~20 MB/s effective**.

**This model, per token:**
- Dense matmul weights scanned once/token (wq,wk,wv,wo,w1,w2,w3 ×5 layers +
  classifier) ≈ **259K fp32 MACs ≈ 1.04 MB read from flash per token**
  (contiguous, row-major — the best case for the flash cache's sequential
  prefetch). Attention dot-products add only ~70K MACs at mid-sequence.
- Compute: 259K scalar fp32 MACs. The S3's 128-bit SIMD only accelerates
  int8/int16, **not fp32**, so this is scalar FPU: ~259K × ~4–8 cyc ≈ 1–2 M cyc
  ≈ **4–9 ms/token** at 240 MHz.

**Bandwidth estimate:** at the esp32-ai-implied ~20 MB/s, 1.04 MB/token ≈ 51 ms
⇒ ~20 tok/s. Flash random reads can be ~1.5–2× slower than PSRAM (⇒ ~10 tok/s);
sequential prefetch can be faster (~40 MB/s ⇒ ~26 ms ⇒ ~38 tok/s). Adding ~5–9 ms
scalar compute:

> **Point estimate ≈ 12–20 tok/s. Honest range ≈ 8–30 tok/s.**

Faster than the esp32-ai headline because our active working set per token
(~1 MB) is about half of theirs, which roughly cancels our fp32-scalar penalty —
landing in the same order of magnitude. At ~15 tok/s a 220-token story prints in
~15 s. **Treat this as an order-of-magnitude estimate until measured on-device.**

## 7. Caveats (read these)

- **Output is toy-quality.** `stories260K` produces simple, often garbled story
  text. It cannot answer questions, follow instructions, or know facts. That is
  expected and is the honest result of the only model that fits.
- **tok/s is an estimate, not a measurement.** No flashing was done. The flash-
  read-bandwidth assumption is the biggest uncertainty; a real measurement could
  land outside 8–30 tok/s.
- **`.incbin` paths in `src/model_blob.S` are absolute.** If this folder moves,
  update them (see NOTES.md).
- **fp32, scalar.** No quantization, no SIMD. An int8 port + S3 SIMD dot-product
  kernels could be several× faster and halve flash use, but was out of scope for a
  "does it run at all" spike.
- **Not integrated into the OS launcher** — deliberately. This is a standalone
  single-purpose firmware that generates once on boot. Wiring it into EWatchOS2.1
  as an app (its own `Screen`, background task, `desiredFrameMs()` streaming) is a
  follow-up, not this spike.
