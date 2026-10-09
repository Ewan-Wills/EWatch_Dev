// Host harness for HeadFace: real head.cpp + JPEGDEC + clock_draw.cpp, no
// hardware. See test/render.sh.
//   host_test <outdir> [--control-timing]   (control: checker must fail)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "head.h"
#include "clock_draw.h"
#include "frames_data.h"
#include "font_data.h"

using namespace head;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
typedef std::vector<uint16_t> Buf;

static Buf decoded(int f) { Buf b((size_t)W * H); decodeInto(f, b.data()); return b; }
static void dump(const std::string &path, const Buf &b) {
  FILE *o = fopen(path.c_str(), "wb"); fwrite(b.data(), 2, b.size(), o); fclose(o);
}

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : "out";
  bool controlTiming = argc > 2 && !strcmp(argv[2], "--control-timing");
  allocPixels = [](size_t n) -> void * { return malloc(n); };
  const int N = count();
  printf("%d frames, %d per turn, %d layers\n", N, FRAMES_PER_TURN, LAYER_COUNT);

  // --video <file>: one whole loop exactly as the screen shows it (firmware
  // decode + clock at 10:09), raw RGB565, for ffmpeg. Nothing else runs.
  if (argc > 3 && !strcmp(argv[2], "--video")) {
    FILE *v = fopen(argv[3], "wb");
    for (int i = 0; i < N; i++) {
      Buf b = decoded(i);
      ClockInput in{true, 10, 9, 1, 28, 9, (int8_t)layerOf(i), layerName(layerOf(i))};
      drawClock(b.data(), in);
      fwrite(b.data(), 2, b.size(), v);
    }
    fclose(v);
    return decodeFailures() ? 1 : 0;
  }
  CHECK(N == FRAMES_PER_TURN * LAYER_COUNT, "frame count %d != %d x %d", N, FRAMES_PER_TURN, LAYER_COUNT);

  // 1. Timing: with the decoder keeping up, frame k is on screen from k*FRAME_MS,
  //    in order, over two whole loops (so the wrap N-1 -> 0 is covered).
  //    The control expects a 1 ms longer period, so it must fail.
  { const uint32_t expect = controlTiming ? FRAME_MS + 1 : FRAME_MS;
    Buf fb((size_t)W * H);
    begin(0, 0);
    int bad = 0;
    for (uint32_t t = 0; t < 2 * N * FRAME_MS; t += 5) {
      decodeStep();
      int f = render(t, fb.data());
      int want = (int)((t / expect) % N);
      if (f != want && bad++ < 3) printf("  t=%u shown %d want %d\n", t, f, want);
    }
    CHECK(bad == 0, "frame timing wrong at %d instants", bad);
    CHECK(lateFrames() == 0, "%u late frames with a decoder that keeps up", lateFrames());
    if (controlTiming) { printf(fails ? "HOST TEST FAILED (%d)\n" : "HOST TEST PASSED\n", fails); return fails ? 1 : 0; }
  }

  // 2. Every frame decodes (pixels dumped for the Pillow cross-check), and the
  //    frame on screen is the decoded frame, bit for bit.
  { FILE *d = fopen((out + "/decoded.bin").c_str(), "wb");
    for (int i = 0; i < N; i++) { Buf b = decoded(i); fwrite(b.data(), 2, b.size(), d); }
    fclose(d);
    CHECK(decodeFailures() == 0, "%u JPEG decode failures", decodeFailures());
    Buf fb((size_t)W * H); begin(0, 0);
    for (int k = 1; k <= 3; k++) { decodeStep(); render(k * FRAME_MS, fb.data()); }
    CHECK(fb == decoded(3), "screen is not frame 3 after 3 frame periods"); }

  // 3. A slow decoder: the head waits (never skips a frame) and counts it late.
  { Buf fb((size_t)W * H); begin(10, 0);
    int f = render(3 * FRAME_MS, fb.data());
    CHECK(f == 10, "advanced to %d without a decoded frame", f);
    decodeStep(); f = render(3 * FRAME_MS + 1, fb.data());
    CHECK(f == 11 && lateFrames() == 1, "after the decoder caught up: frame %d late %u (want 11, 1)", f, lateFrames());
    decodeStep(); f = render(4 * FRAME_MS, fb.data());
    CHECK(f == 11, "frame clock not restarted after a late frame (showed %d)", f);
    f = render(3 * FRAME_MS + 1 + FRAME_MS, fb.data());
    CHECK(f == 12 && lateFrames() == 1, "next frame not on time after a late one (showed %d, late %u)", f, lateFrames()); }

  // 4. Swipes jump to the start of the next / previous layer's turn, with wrap.
  { Buf fb((size_t)W * H);
    struct { int from, delta, want; } cases[] = {
      {10, +1, FRAMES_PER_TURN}, {10, -1, 3 * FRAMES_PER_TURN},
      {3 * FRAMES_PER_TURN + 5, +1, 0}, {FRAMES_PER_TURN + 90, +2, 3 * FRAMES_PER_TURN} };
    for (auto &c : cases) {
      begin(c.from, 0); decodeStep();         // decoder idle holding c.from+1
      skipToLayer(c.delta);
      int f = render(1, fb.data());           // swaps the request in
      CHECK(f == c.from, "skip showed %d before the target was decoded", f);
      decodeStep(); f = render(2, fb.data());
      CHECK(f == c.want, "skip %+d from %d showed %d want %d", c.delta, c.from, f, c.want);
      CHECK(fb == decoded(c.want), "skip target pixels are not frame %d", c.want);
    } }

  // 5. Layer labels: a turn shows its own layer until halfway through the wipe.
  { for (int l = 0; l < LAYER_COUNT; l++) {
      CHECK(layerOf(l * FRAMES_PER_TURN) == l, "turn %d starts on layer %d", l, layerOf(l * FRAMES_PER_TURN));
      CHECK(layerOf(l * FRAMES_PER_TURN + FRAMES_PER_TURN - 1) == (l + 1) % LAYER_COUNT,
            "turn %d does not end on the next layer", l);
    } }

  // 6. Clock: changing the minute's last digit changes pixels only inside
  //    that digit's cell (the colon and other digits do not jiggle). Then
  //    raw dumps of a few frames with the clock, for eyeballing.
  { Buf base = decoded(20), a = base, b = base;
    ClockInput in{true, 12, 34, 1, 28, 9, 0, "SKIN"};
    drawClock(a.data(), in); in.m = 35; drawClock(b.data(), in);
    int w = textWidth(true, "12:34", 1), x0 = W / 2 - w / 2, lastCell = x0 + w - FONT_BIG.digitAdv;
    int changed = 0, outside = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
      if (a[y * W + x] != b[y * W + x]) { changed++; if (x < lastCell - 4 || y > 80) outside++; }
    CHECK(changed > 150, "12:34 -> 12:35 changed only %d pixels", changed);
    CHECK(outside == 0, "%d changed pixels outside the last digit cell", outside);
    CHECK(a != base, "clock drew nothing");
    // Every glyph the clock can print exists (a missing one is silently skipped).
    CHECK(textWidth(true, "--:--", 1) > textWidth(true, "00", 1) + 20, "big font lacks '-' (no-RTC --:--)");
    CHECK(textWidth(false, "VENTRICLES WED 30 SEP", 3) > 150, "small font lacks a label/date glyph"); }

  struct Shot { int frame; uint8_t h, m, wd, day, mon; bool ok; const char *name; };
  int T = FRAMES_PER_TURN;
  Shot shots[] = {
    {0, 9, 41, 1, 28, 9, true, "1_skin_front"}, {T / 4, 10, 8, 2, 29, 9, true, "2_skin_side"},
    {T - 20, 12, 0, 3, 30, 9, true, "3_wipe_skin_skull"}, {T + T / 3, 13, 27, 4, 1, 10, true, "4_skull"},
    {2 * T + T / 2, 18, 55, 5, 2, 10, true, "5_brain_back"}, {2 * T + T - 12, 21, 3, 6, 3, 10, true, "6_wipe_brain_vent"},
    {3 * T + T / 8, 23, 59, 0, 4, 10, true, "7_ventricles"}, {3 * T + T - 8, 0, 0, 0, 0, 0, false, "8_wipe_back_to_skin_nortc"} };
  for (auto &s : shots) {
    Buf b = decoded(s.frame);
    int l = layerOf(s.frame);
    ClockInput in{s.ok, s.h, s.m, s.wd, s.day, s.mon, (int8_t)l, layerName(l)};
    drawClock(b.data(), in);
    dump(out + "/" + s.name + ".raw", b);
  }
  printf(fails ? "HOST TEST FAILED (%d)\n" : "HOST TEST PASSED\n", fails);
  return fails ? 1 : 0;
}
