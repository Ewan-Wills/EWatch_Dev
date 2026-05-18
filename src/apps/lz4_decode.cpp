#include "lz4_decode.h"
#include <string.h>

int lz4_decompress_block(const uint8_t *src, int src_len,
                         uint8_t *dst, int dst_cap) {
  if (src_len < 0 || dst_cap < 0) return -1;

  const uint8_t *ip   = src;
  const uint8_t *iend = src + src_len;
  uint8_t       *op   = dst;
  uint8_t       *oend = dst + dst_cap;

  while (ip < iend) {
    const unsigned token = *ip++;

    // ---- literals ----
    unsigned lit = token >> 4;
    if (lit == 15) {
      unsigned s;
      do {
        if (ip >= iend) return -1;
        s = *ip++;
        lit += s;
        if (lit > (unsigned)dst_cap) return -1;     // sanity
      } while (s == 255);
    }
    if ((size_t)(iend - ip) < lit) return -1;
    if ((size_t)(oend - op) < lit) return -1;
    memcpy(op, ip, lit);
    op += lit;
    ip += lit;

    // Last sequence has only literals; the block ends with the input.
    if (ip == iend) break;

    // ---- match ----
    if ((size_t)(iend - ip) < 2) return -1;
    const unsigned offset = (unsigned)ip[0] | ((unsigned)ip[1] << 8);
    ip += 2;
    if (offset == 0) return -1;
    if (offset > (unsigned)(op - dst)) return -1;

    unsigned mlen = token & 0x0F;
    if (mlen == 15) {
      unsigned s;
      do {
        if (ip >= iend) return -1;
        s = *ip++;
        mlen += s;
        if (mlen > (unsigned)dst_cap) return -1;    // sanity
      } while (s == 255);
    }
    mlen += 4;   // LZ4 minimum match length

    if ((size_t)(oend - op) < mlen) return -1;

    // Byte-by-byte to support overlapping (RLE-style) copies where
    // offset < mlen — e.g. offset==1 turns into "fill with last byte".
    const uint8_t *m = op - offset;
    for (unsigned i = 0; i < mlen; i++) *op++ = *m++;
  }

  if (ip != iend) return -1;   // trailing garbage
  return (int)(op - dst);
}
