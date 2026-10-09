#include "head.h"
#include <string.h>
#include <JPEGDEC.h>
#include "frames_data.h"

namespace head {

void *(*allocPixels)(size_t) = nullptr;

static uint16_t *sSlot[2];
static int       sCurSlot = 0;
static int       sCurFrame = 0;
static volatile int  sNextFrame = 1;
static volatile bool sNextReady = false;
static uint32_t  sShownAt = 0;                 // when the current frame went up
static uint32_t  sFails = 0, sLate = 0;
static int       sSkip = 0;                    // pending layer jump (renderer only)

int count() { return FRAME_COUNT; }
int layerOf(int f) { return FRAMES[((f % FRAME_COUNT) + FRAME_COUNT) % FRAME_COUNT].layer; }
const char *layerName(int l) { return (l >= 0 && l < LAYER_COUNT) ? LAYER_NAMES[l] : ""; }
uint32_t decodeFailures() { return sFails; }
uint32_t lateFrames() { return sLate; }

// ---- decode --------------------------------------------------------------
static JPEGDEC sJpeg;

static int onDraw(JPEGDRAW *d) {
  uint16_t *px = (uint16_t *)d->pUser;
  for (int r = 0; r < d->iHeight; r++) {
    int y = d->y + r;
    if (y >= H) break;
    int w = d->iWidth;
    if (d->x + w > W) w = W - d->x;
    if (w > 0) memcpy(px + (size_t)y * W + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)w * 2);
  }
  return 1;
}

bool decodeInto(int f, uint16_t *px) {
  const HeadFrame &fr = FRAMES[f];
  bool ok = sJpeg.openFLASH((uint8_t *)FRAME_JPEG + fr.off, (int)fr.len, onDraw);
  if (ok) {
    sJpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    sJpeg.setUserPointer(px);
    ok = sJpeg.decode(0, 0, 0) == 1;
    sJpeg.close();
  }
  if (!ok) {                                   // black frame, keep going
    sFails++;
    memset(px, 0, (size_t)W * H * 2);
  }
  return ok;
}

bool begin(int first, uint32_t now) {
  for (int i = 0; i < 2; i++) {
    sSlot[i] = (uint16_t *)allocPixels((size_t)W * H * 2);
    if (!sSlot[i]) return false;
  }
  sCurSlot = 0;
  sCurFrame = ((first % FRAME_COUNT) + FRAME_COUNT) % FRAME_COUNT;
  decodeInto(sCurFrame, sSlot[0]);
  sNextFrame = (sCurFrame + 1) % FRAME_COUNT;
  sNextReady = false;
  sShownAt = now;
  sSkip = 0;
  return true;
}

bool decodeStep() {
  if (sNextReady) return false;
  __sync_synchronize();
  decodeInto(sNextFrame, sSlot[1 - sCurSlot]);
  __sync_synchronize();
  sNextReady = true;
  return true;
}

void skipToLayer(int delta) { sSkip += delta; }

int tick(uint32_t now) {
  if (sNextReady && sSkip) {
    // The decoder is idle (it raised nextReady), so nextFrame is ours to
    // change: throw the decoded frame away and ask for the first frame of the
    // target layer's turn instead.
    int turn = sCurFrame / FRAMES_PER_TURN + sSkip;
    turn = ((turn % LAYER_COUNT) + LAYER_COUNT) % LAYER_COUNT;
    sSkip = 0;
    sNextFrame = turn * FRAMES_PER_TURN;
    __sync_synchronize();
    sNextReady = false;
    sShownAt = now - FRAME_MS;                 // show it as soon as it is decoded
  } else if (now - sShownAt >= FRAME_MS && sNextReady) {
    // Keep the frame clock (no drift) unless we fell more than a frame behind.
    if (now - sShownAt >= 2 * FRAME_MS) { sLate++; sShownAt = now; }
    else sShownAt += FRAME_MS;
    sCurSlot = 1 - sCurSlot;
    sCurFrame = sNextFrame;
    sNextFrame = (sCurFrame + 1) % FRAME_COUNT;
    __sync_synchronize();
    sNextReady = false;                        // old slot goes back to the decoder
  }
  return sCurFrame;
}

void copyCurrent(uint16_t *fb) { memcpy(fb, sSlot[sCurSlot], (size_t)W * H * 2); }

int render(uint32_t now, uint16_t *fb) {
  int f = tick(now);
  copyCurrent(fb);
  return f;
}

}  // namespace head
