// Host harness for GalleryFace: real slides.cpp + JPEGDEC + face_draw.cpp +
// qr_draw.cpp + Arduino_GFX + QRCode, no hardware. See test/render.sh.
//   host_test <outdir> [--break-blend]   (--break-blend: checker control)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "hostgfx.h"
#include "slides.h"
#include "face_draw.h"
#include "qr_draw.h"

using namespace slides;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
typedef std::vector<uint16_t> Buf;

static Buf decoded(int img) { Buf b((size_t)W * height(img)); decodeInto(img, b.data()); return b; }
static Buf viewOf(int img, float pan) { Buf d = decoded(img), f((size_t)W * H); view(d.data(), height(img), pan, f.data()); return f; }
// Same easing as slides.cpp panAt() (kept independent on purpose).
static float pan(uint32_t shown, uint32_t now) {
  float t = (float)(now - shown) / (HOLD_MS + 2 * FADE_MS); t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}
static int maxChanDiffFromMid(const Buf &f, const Buf &a, const Buf &b) {
  int worst = 0;
  for (size_t i = 0; i < f.size(); i++) {
    int sh[3] = {11, 5, 0}, mk[3] = {31, 63, 31};
    for (int c = 0; c < 3; c++) {
      int fa = (a[i] >> sh[c]) & mk[c], fb = (b[i] >> sh[c]) & mk[c], ff = (f[i] >> sh[c]) & mk[c];
      int d = std::abs(2 * ff - (fa + fb));        // 2x to stay integer
      if (d > worst) worst = d;
    }
  }
  return worst;                                    // <= 2 means within 1 LSB
}

// Run one slideshow from painting `first` for `cycles` transitions, checking
// the exact frames at each fade's start, middle and end.
static void checkFades(int first, int cycles, const char *label) {
  Buf fb((size_t)W * H);
  begin(first, 0);
  uint32_t shownCur = 0, t = 0;
  int cur = first;
  for (int c = 0; c < cycles; c++) {
    int next = (cur + 1) % count();
    decodeStep();                                  // decoder keeps up
    uint32_t f0 = t + HOLD_MS;
    int s = render(f0 - 1, fb.data());
    CHECK(s == cur && fb == viewOf(cur, pan(shownCur, f0 - 1)), "%s cycle %d: hold frame wrong", label, c);
    render(f0, fb.data());
    CHECK(fb == viewOf(cur, pan(shownCur, f0)), "%s cycle %d: fade start is not the pure current painting", label, c);
    uint32_t mid = f0 + FADE_MS / 2;
    render(mid, fb.data());
    int d = maxChanDiffFromMid(fb, viewOf(cur, pan(shownCur, mid)), viewOf(next, pan(f0, mid)));
    CHECK(d <= 2, "%s cycle %d: mid-fade frame is not a 50/50 blend (off by %d/2 LSB)", label, c, d);
    s = render(f0 + FADE_MS, fb.data());
    CHECK(s == next && fb == viewOf(next, pan(f0, f0 + FADE_MS)), "%s cycle %d: fade end is not the pure next painting (shown %d want %d)", label, c, s, next);
    cur = next; shownCur = f0; t = f0 + FADE_MS;
  }
}

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : "out";
  testBreakBlend = argc > 2 && !strcmp(argv[2], "--break-blend");
  allocPixels = [](size_t n) -> void * { return malloc(n); };
  const int N = count();
  printf("%d paintings\n", N);

  // 1. Every painting decodes (pixels dumped for the Pillow cross-check).
  FILE *dump = fopen((out + "/decoded.bin").c_str(), "wb");
  for (int i = 0; i < N; i++) { Buf b = decoded(i); fwrite(b.data(), 2, b.size(), dump); }
  fclose(dump);
  CHECK(decodeFailures() == 0, "%u JPEG decode failures", decodeFailures());

  // 2. Crossfades: exact start/mid/end frames, order, and the wrap N-1 -> 0.
  checkFades(10, 4, "run");
  checkFades(N - 2, 3, "wrap");

  // 3. A fade must wait for the decoder: no decodeStep() -> still holding.
  { Buf fb((size_t)W * H); begin(5, 0);
    int s = render(HOLD_MS + 500, fb.data());
    CHECK(s == 5 && fb == viewOf(5, pan(0, HOLD_MS + 500)), "fade started before the next painting was decoded");
    decodeStep(); render(HOLD_MS + 600, fb.data()); render(HOLD_MS + 600 + FADE_MS, fb.data());
    CHECK(render(HOLD_MS + 601 + FADE_MS, fb.data()) == 6, "fade did not run once the decoder caught up"); }

  // 4. PNGs for eyeballing.
  HostGFX g(W, H);
  FaceInput in{true, 10, 9, 42, 0.3f, 4, 25, 9, nullptr};
  begin(3, 0); decodeStep();
  int s = render(2000, g.fb.data()); in.title = title(s); drawFace(&g, in);
  g.writePNG((out + "/1_face.png").c_str());
  render(HOLD_MS, g.fb.data()); s = render(HOLD_MS + FADE_MS / 2, g.fb.data());
  in.title = title(s); in.h = 15; in.m = 47; drawFace(&g, in);
  g.writePNG((out + "/2_face_midfade.png").c_str());
  render(HOLD_MS + FADE_MS + 3000, g.fb.data());
  g.writePNG((out + "/3_screensaver.png").c_str());
  const char *cards[2][3] = {
    { "Find the gallery", "Directions in Google Maps", "https://maps.app.goo.gl/LDtHCnFEXopYkxdy5" },
    { "clairewills.co.uk", "See all the paintings online", "https://clairewills.co.uk" } };
  for (int i = 0; i < 2; i++) {
    CHECK(drawQR(&g, cards[i][0], cards[i][1], cards[i][2], i + 1, 3), "QR %d did not fit", i);
    g.writePNG((out + "/" + std::to_string(4 + i) + "_qr.png").c_str());
  }
  printf(fails ? "HOST TEST FAILED (%d)\n" : "HOST TEST PASSED\n", fails);
  return fails ? 1 : 0;
}
