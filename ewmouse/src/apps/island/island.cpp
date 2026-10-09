#include <Arduino_GFX_Library.h>
#include <math.h>
#include "island.h"
#include "r3d.h"
#include "display.h"     // gfx, frameCanvas
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model + ModelLock
#include "view.h"        // switchTo, tappedBack, Screen

// ============================================================================
// World layout — one source of truth for both the geometry build and the 2D
// overlays (clock hands, light glows) that anchor onto landmarks.
// ============================================================================
static const float kGrassY   = 0.0f;     // top of the island = walking plane
static const float kGrassR   = 9.0f;     // island radius

// Landmark anchors (x, y, z).  z is "depth": camera sits at +z looking toward
// -z, so larger z is nearer the viewer / front of the diorama.
static const Vec3 kClockBase   = vec3( 0.0f, 0.0f, -2.0f);
static const Vec3 kClockFace   = vec3( 0.0f, 3.1f, -2.0f + 1.16f);   // dial center, front face
static const Vec3 kLighthouse  = vec3( 5.6f, 0.0f, -1.2f);
static const Vec3 kLightLamp   = vec3( 5.6f, 4.35f, -1.2f);
static const Vec3 kCabin       = vec3(-5.0f, 0.0f,  1.6f);
static const Vec3 kCabinWindow = vec3(-5.0f, 1.05f, 1.6f + 1.55f);
static const Vec3 kCampfire    = vec3( 2.6f, 0.0f,  4.2f);
static const Vec3 kPond        = vec3(-2.6f, 0.0f,  4.0f);

// Fixed idle-postcard camera — a slightly elevated 3/4 view that keeps the
// clock tower readable and the whole island in frame.
static const Camera kCam = {
  /*eye   */ vec3(0.0f, 11.5f, 18.0f),
  /*target*/ vec3(0.0f,  2.2f, -0.5f),
  /*up    */ vec3(0.0f,  1.0f,  0.0f),
  /*focal */ 232.0f,
};

static Scene gScene;     // ~28 KiB, file-scope so it never hits the task stack

// ============================================================================
// Geometry — built once. Procedural primitives keep this compact and easy to
// re-art-direct; colours here are base albedo, the day/night light tints them.
// ============================================================================
static void buildTree(Scene &s, float x, float z, float scale) {
  // Trunk + two stacked canopy cones (a little Animal-Crossing topiary).
  s.box(vec3(x, 0.55f * scale, z), vec3(0.16f * scale, 0.55f * scale, 0.16f * scale),
        90, 64, 40);
  s.cone(x, z, 0.7f * scale, 1.05f * scale, 1.5f * scale, 7,  64, 132, 58);
  s.cone(x, z, 1.7f * scale, 0.78f * scale, 1.3f * scale, 7,  78, 150, 66);
}

static void buildIsland(Scene &s) {
  s.clear();

  // --- floating island body: grass disc on top, rock frustum tapering to a
  //     hanging point underneath ---
  s.frustum(0, 0, -5.5f, 1.4f, kGrassR + 0.2f, 5.5f, 12, 96, 74, 52,
            /*capTop=*/false, 0, 0, 0);                  // upper rock sides
  s.frustum(0, 0, -8.5f, 0.05f, 1.4f, 3.0f, 12, 84, 64, 46,
            /*capTop=*/false, 0, 0, 0);                  // lower rock → hanging point
  s.disc(0, 0, kGrassY + 0.06f, kGrassR, 16, 96, 168, 78);  // grass top
  // a soft dirt rim just inside the edge
  s.disc(0, 0, kGrassY + 0.04f, kGrassR + 0.18f, 16, 120, 96, 66);

  // --- pond: a flat water disc, slightly inset ---
  s.disc(kPond.x, kPond.z, kGrassY + 0.05f, 2.2f, 14, 64, 120, 168,
         Scene::DOUBLE_SIDED);

  // --- clock tower (the hero): octagonal stone shaft + belfry + roof ---
  s.frustum(kClockBase.x, kClockBase.z, 0.0f, 1.30f, 1.08f, 4.4f, 8,
            178, 170, 152, /*capTop=*/true, 150, 142, 126);   // shaft
  s.frustum(kClockBase.x, kClockBase.z, 4.4f, 0.92f, 0.92f, 0.8f, 8,
            150, 142, 126, /*capTop=*/true, 132, 124, 110);   // belfry
  s.cone(kClockBase.x, kClockBase.z, 5.2f, 1.32f, 1.7f, 8, 150, 60, 52);  // roof

  // --- lighthouse: white tower with one red band + lantern room + cap ---
  s.frustum(kLighthouse.x, kLighthouse.z, 0.0f, 0.85f, 0.66f, 2.6f, 10,
            228, 226, 222, /*capTop=*/false, 0, 0, 0);
  s.frustum(kLighthouse.x, kLighthouse.z, 2.6f, 0.66f, 0.6f, 0.7f, 10,
            196, 64, 56, /*capTop=*/false, 0, 0, 0);            // red band
  s.frustum(kLighthouse.x, kLighthouse.z, 3.3f, 0.6f, 0.58f, 0.7f, 10,
            228, 226, 222, /*capTop=*/true, 90, 92, 96);        // lantern housing
  s.cone(kLighthouse.x, kLighthouse.z, 4.0f, 0.7f, 0.8f, 10, 60, 62, 70);  // cap

  // --- cabin: wood box + slate roof + glowing window (front, +z face) ---
  s.box(kCabin, vec3(1.3f, 0.95f, 1.3f), 150, 96, 60);
  s.frustum(kCabin.x, kCabin.z, 1.9f, 1.7f, 0.05f, 1.1f, 4, 120, 70, 58,
            /*capTop=*/false, 0, 0, 0);                          // pyramid roof
  // window quad sits just proud of the +z wall so it always draws on top
  {
    float wz = kCabin.z + 1.31f;
    s.quad(vec3(kCabin.x - 0.45f, 0.55f, wz), vec3(kCabin.x + 0.45f, 0.55f, wz),
           vec3(kCabin.x + 0.45f, 1.45f, wz), vec3(kCabin.x - 0.45f, 1.45f, wz),
           120, 150, 170);
  }

  // --- campfire: a little stack of logs + ember disc (the power-off object) ---
  s.box(vec3(kCampfire.x, 0.12f, kCampfire.z), vec3(0.55f, 0.1f, 0.18f), 96, 64, 40);
  s.box(vec3(kCampfire.x, 0.12f, kCampfire.z), vec3(0.18f, 0.1f, 0.55f), 110, 72, 46);
  s.disc(kCampfire.x, kCampfire.z, 0.24f, 0.42f, 8, 200, 90, 40);  // embers

  // --- jetty reaching off the front edge toward the camera ---
  s.box(vec3(0.0f, 0.18f, kGrassR - 0.3f + 1.2f), vec3(0.7f, 0.12f, 1.6f), 130, 96, 62);

  // --- scatter: trees + a couple of rocks ---
  buildTree(s, -6.8f, -2.8f, 1.05f);
  buildTree(s,  6.9f,  2.6f, 1.0f);
  buildTree(s,  3.6f, -5.0f, 0.85f);
  buildTree(s, -3.0f, -5.6f, 0.95f);
  buildTree(s, -7.0f,  2.2f, 0.8f);
  s.frustum( 7.4f, -3.6f, 0.0f, 0.7f, 0.5f, 0.55f, 6, 130, 128, 122, true, 120, 118, 112);
  s.frustum(-1.5f,  6.2f, 0.0f, 0.55f, 0.4f, 0.4f, 6, 124, 120, 116, true, 112, 110, 104);

  s.finalize();
}

// ============================================================================
// Day / night cycle — keyframed across the 24h day, interpolated by RTC time.
// Each key carries the sky gradient (top + horizon) and the lighting (cool
// ambient skylight + warm directional sun).  The "money shot" golden hour
// leans into #E86B2B.
// ============================================================================
struct DayKey {
  float t;                          // hour 0..24
  uint8_t topR, topG, topB;         // sky gradient — top of screen
  uint8_t botR, botG, botB;         // sky gradient — horizon
  float ambR, ambG, ambB;           // ambient (skylight) multipliers
  float sunR, sunG, sunB;           // directional key multipliers
};

static const DayKey kDay[] = {
  // t      topRGB            botRGB             ambient                sun
  {  0.0f,  12, 14, 30,       28, 30, 54,        0.28f,0.30f,0.44f,     0.10f,0.12f,0.24f },
  {  5.0f,  34, 34, 66,       96, 76,100,        0.34f,0.34f,0.50f,     0.22f,0.20f,0.32f },
  {  6.5f, 120,140,190,      248,176,150,        0.50f,0.50f,0.58f,     1.05f,0.72f,0.55f },
  {  9.0f,  86,150,224,      182,212,232,        0.55f,0.60f,0.68f,     1.05f,0.98f,0.86f },
  { 12.0f,  62,140,232,      166,206,240,        0.58f,0.64f,0.72f,     1.12f,1.06f,0.96f },
  { 16.5f,  98,138,214,      226,150, 92,        0.56f,0.55f,0.60f,     1.16f,0.86f,0.56f },
  { 18.0f, 116, 96,168,      232,107, 43,        0.48f,0.42f,0.46f,     1.22f,0.62f,0.28f },
  { 19.5f,  48, 40, 96,      150, 74, 86,        0.36f,0.32f,0.48f,     0.55f,0.30f,0.40f },
  { 21.0f,  16, 18, 40,       40, 38, 66,        0.30f,0.30f,0.46f,     0.14f,0.16f,0.28f },
  { 24.0f,  12, 14, 30,       28, 30, 54,        0.28f,0.30f,0.44f,     0.10f,0.12f,0.24f },
};
static const int kDayN = sizeof(kDay) / sizeof(kDay[0]);

static inline float flerp(float a, float b, float u) { return a + (b - a) * u; }
static inline uint8_t blerp(uint8_t a, uint8_t b, float u) {
  return (uint8_t)(a + (b - a) * u + 0.5f);
}

struct DayState {
  uint8_t  topR, topG, topB, botR, botG, botB;
  Lighting light;
  float    nightAmt;       // 0 = full day, 1 = deep night (drives stars/glows)
};

static DayState sampleDay(float tod) {
  if (tod < 0) tod = 0; else if (tod >= 24) tod = 23.999f;
  int i = 0;
  while (i < kDayN - 1 && kDay[i + 1].t <= tod) i++;
  const DayKey &a = kDay[i];
  const DayKey &b = kDay[(i + 1 < kDayN) ? i + 1 : i];
  float span = (b.t - a.t);
  float u = (span > 1e-4f) ? (tod - a.t) / span : 0.0f;

  DayState d;
  d.topR = blerp(a.topR, b.topR, u); d.topG = blerp(a.topG, b.topG, u); d.topB = blerp(a.topB, b.topB, u);
  d.botR = blerp(a.botR, b.botR, u); d.botG = blerp(a.botG, b.botG, u); d.botB = blerp(a.botB, b.botB, u);
  d.light.ambR = flerp(a.ambR, b.ambR, u);
  d.light.ambG = flerp(a.ambG, b.ambG, u);
  d.light.ambB = flerp(a.ambB, b.ambB, u);
  d.light.sunR = flerp(a.sunR, b.sunR, u);
  d.light.sunG = flerp(a.sunG, b.sunG, u);
  d.light.sunB = flerp(a.sunB, b.sunB, u);

  // Sun elevation over the day → light direction + how "night" it is.
  float p = (tod - 6.0f) / 12.0f;             // 0 at sunrise, 1 at sunset
  float elev = sinf(p * (float)M_PI);          // >0 while the sun is up
  if (p < 0 || p > 1) elev = -0.2f;
  if (elev > 0.0f) {
    // Directional sun: swings E→W, always biased toward the camera (+z) so the
    // faces we actually see get the warm key light.
    d.light.sunDir = vnorm(vec3(cosf(p * (float)M_PI), fmaxf(0.20f, elev), 0.5f));
  } else {
    d.light.sunDir = vnorm(vec3(-0.30f, 0.65f, 0.5f));   // soft moonlight
  }
  d.nightAmt = elev > 0 ? fmaxf(0.0f, 1.0f - elev * 2.6f) : 1.0f;
  return d;
}

// ============================================================================
// Sky + celestial bodies (2D, drawn straight into the canvas framebuffer).
// ============================================================================
static void fillSky(Arduino_Canvas *cv, const DayState &d) {
  uint16_t *fb = cv->getFramebuffer();
  if (!fb) return;
  int16_t W = cv->width(), H = cv->height();
  for (int16_t y = 0; y < H; y++) {
    float u = (float)y / (H - 1);
    uint16_t c = rgb565(blerp(d.topR, d.botR, u),
                        blerp(d.topG, d.botG, u),
                        blerp(d.topB, d.botB, u));
    uint16_t *row = fb + (int)y * W;
    for (int16_t x = 0; x < W; x++) row[x] = c;
  }
}

// A cheap deterministic star field — fixed positions so they don't crawl.
static void drawStars(Arduino_Canvas *cv, float alpha) {
  if (alpha <= 0.02f) return;
  static uint16_t sx[60], sy[60], sb[60];
  static bool init = false;
  if (!init) {
    uint32_t seed = 0x1234567u;
    for (int i = 0; i < 60; i++) {
      seed = seed * 1664525u + 1013904223u; sx[i] = seed % cv->width();
      seed = seed * 1664525u + 1013904223u; sy[i] = (seed % 150);          // upper sky
      seed = seed * 1664525u + 1013904223u; sb[i] = 140 + (seed % 116);
    }
    init = true;
  }
  for (int i = 0; i < 60; i++) {
    int v = (int)(sb[i] * alpha);
    cv->drawPixel(sx[i], sy[i], rgb565(v, v, (int)(v * 1.05f)));
  }
}

static void drawSunMoon(Arduino_Canvas *cv, float tod, const DayState &d) {
  float p = (tod - 6.0f) / 12.0f;
  if (p >= 0.0f && p <= 1.0f) {
    // Sun: warm core + soft halo. Colour shifts toward the horizon hue at the
    // ends of the arc so dawn/dusk suns glow orange.
    int sxp = (int)(24 + p * 192);
    int syp = (int)(132 - sinf(p * (float)M_PI) * 104);
    int warm = (int)(120 * (1.0f - sinf(p * (float)M_PI)));   // more orange low down
    cv->fillCircle(sxp, syp, 22, rgb565(255, 200 - warm, 120 - warm));
    cv->fillCircle(sxp, syp, 14, rgb565(255, 244, 200));
  } else {
    // Moon: pale disc with a couple of craters, fading in with the night.
    float q = (tod > 18.0f) ? (tod - 18.0f) / 12.0f : (tod + 6.0f) / 12.0f;
    int mxp = (int)(24 + q * 192);
    int myp = (int)(132 - sinf(q * (float)M_PI) * 104);
    float a = fminf(1.0f, d.nightAmt + 0.2f);
    int base = (int)(225 * a);
    cv->fillCircle(mxp, myp, 13, rgb565(base, base, (int)(base * 0.92f)));
    cv->fillCircle(mxp - 4, myp - 3, 3, rgb565((int)(base * 0.8f), (int)(base * 0.8f), (int)(base * 0.74f)));
    cv->fillCircle(mxp + 3, myp + 4, 2, rgb565((int)(base * 0.8f), (int)(base * 0.8f), (int)(base * 0.74f)));
  }
}

// ============================================================================
// Clock-tower hands (monumental exact read) — projected onto the tower face.
// ============================================================================
static void drawHand(Arduino_Canvas *cv, float cx, float cy, float ang,
                     float len, float halfW, uint16_t color) {
  float dx = sinf(ang), dy = -cosf(ang);       // 0 rad = 12 o'clock (up), CW
  float pxx = cosf(ang), pyy = sinf(ang);       // perpendicular
  float tx = cx + dx * len, ty = cy + dy * len;
  cv->fillTriangle((int16_t)(cx + pxx * halfW), (int16_t)(cy + pyy * halfW),
                   (int16_t)(cx - pxx * halfW), (int16_t)(cy - pyy * halfW),
                   (int16_t)tx, (int16_t)ty, color);
}

static void drawClock(Arduino_Canvas *cv, int hour, int minute, int second,
                      bool rtcOk) {
  float sx, sy, depth;
  if (!projectPoint(kCam, cv, kClockFace, sx, sy, depth)) return;
  float R = kCam.focal * 1.35f / depth;         // dial radius in px (slightly
  if (R < 9) R = 9;                             // oversized for legibility)
  // Dial: stone rim + pale face.
  cv->fillCircle((int16_t)sx, (int16_t)sy, (int16_t)(R + 2), rgb565(70, 64, 56));
  cv->fillCircle((int16_t)sx, (int16_t)sy, (int16_t)R,       rgb565(238, 232, 214));
  // Hour ticks.
  for (int i = 0; i < 12; i++) {
    float a = i / 12.0f * 2.0f * (float)M_PI;
    int x0 = (int)(sx + sinf(a) * R * 0.82f), y0 = (int)(sy - cosf(a) * R * 0.82f);
    cv->fillCircle(x0, y0, (R > 16) ? 2 : 1, rgb565(60, 54, 48));
  }
  if (!rtcOk) return;
  float ah = ((hour % 12) + minute / 60.0f) / 12.0f * 2.0f * (float)M_PI;
  float am = (minute + second / 60.0f) / 60.0f * 2.0f * (float)M_PI;
  drawHand(cv, sx, sy, ah, R * 0.52f, fmaxf(2.0f, R * 0.10f), rgb565(40, 36, 32));
  drawHand(cv, sx, sy, am, R * 0.84f, fmaxf(1.5f, R * 0.07f), rgb565(40, 36, 32));
  cv->fillCircle((int16_t)sx, (int16_t)sy, (R > 16) ? 3 : 2, rgb565(150, 60, 52));
}

// A warm light "glow" overlay anchored on a 3D point — sells lamps/fire at
// night. Radius + alpha grow with how dark it is.
static void drawGlow(Arduino_Canvas *cv, Vec3 anchor, float nightAmt,
                     uint8_t r, uint8_t g, uint8_t b, float scale) {
  if (nightAmt < 0.3f) return;
  float sx, sy, depth;
  if (!projectPoint(kCam, cv, anchor, sx, sy, depth)) return;
  float rad = (kCam.focal * scale / depth) * (0.6f + 0.6f * nightAmt);
  float a = (nightAmt - 0.3f) / 0.7f;
  if (a > 1) a = 1;
  cv->fillCircle((int16_t)sx, (int16_t)sy, (int16_t)(rad),
                 rgb565((int)(r * a), (int)(g * a), (int)(b * a)));
  cv->fillCircle((int16_t)sx, (int16_t)sy, (int16_t)(rad * 0.5f),
                 rgb565((int)(255 * a), (int)(fminf(255, g + 60) * a), (int)(fminf(255, b + 40) * a)));
}

// ============================================================================
// IslandView.
// ============================================================================
void IslandView::onEnter() {
  if (!sceneBuilt) { buildIsland(gScene); sceneBuilt = true; }
  lastMinute = -1;
  if (gfx) gfx->fillScreen(BLACK);   // hide whatever was on the panel until first flush
}

void IslandView::onExit() {}

void IslandView::render() {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) return;

  // Snapshot the time once.
  uint8_t h, m, sec; bool rtcOk;
  { ModelLock lk; h = model.hour; m = model.minute; sec = model.second; rtcOk = model.rtcOk; }
  float tod = h + m / 60.0f + sec / 3600.0f;

  DayState day = sampleDay(tod);

  // 1. Sky gradient + celestial body + stars.
  fillSky(cv, day);
  drawStars(cv, day.nightAmt);
  drawSunMoon(cv, tod, day);

  // 2. The 3D island. Culling is OFF for now — painter's algorithm hides
  //    back faces correctly for these convex solids, and this avoids any
  //    invisible-hole risk from a mis-wound primitive. Flip to true as a
  //    perf pass once the geometry is visually confirmed.
  renderScene(cv, gScene, kCam, day.light, /*cull=*/false);

  // 3. Light glows at dusk/night — fire, cabin window, lighthouse lantern.
  drawGlow(cv, kCampfire,    day.nightAmt, 230, 120, 40, 0.7f);
  drawGlow(cv, kCabinWindow, day.nightAmt, 240, 200, 90, 0.45f);
  drawGlow(cv, kLightLamp,   day.nightAmt, 240, 240, 210, 0.4f);

  // 4. The monumental read — clock-tower hands.
  drawClock(cv, h, m, sec, rtcOk);

  cv->flush();
  lastMinute = m;
}

void IslandView::onEvent(const Event &e) {
  // Back: hardware button (short) and right-swipe both leave to the launcher.
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Gesture &&
      (e.gesture == Gesture::SwipeRight || e.gesture == Gesture::SwipeLeft)) {
    hapticBuzz(50, 60);
    switchTo(Screen::AppList);
    return;
  }
}
