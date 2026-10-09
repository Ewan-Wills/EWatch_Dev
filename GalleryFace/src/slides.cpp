#include "slides.h"
#include <string.h>
#include <JPEGDEC.h>
#include "paintings_data.h"

namespace slides {

void *(*allocPixels)(size_t) = nullptr;
bool testBreakBlend = false;

static uint16_t *sSlot[2];
static int       sCurSlot = 0;
static int       sCurImg = 0;
static volatile int  sNextImg = 1;
static volatile bool sNextReady = false;
static uint32_t  sFails = 0;

enum Phase { HOLD, FADE };
static Phase    sPhase = HOLD;
static uint32_t sPhaseStart = 0;
static uint32_t sCurShown = 0, sNextShown = 0;   // when each started fading in

int count() { return PAINTING_COUNT; }
int16_t height(int i) { return (int16_t)PAINTINGS[i].h; }
const char *title(int i) { return (i >= 0 && i < PAINTING_COUNT) ? PAINTINGS[i].title : ""; }
uint32_t decodeFailures() { return sFails; }

// ---- decode --------------------------------------------------------------
static JPEGDEC sJpeg;
struct Target { uint16_t *px; int16_t h; };

static int onDraw(JPEGDRAW *d) {
  Target *t = (Target *)d->pUser;
  for (int r = 0; r < d->iHeight; r++) {
    int y = d->y + r;
    if (y >= t->h) break;
    int w = d->iWidth;
    if (d->x + w > W) w = W - d->x;
    if (w > 0) memcpy(t->px + (size_t)y * W + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)w * 2);
  }
  return 1;
}

bool decodeInto(int img, uint16_t *px) {
  const Painting &p = PAINTINGS[img];
  Target t{px, (int16_t)p.h};
  bool ok = sJpeg.openFLASH((uint8_t *)PAINTING_JPEG + p.off, (int)p.len, onDraw);
  if (ok) {
    sJpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    sJpeg.setUserPointer(&t);
    ok = sJpeg.decode(0, 0, 0) == 1;
    sJpeg.close();
  }
  if (!ok) {                                   // flat grey tile, keep going
    sFails++;
    for (size_t i = 0; i < (size_t)W * p.h; i++) px[i] = 0x4208;
  }
  return ok;
}

bool begin(int firstImg, uint32_t nowMs) {
  const size_t maxBytes = (size_t)W * 360 * 2;   // tools/build_paintings.py MAX_H
  for (int i = 0; i < 2; i++) {
    sSlot[i] = (uint16_t *)allocPixels(maxBytes);
    if (!sSlot[i]) return false;
  }
  sCurSlot = 0;
  sCurImg = ((firstImg % PAINTING_COUNT) + PAINTING_COUNT) % PAINTING_COUNT;
  decodeInto(sCurImg, sSlot[0]);
  sNextImg = (sCurImg + 1) % PAINTING_COUNT;
  sNextReady = false;
  sPhase = HOLD;
  sPhaseStart = sCurShown = nowMs;
  return true;
}

bool decodeStep() {
  if (sNextReady) return false;
  __sync_synchronize();
  decodeInto(sNextImg, sSlot[1 - sCurSlot]);
  __sync_synchronize();
  sNextReady = true;
  return true;
}

// ---- compose -------------------------------------------------------------
// Row y of the screen for painting px/h at pan 0..1, or nullptr for black.
static inline const uint16_t *rowOf(const uint16_t *px, int16_t h, float pan, int y) {
  int src;
  if (h >= H) src = y + (int)((h - H) * pan + 0.5f);
  else        src = y - (H - h) / 2;
  return (src >= 0 && src < h) ? px + (size_t)src * W : nullptr;
}

void view(const uint16_t *px, int16_t h, float pan, uint16_t *fb) {
  for (int y = 0; y < H; y++) {
    const uint16_t *r = rowOf(px, h, pan, y);
    if (r) memcpy(fb + (size_t)y * W, r, W * 2);
    else   memset(fb + (size_t)y * W, 0, W * 2);
  }
}

// RGB565 a*(32-k)/32 + b*k/32 for two pixels' worth of channels at once.
static inline uint16_t blend(uint16_t a, uint16_t b, uint32_t k) {
  uint32_t fa = (a | ((uint32_t)a << 16)) & 0x07E0F81F;
  uint32_t fb = (b | ((uint32_t)b << 16)) & 0x07E0F81F;
  uint32_t r = ((fa * (32 - k) + fb * k) >> 5) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

// Pan position: eased 0..1 over the whole time a painting is on screen.
static float panAt(uint32_t shownMs, uint32_t now) {
  float t = (float)(now - shownMs) / (float)(HOLD_MS + 2 * FADE_MS);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return t * t * (3 - 2 * t);
}

int render(uint32_t now, uint16_t *fb) {
  if (sPhase == HOLD && now - sPhaseStart >= HOLD_MS && sNextReady) {
    sPhase = FADE; sPhaseStart = now; sNextShown = now;
  }
  if (sPhase == FADE && now - sPhaseStart >= FADE_MS) {
    sCurSlot = 1 - sCurSlot;                 // the incoming painting is current
    sCurImg = sNextImg;
    sCurShown = sNextShown;
    sNextImg = (sCurImg + 1) % PAINTING_COUNT;
    __sync_synchronize();
    sNextReady = false;                      // old slot goes back to the decoder
    sPhase = HOLD; sPhaseStart = now;
  }

  const uint16_t *cp = sSlot[sCurSlot];
  int16_t ch = PAINTINGS[sCurImg].h;
  float cpan = panAt(sCurShown, now);
  if (sPhase == HOLD) { view(cp, ch, cpan, fb); return sCurImg; }

  const uint16_t *np = sSlot[1 - sCurSlot];
  int16_t nh = PAINTINGS[sNextImg].h;
  float npan = panAt(sNextShown, now);
  uint32_t k = (now - sPhaseStart) * 32 / FADE_MS;
  if (k > 32) k = 32;
  if (testBreakBlend && k == 16) k = 3;
  for (int y = 0; y < H; y++) {
    const uint16_t *a = rowOf(cp, ch, cpan, y), *b = rowOf(np, nh, npan, y);
    uint16_t *o = fb + (size_t)y * W;
    for (int x = 0; x < W; x++) o[x] = blend(a ? a[x] : 0, b ? b[x] : 0, k);
  }
  return k < 16 ? sCurImg : sNextImg;
}

}  // namespace slides
