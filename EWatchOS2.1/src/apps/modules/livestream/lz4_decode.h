// Minimal safe LZ4 block-format decoder.
//
// Sized for the Stream app: one call per received frame, decoding into a
// pre-allocated PSRAM buffer. The implementation is byte-oriented (no SIMD,
// no unaligned wide-word copies) — it sustains 50–100 MB/s on the ESP32-S3
// which is plenty for our 134 KB frames (≈2 ms decode budget).
//
// Compatible with the LZ4 *block* format (what Python's
// `lz4.block.compress(data, store_size=False)` produces). The framed format
// `lz4.frame` is NOT supported — that's deliberate, the wire format puts the
// frame length in our own header instead.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Decompress one LZ4 block. Returns the number of bytes written on success,
// or -1 on any malformed input (bad token, truncated stream, offset before
// start of output, or output overrun). Output is written byte-by-byte so
// overlapping back-references are handled correctly.
int lz4_decompress_block(const uint8_t *src, int src_len,
                         uint8_t *dst, int dst_cap);
