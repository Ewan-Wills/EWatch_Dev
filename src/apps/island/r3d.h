// r3d — a tiny flat-shaded low-poly 3D renderer for the cozy-island watch face.
//
// Design constraints (why it looks the way it does):
//   * We render into the shared 240x280 PSRAM Arduino_Canvas (display.h →
//     frameCanvas()) and blit once per frame. We do NOT write our own scanline
//     rasterizer — Arduino_GFX::fillTriangle already gives us clipped, integer
//     triangle fills, so a "renderer" here is just: project verts → sort faces
//     back-to-front (painter's algorithm) → flat-shade → fillTriangle.
//   * Geometry is built ONCE into a Scene (world-space verts + faces + per-face
//     world normals). Each frame we only re-project verts with the current
//     camera and re-shade with the current light — cheap. The island never
//     deforms, so nothing here re-touches vertex data per frame.
//   * Everything is float math on the FPU; counts are tiny (a few hundred
//     faces) so this comfortably hits an interactive frame rate.
#pragma once
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdint.h>

// ----------------------------------------------------------------------------
// Vector math (free functions — no operator overloading so the intent is plain
// at every call site, and there's no ambiguity to trip the compiler on).
// ----------------------------------------------------------------------------
struct Vec3 { float x, y, z; };

static inline Vec3 vec3(float x, float y, float z) { return Vec3{ x, y, z }; }
static inline Vec3 vadd(Vec3 a, Vec3 b)  { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline Vec3 vsub(Vec3 a, Vec3 b)  { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline Vec3 vscale(Vec3 a, float s) { return Vec3{ a.x * s, a.y * s, a.z * s }; }
static inline float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vcross(Vec3 a, Vec3 b) {
  return Vec3{ a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x };
}
static inline float vlen(Vec3 a) { return sqrtf(vdot(a, a)); }
static inline Vec3 vnorm(Vec3 a) {
  float l = vlen(a);
  if (l < 1e-6f) return Vec3{ 0, 1, 0 };
  return vscale(a, 1.0f / l);
}

static inline uint16_t rgb565(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ----------------------------------------------------------------------------
// Camera + lighting passed to renderScene each frame.
// ----------------------------------------------------------------------------
struct Camera {
  Vec3  eye;        // world position of the lens
  Vec3  target;     // point it looks at
  Vec3  up;         // world up (usually 0,1,0)
  float focal;      // pixels — bigger = narrower FOV / more zoom
};

// Flat shading: final_channel = base_channel * (ambient + sun * max(0, n·L)).
// ambient is the cool sky bounce; sun is the warm directional key. Both are
// 0..~1.3 per channel (we allow slight over-1 so midday can read bright).
struct Lighting {
  Vec3  sunDir;                 // unit vector pointing TO the light, world space
  float ambR, ambG, ambB;       // ambient (skylight) multipliers
  float sunR, sunG, sunB;       // directional sun multipliers
};

// ----------------------------------------------------------------------------
// Scene — the world. Built once via the primitive helpers, then finalize() to
// bake per-face normals. Fixed capacity (no heap churn); sized generously for
// the island (~300 faces today, head-room to grow).
// ----------------------------------------------------------------------------
class Scene {
public:
  // Primitives don't share vertices (simpler to author), so the island needs
  // ~1k verts. Sized with head-room; ~38 KiB of static .bss for the Scene.
  static const int MAXV = 1400;
  static const int MAXF = 1100;

  int   nv = 0, nf = 0;
  Vec3  vert[MAXV];

  struct Face {
    uint16_t a, b, c;     // vertex indices
    uint8_t  cr, cg, cb;  // base albedo (RGB888)
    uint8_t  flags;       // bit0: double-sided (skip back-face cull)
  };
  Face  face[MAXF];
  Vec3  nrm[MAXF];        // outward world normal, baked by finalize()

  static const uint8_t DOUBLE_SIDED = 0x01;

  void clear() { nv = 0; nf = 0; }

  int addV(Vec3 p) {
    if (nv >= MAXV) return nv - 1;
    vert[nv] = p;
    return nv++;
  }
  void addF(int a, int b, int c, uint8_t r, uint8_t g, uint8_t bl, uint8_t fl = 0) {
    if (nf >= MAXF) return;
    face[nf] = { (uint16_t)a, (uint16_t)b, (uint16_t)c, r, g, bl, fl };
    nf++;
  }

  // Bake outward normals from winding. Call once after all geometry is added.
  void finalize();

  // ---- primitive builders (append into this scene) -----------------------
  // Quad as two tris; author p0..p3 counter-clockwise as seen from the front
  // so the baked normal faces the viewer.
  void quad(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
            uint8_t r, uint8_t g, uint8_t b, uint8_t fl = 0);

  // Axis-aligned box centered at c with half-extents h.
  void box(Vec3 c, Vec3 h, uint8_t r, uint8_t g, uint8_t b);

  // Vertical frustum (truncated cone): a ring from radius rb at y=y0 to radius
  // rt at y=y0+height, `sides` facets, plus an optional flat top cap.
  void frustum(float cx, float cz, float y0, float rb, float rt, float height,
               int sides, uint8_t sr, uint8_t sg, uint8_t sb,
               bool capTop, uint8_t tr, uint8_t tg, uint8_t tb);

  // Cone: apex above a base ring. Used for tree canopies and tower roofs.
  void cone(float cx, float cz, float y0, float radius, float height,
            int sides, uint8_t r, uint8_t g, uint8_t b);

  // Flat horizontal disc (fan) facing up at height y. Pond, grass top, etc.
  void disc(float cx, float cz, float y, float radius, int sides,
            uint8_t r, uint8_t g, uint8_t b, uint8_t fl = 0);
};

// Draw the whole scene into `cv` (does NOT clear it — caller paints the sky
// first). When `cull` is true, screen-space back-facing tris are skipped to
// cut overdraw; double-sided faces are always drawn.
void renderScene(Arduino_Canvas *cv, const Scene &s,
                 const Camera &cam, const Lighting &L, bool cull);

// Project a single world point to screen space using the same camera math as
// renderScene. Returns false if behind the camera. Used to anchor 2D overlays
// (clock-tower hands, prompts) onto 3D landmarks. `depth` is positive view-z.
bool projectPoint(const Camera &cam, Arduino_Canvas *cv, Vec3 p,
                  float &sx, float &sy, float &depth);
