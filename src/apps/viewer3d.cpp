// 3D viewer implementation. Whole file gated on EWATCH_ENABLE_VIEWER3D so the
// renderer + model data drop from the build when the feature is off.
//
// Renderer: transform vertices, then rasterize triangles into an off-screen
// canvas with a per-pixel z-buffer (no painter's sort needed). Gouraud mode
// interpolates per-vertex light intensity across each triangle through a
// 32-entry shade LUT; wireframe just strokes the edges.
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "viewer3d.h"
#include "models/cow_obj.h"
#include "models/cube_obj.h"

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

// Canvas-local center.
static const int16_t CX = CANVAS_W / 2;
static const int16_t CY = CANVAS_H / 2;

// Camera distance + scale.
static const float CAM_Z = 20.0f;
static const float SCALE = 50.0f;

// Light direction in world space (normalised before use).
static const float L_X = 0.4f, L_Y = -0.7f, L_Z = 0.5f;

// Model-switch arrows (screen coords, bottom corners of the scene).
static const int16_t ARROW_W = 44, ARROW_H = 44;
static const int16_t ARROW_Y = SCENE_BOTTOM - ARROW_H - 2;

// ---------------------------------------------------------------------------
// Mesh registry. Static models point straight into flash; procedural models
// generate into shared PSRAM buffers on first selection.
// ---------------------------------------------------------------------------
struct Mesh {
  const char     *name;
  const float   (*V)[3];  int vCount;
  const uint16_t(*F)[3];  int fCount;
  uint8_t r, g, b;        // base colour
  bool    cull;           // true when winding is trusted (skip back faces)
};

// Generation capacity. The torus knot is the largest: 72 rings × 8 sides.
static const int kMaxGenV = 800;
static const int kMaxGenF = 1600;

// PSRAM pools (allocated lazily, never freed — the app is re-entered often).
static float    (*genV)[3]  = nullptr;   // generated vertices
static uint16_t (*genF)[3]  = nullptr;   // generated faces
static float    (*vertN)[3] = nullptr;   // per-vertex normals (Gouraud)
static float    (*txX)      = nullptr;   // transformed vertex x/y/z
static float    (*txY)      = nullptr;
static float    (*txZ)      = nullptr;
static int16_t  (*scrX)     = nullptr;   // projected screen coords
static int16_t  (*scrY)     = nullptr;
static float    (*vertI)    = nullptr;   // per-vertex light intensity
static uint16_t (*cubeF16)[3] = nullptr; // cube faces widened to uint16

static void *pAlloc(size_t sz) {
  void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  return p;
}

static bool ensurePools() {
  if (txX) return true;
  genV   = (float(*)[3])   pAlloc(sizeof(float)    * 3 * kMaxGenV);
  genF   = (uint16_t(*)[3])pAlloc(sizeof(uint16_t) * 3 * kMaxGenF);
  vertN  = (float(*)[3])   pAlloc(sizeof(float)    * 3 * kMaxGenV);
  txX    = (float *)       pAlloc(sizeof(float)    * kMaxGenV);
  txY    = (float *)       pAlloc(sizeof(float)    * kMaxGenV);
  txZ    = (float *)       pAlloc(sizeof(float)    * kMaxGenV);
  scrX   = (int16_t *)     pAlloc(sizeof(int16_t)  * kMaxGenV);
  scrY   = (int16_t *)     pAlloc(sizeof(int16_t)  * kMaxGenV);
  vertI  = (float *)       pAlloc(sizeof(float)    * kMaxGenV);
  cubeF16= (uint16_t(*)[3])pAlloc(sizeof(uint16_t) * 3 * cube_model::F_COUNT);
  if (!genV || !genF || !vertN || !txX || !txY || !txZ ||
      !scrX || !scrY || !vertI || !cubeF16) {
    Serial.println("viewer3d: pool alloc failed");
    return false;
  }
  for (int f = 0; f < cube_model::F_COUNT; f++)
    for (int k = 0; k < 3; k++) cubeF16[f][k] = cube_model::F[f][k];
  return true;
}

// ---- procedural generators (fill genV/genF, return counts) ----

// (p,q) torus knot with a circular tube swept along it.
static void genTorusKnot(int &nv, int &nf) {
  const int   RINGS = 72, SIDES = 8;
  const int   P = 2, Q = 3;
  const float TUBE = 0.30f;
  auto curve = [&](float t, float &x, float &y, float &z) {
    float r = cosf(Q * t) + 2.0f;
    x = r * cosf(P * t) / 3.2f;
    y = r * sinf(P * t) / 3.2f;
    z = -sinf(Q * t) / 3.2f * 1.6f;
  };
  nv = 0;
  for (int i = 0; i < RINGS; i++) {
    float t  = (float)i / RINGS * 6.2831853f;
    float t2 = t + 0.01f;
    float px, py, pz, qx, qy, qz;
    curve(t, px, py, pz);
    curve(t2, qx, qy, qz);
    // Frenet-ish frame: tangent + a stable normal/binormal pair.
    float tx = qx - px, ty = qy - py, tz = qz - pz;
    float tl = sqrtf(tx * tx + ty * ty + tz * tz);
    tx /= tl; ty /= tl; tz /= tl;
    // n = t × up, b = t × n (up chosen to avoid degeneracy).
    float ux = 0, uy = 0, uz = 1;
    if (fabsf(tz) > 0.9f) { ux = 1; uz = 0; }
    float nx = ty * uz - tz * uy, ny = tz * ux - tx * uz, nz = tx * uy - ty * ux;
    float nl = sqrtf(nx * nx + ny * ny + nz * nz);
    nx /= nl; ny /= nl; nz /= nl;
    float bx = ty * nz - tz * ny, by = tz * nx - tx * nz, bz = tx * ny - ty * nx;
    for (int j = 0; j < SIDES; j++) {
      float a = (float)j / SIDES * 6.2831853f;
      float ca = cosf(a), sa = sinf(a);
      genV[nv][0] = px + TUBE * (ca * nx + sa * bx);
      genV[nv][1] = py + TUBE * (ca * ny + sa * by);
      genV[nv][2] = pz + TUBE * (ca * nz + sa * bz);
      nv++;
    }
  }
  nf = 0;
  for (int i = 0; i < RINGS; i++) {
    int i2 = (i + 1) % RINGS;
    for (int j = 0; j < SIDES; j++) {
      int j2 = (j + 1) % SIDES;
      uint16_t a = i * SIDES + j,  b = i * SIDES + j2;
      uint16_t c = i2 * SIDES + j, d = i2 * SIDES + j2;
      genF[nf][0] = a; genF[nf][1] = b; genF[nf][2] = c; nf++;
      genF[nf][0] = b; genF[nf][1] = d; genF[nf][2] = c; nf++;
    }
  }
}

// UV sphere — simpler than an icosphere and good enough at this resolution.
static void genPlanet(int &nv, int &nf) {
  const int LAT = 12, LON = 18;
  nv = 0;
  for (int i = 0; i <= LAT; i++) {
    float phi = (float)i / LAT * 3.14159265f;     // 0..pi
    for (int j = 0; j < LON; j++) {
      float th = (float)j / LON * 6.2831853f;
      genV[nv][0] = sinf(phi) * cosf(th);
      genV[nv][1] = cosf(phi);
      genV[nv][2] = sinf(phi) * sinf(th);
      nv++;
    }
  }
  nf = 0;
  for (int i = 0; i < LAT; i++) {
    for (int j = 0; j < LON; j++) {
      int j2 = (j + 1) % LON;
      uint16_t a = i * LON + j,       b = i * LON + j2;
      uint16_t c = (i + 1) * LON + j, d = (i + 1) * LON + j2;
      if (i > 0)       { genF[nf][0] = a; genF[nf][1] = b; genF[nf][2] = c; nf++; }
      if (i < LAT - 1) { genF[nf][0] = b; genF[nf][1] = d; genF[nf][2] = c; nf++; }
    }
  }
}

// Faceted gem: crown ring + girdle ring + two apexes.
static void genGem(int &nv, int &nf) {
  const int SEG = 8;
  nv = 0;
  genV[nv][0] = 0; genV[nv][1] = 0.95f; genV[nv][2] = 0; nv++;   // 0: top apex
  genV[nv][0] = 0; genV[nv][1] = -0.95f; genV[nv][2] = 0; nv++;  // 1: bottom apex
  for (int j = 0; j < SEG; j++) {                                 // 2..9: crown
    float a = ((float)j + 0.5f) / SEG * 6.2831853f;
    genV[nv][0] = cosf(a) * 0.52f; genV[nv][1] = 0.45f; genV[nv][2] = sinf(a) * 0.52f; nv++;
  }
  for (int j = 0; j < SEG; j++) {                                 // 10..17: girdle
    float a = (float)j / SEG * 6.2831853f;
    genV[nv][0] = cosf(a) * 0.85f; genV[nv][1] = 0.05f; genV[nv][2] = sinf(a) * 0.85f; nv++;
  }
  nf = 0;
  for (int j = 0; j < SEG; j++) {
    int j2 = (j + 1) % SEG;
    uint16_t cr = 2 + j, cr2 = 2 + j2;
    uint16_t gi = 2 + SEG + j, gi2 = 2 + SEG + j2;
    genF[nf][0] = 0;   genF[nf][1] = cr2; genF[nf][2] = cr;  nf++;  // table
    genF[nf][0] = cr;  genF[nf][1] = cr2; genF[nf][2] = gi2; nf++;  // crown band
    genF[nf][0] = cr;  genF[nf][1] = gi2; genF[nf][2] = gi;  nf++;
    genF[nf][0] = 1;   genF[nf][1] = gi;  genF[nf][2] = gi2; nf++;  // pavilion
  }
}

// Model table. Procedural entries regenerate on selection (cheap, <5 ms).
enum { MODEL_COW = 0, MODEL_CUBE, MODEL_KNOT, MODEL_PLANET, MODEL_GEM,
       MODEL_COUNT };
static const char *kModelNames[MODEL_COUNT] =
  { "Cow", "Cube", "Knot", "Planet", "Gem" };

static Mesh mesh;                 // the currently-selected model
static int  meshLoaded = -1;      // which model the pools currently hold

static void computeVertexNormals() {
  // Per-vertex normal = normalised sum of adjacent face normals. Faces with
  // untrusted winding still average out fine because we flip at shade time.
  for (int i = 0; i < mesh.vCount; i++) {
    vertN[i][0] = vertN[i][1] = vertN[i][2] = 0;
  }
  for (int f = 0; f < mesh.fCount; f++) {
    int a = mesh.F[f][0], b = mesh.F[f][1], c = mesh.F[f][2];
    float ux = mesh.V[b][0] - mesh.V[a][0];
    float uy = mesh.V[b][1] - mesh.V[a][1];
    float uz = mesh.V[b][2] - mesh.V[a][2];
    float vx = mesh.V[c][0] - mesh.V[a][0];
    float vy = mesh.V[c][1] - mesh.V[a][1];
    float vz = mesh.V[c][2] - mesh.V[a][2];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    for (int k = 0; k < 3; k++) {
      int vi = mesh.F[f][k];
      vertN[vi][0] += nx; vertN[vi][1] += ny; vertN[vi][2] += nz;
    }
  }
  for (int i = 0; i < mesh.vCount; i++) {
    float l = sqrtf(vertN[i][0] * vertN[i][0] +
                    vertN[i][1] * vertN[i][1] +
                    vertN[i][2] * vertN[i][2]);
    if (l > 1e-6f) { vertN[i][0] /= l; vertN[i][1] /= l; vertN[i][2] /= l; }
  }
}

static void loadModel(int idx) {
  if (idx == meshLoaded) return;
  int nv = 0, nf = 0;
  switch (idx) {
    case MODEL_COW:
      mesh = { kModelNames[idx], cow_model::V, (int)cow_model::V_COUNT,
               cow_model::F, (int)cow_model::F_COUNT,
               cow_model::BASE_R, cow_model::BASE_G, cow_model::BASE_B, true };
      break;
    case MODEL_CUBE:
      mesh = { kModelNames[idx], cube_model::V, (int)cube_model::V_COUNT,
               cubeF16, (int)cube_model::F_COUNT,
               90, 170, 255, true };
      break;
    case MODEL_KNOT:
      genTorusKnot(nv, nf);
      mesh = { kModelNames[idx], genV, nv, genF, nf, 255, 120, 60, false };
      break;
    case MODEL_PLANET:
      genPlanet(nv, nf);
      mesh = { kModelNames[idx], genV, nv, genF, nf, 80, 200, 120, false };
      break;
    default:
      genGem(nv, nf);
      mesh = { kModelNames[idx], genV, nv, genF, nf, 120, 220, 255, false };
      break;
  }
  computeVertexNormals();
  meshLoaded = idx;
}

// ---------------------------------------------------------------------------
// Canvas + z-buffer (same lazy-allocation strategy as before).
// ---------------------------------------------------------------------------
static Arduino_Canvas *canvas = nullptr;
static bool            canvasOk = false;
static uint16_t       *zbuf = nullptr;

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
  zbuf = (uint16_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!zbuf) zbuf = (uint16_t *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!zbuf) Serial.println("viewer3d: zbuffer alloc failed");
}

static inline uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Map post-rotation z to a uint16 depth; smaller = closer to camera.
static inline uint16_t depthOf(float z) {
  int v = (int)((z + 2.0f) * 16000.0f);
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return (uint16_t)v;
}

// 32-step shade LUT for the current base colour: index 0 = ambient floor,
// 31 = fully lit. Rebuilt on model switch.
static uint16_t shadeLut[32];
static void buildShadeLut() {
  for (int k = 0; k < 32; k++) {
    float it = 0.18f + 0.82f * ((float)k / 31.0f);
    shadeLut[k] = rgb888to565((uint8_t)(mesh.r * it),
                              (uint8_t)(mesh.g * it),
                              (uint8_t)(mesh.b * it));
  }
}

// Flat-shaded triangle with Z test (single colour).
static void rasterTriFlat(uint16_t *fb, uint16_t *zb,
                          int x0, int y0, float z0,
                          int x1, int y1, float z1,
                          int x2, int y2, float z2,
                          uint16_t color) {
  int minX = mn3(x0, x1, x2); if (minX < 0) minX = 0;
  int maxX = mx3(x0, x1, x2); if (maxX > CANVAS_W - 1) maxX = CANVAS_W - 1;
  int minY = mn3(y0, y1, y2); if (minY < 0) minY = 0;
  int maxY = mx3(y0, y1, y2); if (maxY > CANVAS_H - 1) maxY = CANVAS_H - 1;
  if (minX > maxX || minY > maxY) return;

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

// Gouraud triangle: interpolates per-vertex intensity (0..1) through the
// shade LUT.
static void rasterTriGouraud(uint16_t *fb, uint16_t *zb,
                             int x0, int y0, float z0, float i0,
                             int x1, int y1, float z1, float i1,
                             int x2, int y2, float z2, float i2) {
  int minX = mn3(x0, x1, x2); if (minX < 0) minX = 0;
  int maxX = mx3(x0, x1, x2); if (maxX > CANVAS_W - 1) maxX = CANVAS_W - 1;
  int minY = mn3(y0, y1, y2); if (minY < 0) minY = 0;
  int maxY = mx3(y0, y1, y2); if (maxY > CANVAS_H - 1) maxY = CANVAS_H - 1;
  if (minX > maxX || minY > maxY) return;

  float area = (float)((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0));
  if (fabsf(area) < 0.5f) return;
  float invA = 1.0f / area;

  // Pre-scale intensities into LUT space.
  float s0 = i0 * 31.f, s1 = i1 * 31.f, s2 = i2 * 31.f;

  for (int y = minY; y <= maxY; y++) {
    uint16_t *fbRow = fb + y * CANVAS_W;
    uint16_t *zbRow = zb + y * CANVAS_W;
    for (int x = minX; x <= maxX; x++) {
      float w0 = ((float)(x2 - x1) * (y - y1) - (float)(y2 - y1) * (x - x1)) * invA;
      float w1 = ((float)(x0 - x2) * (y - y2) - (float)(y0 - y2) * (x - x2)) * invA;
      float w2 = 1.0f - w0 - w1;
      if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
      float z = z0 * w0 + z1 * w1 + z2 * w2;
      uint16_t zd = depthOf(z);
      if (zd < zbRow[x]) {
        zbRow[x] = zd;
        int s = (int)(s0 * w0 + s1 * w1 + s2 * w2);
        if (s < 0) s = 0; else if (s > 31) s = 31;
        fbRow[x] = shadeLut[s];
      }
    }
  }
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
void Viewer3DView::onEnter() {
  if (!gfx) return;
  ensureCanvas();
  ensureZBuffer();
  if (!ensurePools()) return;
  loadModel(modelIdx);
  buildShadeLut();

  drawChrome();
  if (canvasOk) {
    canvas->fillScreen(BLACK);
    canvas->flush();
  }

  lastTx = lastTy = -1;
  dragging = false;
  dragMoved = false;
}

// Chrome (title bar + back button + labels) goes straight to the live
// display; the canvas never touches this region.
void Viewer3DView::drawChrome() {
  gfx->fillRect(0, 0, W, SCENE_TOP, BLACK);
  drawBackButton();
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  char title[24];
  snprintf(title, sizeof(title), "%s", mesh.name);
  gfx->setCursor(80, 8);
  gfx->print(title);
  static const char *kShadeNames[3] = { "flat", "smooth", "wire" };
  gfx->setTextSize(1);
  gfx->setTextColor(DARKGREY, BLACK);
  gfx->setCursor(80, 30);
  gfx->printf("%s  (tap = shade)", kShadeNames[shading]);
  gfx->drawFastHLine(20, 46, 200, DARKGREY);
}

void Viewer3DView::switchModel(int dir) {
  modelIdx = (modelIdx + MODEL_COUNT + dir) % MODEL_COUNT;
  loadModel(modelIdx);
  buildShadeLut();
  hapticBuzz(50, 50);
  drawChrome();
}

void Viewer3DView::cycleShading() {
  shading = (uint8_t)((shading + 1) % 3);
  hapticBuzz(40, 40);
  drawChrome();
}

void Viewer3DView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    switchTo(Screen::AppList); return;
  }
  if (e.type == EventType::Touch) {
    if (e.y >= SCENE_TOP) {
      // Model-switch arrows first.
      if (uiInRect(e.x, e.y, 0, ARROW_Y, ARROW_W, ARROW_H))      { switchModel(-1); return; }
      if (uiInRect(e.x, e.y, W - ARROW_W, ARROW_Y, ARROW_W, ARROW_H)) { switchModel(+1); return; }
      lastTx = e.x; lastTy = e.y;
      dragging = true;
      dragMoved = false;
      pressMs = millis();
    }
    return;
  }
  if (e.type == EventType::TouchHold && dragging) {
    int dx = (int)e.x - lastTx;
    int dy = (int)e.y - lastTy;
    if (dx * dx + dy * dy > 0) {
      angleY += dx * 0.02f;
      angleX += dy * 0.02f;
    }
    if (abs((int)e.x - lastTx) + abs((int)e.y - lastTy) > 3) dragMoved = true;
    lastTx = e.x; lastTy = e.y;
    return;
  }
  if (e.type == EventType::TouchUp) {
    // A quick tap with no drag cycles the shading mode.
    if (dragging && !dragMoved && millis() - pressMs < 300) cycleShading();
    dragging = false;
    lastTx = lastTy = -1;
  }
}

void Viewer3DView::render() {
  if (!gfx) return;

  if (!dragging) {
    int16_t axr, ayr;
    { ModelLock lk; axr = model.ax; ayr = model.ay; }
    float ax = -axr / 4096.0f;
    float ay =  ayr / 4096.0f;
    if (fabsf(ax) > 0.06f) angleY += ax * 0.04f;
    if (fabsf(ay) > 0.06f) angleX += ay * 0.04f;
  }

  drawScene();
}

void Viewer3DView::drawScene() {
  if (!txX) return;                 // pools failed — nothing we can do
  Arduino_GFX *t = canvasOk ? (Arduino_GFX *)canvas : gfx;

  if (canvasOk) {
    t->fillScreen(BLACK);
  } else {
    gfx->fillRect(0, SCENE_TOP, W, CANVAS_H, BLACK);
  }

  // Pre-compute rotation trig.
  const float cx = cosf(angleX), sx = sinf(angleX);
  const float cy = cosf(angleY), sy = sinf(angleY);

  const float ll = sqrtf(L_X * L_X + L_Y * L_Y + L_Z * L_Z);
  const float lx = L_X / ll, ly = L_Y / ll, lz = L_Z / ll;

  // Centre-Y depends on whether we're drawing canvas-local or screen-direct.
  const int16_t centerY = canvasOk ? CY : (SCENE_TOP + CANVAS_H / 2);
  const int16_t centerX = CX;

  const bool wantGouraud = (shading == 1) && canvasOk && zbuf;

  for (int i = 0; i < mesh.vCount; i++) {
    float x = mesh.V[i][0];
    float y = mesh.V[i][1];
    float z = mesh.V[i][2];
    float y1 = y * cx - z * sx;
    float z1 = y * sx + z * cx;
    float x2 =  x * cy + z1 * sy;
    float z2 = -x * sy + z1 * cy;
    txX[i] = x2; txY[i] = y1; txZ[i] = z2;
    float zCam = CAM_Z + z2;
    if (zCam < 0.5f) zCam = 0.5f;
    scrX[i] = centerX + (int16_t)(x2 * SCALE * CAM_Z / zCam);
    scrY[i] = centerY - (int16_t)(y1 * SCALE * CAM_Z / zCam);

    if (wantGouraud) {
      // Rotate the vertex normal by the same matrix, then two-sided diffuse:
      // flip the normal when it faces away so both winding families light up.
      float nx0 = vertN[i][0], ny0 = vertN[i][1], nz0 = vertN[i][2];
      float ny1 = ny0 * cx - nz0 * sx;
      float nz1 = ny0 * sx + nz0 * cx;
      float nx2 =  nx0 * cy + nz1 * sy;
      float nz2 = -nx0 * sy + nz1 * cy;
      if (nz2 > 0) { nx2 = -nx2; ny1 = -ny1; nz2 = -nz2; }
      float dot = -(nx2 * lx + ny1 * ly + nz2 * lz);
      if (dot < 0) dot = 0;
      vertI[i] = dot;
    }
  }

  uint16_t *fb = (canvasOk && zbuf) ? canvas->getFramebuffer() : nullptr;
  if (fb && zbuf && shading != 2) {
    memset(zbuf, 0xFF, CANVAS_W * CANVAS_H * sizeof(uint16_t));
  }

  const uint16_t baseColor = rgb888to565(mesh.r, mesh.g, mesh.b);

  for (int f = 0; f < mesh.fCount; f++) {
    int a = mesh.F[f][0];
    int b = mesh.F[f][1];
    int c = mesh.F[f][2];

    // Face normal (screen space z decides culling / flat shade).
    float ux = txX[b] - txX[a], uy = txY[b] - txY[a], uz = txZ[b] - txZ[a];
    float vx = txX[c] - txX[a], vy = txY[c] - txY[a], vz = txZ[c] - txZ[a];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nlen < 1e-6f) continue;
    nx /= nlen; ny /= nlen; nz /= nlen;

    if (mesh.cull && nz > -0.05f) continue;    // back-facing (trusted winding)

    if (shading == 2) {
      // Wireframe: stroke the edges; skip back faces on trusted meshes so
      // the interior doesn't turn to noise.
      t->drawLine(scrX[a], scrY[a], scrX[b], scrY[b], baseColor);
      t->drawLine(scrX[b], scrY[b], scrX[c], scrY[c], baseColor);
      t->drawLine(scrX[c], scrY[c], scrX[a], scrY[a], baseColor);
      continue;
    }

    if (wantGouraud && fb) {
      rasterTriGouraud(fb, zbuf,
                       scrX[a], scrY[a], txZ[a], vertI[a],
                       scrX[b], scrY[b], txZ[b], vertI[b],
                       scrX[c], scrY[c], txZ[c], vertI[c]);
      continue;
    }

    // Flat: two-sided diffuse from the face normal.
    float fnx = nx, fny = ny, fnz = nz;
    if (fnz > 0) { fnx = -fnx; fny = -fny; fnz = -fnz; }
    float dot = -(fnx * lx + fny * ly + fnz * lz);
    if (dot < 0) dot = 0;
    float intensity = 0.18f + 0.82f * dot;
    if (intensity > 1.f) intensity = 1.f;

    uint16_t color = rgb888to565((uint8_t)(mesh.r * intensity),
                                 (uint8_t)(mesh.g * intensity),
                                 (uint8_t)(mesh.b * intensity));

    if (fb && zbuf) {
      rasterTriFlat(fb, zbuf,
                    scrX[a], scrY[a], txZ[a],
                    scrX[b], scrY[b], txZ[b],
                    scrX[c], scrY[c], txZ[c],
                    color);
    } else {
      t->fillTriangle(scrX[a], scrY[a], scrX[b], scrY[b],
                      scrX[c], scrY[c], color);
    }
  }

  // Model-switch arrows, drawn into the canvas so they ride the frame.
  if (canvasOk) {
    int16_t ay = ARROW_Y - SCENE_TOP + ARROW_H / 2;
    t->fillTriangle(6, ay, 26, ay - 12, 26, ay + 12, DARKGREY);
    t->fillTriangle(W - 6, ay, W - 26, ay - 12, W - 26, ay + 12, DARKGREY);
  }

  if (canvasOk) canvas->flush();
}

#endif  // EWATCH_ENABLE_VIEWER3D
