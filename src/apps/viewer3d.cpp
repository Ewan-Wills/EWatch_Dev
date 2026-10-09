// 3D viewer implementation. Whole file gated on EWATCH_ENABLE_VIEWER3D so the
// painter's-algorithm renderer + the cube model data drop from the build when
// the feature is off.
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
#include <math.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "model.h"
#include "view.h"
#include "viewer3d.h"
#include "models/cow_obj.h"

// Compact min/max helpers (don't pull in <algorithm>).
template <typename T> static inline T mn3(T a, T b, T c) { T m = a < b ? a : b; return m < c ? m : c; }
template <typename T> static inline T mx3(T a, T b, T c) { T m = a > b ? a : b; return m > c ? m : c; }

// Display extent.
static const int16_t W = 240;
static const int16_t H = 280;

// Scene viewport: leave the top 50 px for title/back chrome.
static const int16_t SCENE_TOP    = 50;
static const int16_t SCENE_BOTTOM = 270;
static const int16_t CANVAS_W     = W;
static const int16_t CANVAS_H     = SCENE_BOTTOM - SCENE_TOP;   // 220 px

// Canvas-local center (canvas origin maps to screen (0, SCENE_TOP)).
static const int16_t CX = CANVAS_W / 2;
static const int16_t CY = CANVAS_H / 2;

// Camera distance + scale.
static const float   CAM_Z   = 20.0f;
static const float   SCALE   = 50.0f;

// Light direction in world space (must be normalised before use).
static const float   L_X = 0.4f, L_Y = -0.7f, L_Z = 0.5f;

// Off-screen framebuffer. Allocated lazily on first view-enter; we never free
// it (user re-enters the app frequently and a fresh allocation is wasteful).
// 240*220*2 = 105 KB — comfortably fits in either internal SRAM or PSRAM via
// heap_caps_malloc(MALLOC_CAP_8BIT).
static Arduino_Canvas *canvas = nullptr;
static bool            canvasOk = false;

// Per-pixel Z-buffer. uint16_t depth, smaller = closer. Allocated in PSRAM so
// the 105 KB doesn't stress internal SRAM. Without per-pixel depth, painter's
// sort can't resolve a 720-triangle mesh and you get wrong-shade triangles.
static uint16_t       *zbuf = nullptr;

// Back-button geometry (duplicated from system views to keep the user app
// self-contained — it doesn't reach into system internals).
static const int16_t BACK_W = 60, BACK_H = 42;
static void drawBackBtn() {
  if (!gfx) return;
  gfx->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  gfx->setTextColor(WHITE, DARKGREY);
  gfx->setTextSize(3);
  gfx->setCursor(18, 12);
  gfx->print('<');
}
static bool tappedBackHere(uint16_t x, uint16_t y) {
  return x < BACK_W + 12 && y < BACK_H + 10;
}

static inline uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void ensureCanvas() {
  if (canvas) return;
  canvas = new Arduino_Canvas(CANVAS_W, CANVAS_H, gfx, 0, SCENE_TOP);
  canvasOk = canvas->begin();
  if (!canvasOk) {
    Serial.println("viewer3d: canvas alloc failed; falling back to direct draw");
    delete canvas;
    canvas = nullptr;
  }
}

static void ensureZBuffer() {
  if (zbuf) return;
  size_t sz = CANVAS_W * CANVAS_H * sizeof(uint16_t);
  // Prefer PSRAM so the canvas (which heap_caps_malloc lands in internal SRAM)
  // doesn't have to share the same pool.
  zbuf = (uint16_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!zbuf) zbuf = (uint16_t *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!zbuf) Serial.println("viewer3d: zbuffer alloc failed");
}

// Map post-rotation z (range roughly [-sqrt(3), +sqrt(3)] for unit-half-extent
// model) to a uint16 depth. Smaller depth = closer to camera (nearer the
// viewer's z = -CAM_Z). The +2 offset keeps the mapping non-negative; the
// 16000 scale stretches the meaningful range across most of the uint16.
static inline uint16_t depthOf(float z) {
  int v = (int)((z + 2.0f) * 16000.0f);
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return (uint16_t)v;
}

// Per-pixel rasterizer with Z test. Walks the screen-space bounding box,
// computes barycentric weights via the edge-function method, interpolates
// depth, and writes color+depth where the new pixel is closer.
static void rasterTri(uint16_t *fb, uint16_t *zb,
                      int x0, int y0, float z0,
                      int x1, int y1, float z1,
                      int x2, int y2, float z2,
                      uint16_t color) {
  int minX = mn3(x0, x1, x2); if (minX < 0) minX = 0;
  int maxX = mx3(x0, x1, x2); if (maxX > CANVAS_W - 1) maxX = CANVAS_W - 1;
  int minY = mn3(y0, y1, y2); if (minY < 0) minY = 0;
  int maxY = mx3(y0, y1, y2); if (maxY > CANVAS_H - 1) maxY = CANVAS_H - 1;
  if (minX > maxX || minY > maxY) return;

  // Doubled signed area; sign tells us winding so we can pick the inside test.
  float area = (float)((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0));
  if (fabsf(area) < 0.5f) return;
  float invA = 1.0f / area;

  for (int y = minY; y <= maxY; y++) {
    uint16_t *fbRow = fb + y * CANVAS_W;
    uint16_t *zbRow = zb + y * CANVAS_W;
    for (int x = minX; x <= maxX; x++) {
      float w0 = ((float)(x2 - x1) * (y - y1) - (float)(y2 - y1) * (x - x1)) * invA;
      float w1 = ((float)(x0 - x2) * (y - y2) - (float)(y0 - y2) * (x - x2)) * invA;
      float w2 = 1.0f - w0 - w1;
      // Inside test (handles either winding via the area sign already absorbed
      // into invA — w's are positive when inside).
      if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
      float z = z0 * w0 + z1 * w1 + z2 * w2;
      uint16_t zd = depthOf(z);
      if (zd < zbRow[x]) {
        zbRow[x] = zd;
        fbRow[x] = color;
      }
    }
  }
}

void Viewer3DView::onEnter() {
  if (!gfx) return;

  // Chrome (title bar + back button) goes straight to the live display and is
  // never repainted; the canvas never touches this region.
  gfx->fillScreen(BLACK);
  drawBackBtn();
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(80, 14);
  gfx->print("3D View");
  gfx->drawFastHLine(20, 46, 200, DARKGREY);

  ensureCanvas();
  ensureZBuffer();
  if (canvasOk) {
    canvas->fillScreen(BLACK);
    canvas->flush();
  }

  firstDraw = true;
  lastTx = lastTy = -1;
  dragging = false;
}

void Viewer3DView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBackHere(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }
  if (e.type == EventType::Touch) {
    if (e.y >= SCENE_TOP) {
      lastTx = e.x; lastTy = e.y;
      dragging = true;
    }
    return;
  }
  if (e.type == EventType::TouchHold && dragging) {
    int dx = (int)e.x - lastTx;
    int dy = (int)e.y - lastTy;
    angleY += dx * 0.02f;
    angleX += dy * 0.02f;
    lastTx = e.x; lastTy = e.y;
    return;
  }
  if (e.type == EventType::TouchUp) {
    dragging = false;
    lastTx = lastTy = -1;
  }
}

void Viewer3DView::render() {
  if (!gfx) return;

  Model snap; { ModelLock lk; snap = model; }
  if (!dragging) {
    float ax = -snap.ax / 4096.0f;
    float ay =  snap.ay / 4096.0f;
    if (fabsf(ax) > 0.06f) angleY += ax * 0.04f;
    if (fabsf(ay) > 0.06f) angleX += ay * 0.04f;
  }

  drawScene();
}

void Viewer3DView::drawScene() {
  // Pick the draw target — canvas if available, otherwise direct (which still
  // works, just with the original flicker).
  Arduino_GFX *t = canvasOk ? (Arduino_GFX *)canvas : gfx;

  if (canvasOk) {
    t->fillScreen(BLACK);
  } else {
    gfx->fillRect(0, SCENE_TOP, W, CANVAS_H, BLACK);
  }

  // Pre-compute rotation trig.
  const float cx = cosf(angleX), sx = sinf(angleX);
  const float cy = cosf(angleY), sy = sinf(angleY);

  // Transform vertices and project to (canvas or screen) space. File-scope
  // statics so we don't blow the render task's 6 KB stack — 362 verts × five
  // arrays would be ~5.8 KB on the stack which is too close for comfort.
  static float   xv[cow_model::V_COUNT];
  static float   yv[cow_model::V_COUNT];
  static float   zv[cow_model::V_COUNT];
  static int16_t sxv[cow_model::V_COUNT];
  static int16_t syv[cow_model::V_COUNT];

  // Centre-Y depends on whether we're drawing canvas-local (origin top of
  // canvas) or screen-direct (origin top of display).
  const int16_t centerY = canvasOk ? CY : (SCENE_TOP + CANVAS_H / 2);

  for (int i = 0; i < cow_model::V_COUNT; i++) {
    float x = cow_model::V[i][0];
    float y = cow_model::V[i][1];
    float z = cow_model::V[i][2];
    float y1 = y * cx - z * sx;
    float z1 = y * sx + z * cx;
    float x2 =  x * cy + z1 * sy;
    float z2 = -x * sy + z1 * cy;
    xv[i] = x2; yv[i] = y1; zv[i] = z2;
    float zCam = CAM_Z + z2;
    if (zCam < 0.5f) zCam = 0.5f;
    sxv[i] = CX + (int16_t)(x2 * SCALE * CAM_Z / zCam);
    syv[i] = centerY - (int16_t)(y1 * SCALE * CAM_Z / zCam);
  }

  // Z-buffer path: clear depth, walk faces in any order, custom rasterize.
  // Painter's sort isn't used — per-pixel depth handles overlap exactly.
  uint16_t *fb = (canvasOk && zbuf) ? canvas->getFramebuffer() : nullptr;
  if (fb && zbuf) {
    memset(zbuf, 0xFF, CANVAS_W * CANVAS_H * sizeof(uint16_t));
  }

  const float ll = sqrtf(L_X * L_X + L_Y * L_Y + L_Z * L_Z);
  const float lx = L_X / ll, ly = L_Y / ll, lz = L_Z / ll;

  for (int f = 0; f < cow_model::F_COUNT; f++) {
    int a = cow_model::F[f][0];
    int b = cow_model::F[f][1];
    int c = cow_model::F[f][2];

    float ux = xv[b] - xv[a], uy = yv[b] - yv[a], uz = zv[b] - zv[a];
    float vx = xv[c] - xv[a], vy = yv[c] - yv[a], vz = zv[c] - zv[a];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nlen < 1e-6f) continue;
    nx /= nlen; ny /= nlen; nz /= nlen;

    if (nz > -0.05f) continue;     // back-facing or near-edge-on

    float dot = -(nx * lx + ny * ly + nz * lz);
    if (dot < 0) dot = 0;
    float intensity = 0.18f + 0.82f * dot;
    if (intensity > 1.f) intensity = 1.f;

    uint8_t r  = (uint8_t)(cow_model::BASE_R * intensity);
    uint8_t g  = (uint8_t)(cow_model::BASE_G * intensity);
    uint8_t bl = (uint8_t)(cow_model::BASE_B * intensity);
    uint16_t color = rgb888to565(r, g, bl);

    if (fb && zbuf) {
      rasterTri(fb, zbuf,
                sxv[a], syv[a], zv[a],
                sxv[b], syv[b], zv[b],
                sxv[c], syv[c], zv[c],
                color);
    } else {
      // Fallback (no canvas/zbuffer): paint via GFX's solid triangle.
      t->fillTriangle(sxv[a], syv[a], sxv[b], syv[b], sxv[c], syv[c], color);
    }
  }

  // Push the finished frame in one shot.
  if (canvasOk) canvas->flush();
}

#endif  // EWATCH_ENABLE_VIEWER3D
