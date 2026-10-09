#include "r3d.h"
#include <stdlib.h>   // qsort

// ----------------------------------------------------------------------------
// Per-frame scratch — file-scope so it never lands on the 6 KiB render-task
// stack. Indexed in lockstep with Scene::vert / Scene::face.
// ----------------------------------------------------------------------------
static float px[Scene::MAXV];   // projected screen x
static float py[Scene::MAXV];   // projected screen y
static float pz[Scene::MAXV];   // positive view-space depth (bigger = farther)
static bool  vis[Scene::MAXV];  // vertex is in front of the near plane

struct DrawItem { float depth; uint16_t face; };
static DrawItem items[Scene::MAXF];

static int cmpFarFirst(const void *a, const void *b) {
  // Sort descending by depth so far faces paint first (painter's algorithm).
  float da = ((const DrawItem *)a)->depth;
  float db = ((const DrawItem *)b)->depth;
  if (da < db) return 1;
  if (da > db) return -1;
  return 0;
}

// ----------------------------------------------------------------------------
// Scene::finalize — bake one outward normal per face from its winding.
// ----------------------------------------------------------------------------
void Scene::finalize() {
  for (int f = 0; f < nf; f++) {
    Vec3 a = vert[face[f].a];
    Vec3 b = vert[face[f].b];
    Vec3 c = vert[face[f].c];
    nrm[f] = vnorm(vcross(vsub(b, a), vsub(c, a)));
  }
}

// ----------------------------------------------------------------------------
// Primitive builders.
// ----------------------------------------------------------------------------
void Scene::quad(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t fl) {
  int i0 = addV(p0), i1 = addV(p1), i2 = addV(p2), i3 = addV(p3);
  addF(i0, i1, i2, r, g, b, fl);
  addF(i0, i2, i3, r, g, b, fl);
}

void Scene::box(Vec3 c, Vec3 h, uint8_t r, uint8_t g, uint8_t b) {
  // 8 corners
  Vec3 p000 = vec3(c.x - h.x, c.y - h.y, c.z - h.z);
  Vec3 p001 = vec3(c.x - h.x, c.y - h.y, c.z + h.z);
  Vec3 p010 = vec3(c.x - h.x, c.y + h.y, c.z - h.z);
  Vec3 p011 = vec3(c.x - h.x, c.y + h.y, c.z + h.z);
  Vec3 p100 = vec3(c.x + h.x, c.y - h.y, c.z - h.z);
  Vec3 p101 = vec3(c.x + h.x, c.y - h.y, c.z + h.z);
  Vec3 p110 = vec3(c.x + h.x, c.y + h.y, c.z - h.z);
  Vec3 p111 = vec3(c.x + h.x, c.y + h.y, c.z + h.z);
  // Slight per-face shading variety so flat boxes read as 3D even before the
  // light hits them: keep base colour, the lighting pass does the rest.
  quad(p001, p101, p111, p011, r, g, b);            // +z (front)
  quad(p100, p000, p010, p110, r, g, b);            // -z (back)
  quad(p101, p100, p110, p111, r, g, b);            // +x (right)
  quad(p000, p001, p011, p010, r, g, b);            // -x (left)
  quad(p011, p111, p110, p010, r, g, b);            // +y (top)
  quad(p000, p100, p101, p001, r, g, b);            // -y (bottom)
}

void Scene::frustum(float cx, float cz, float y0, float rb, float rt,
                    float height, int sides,
                    uint8_t sr, uint8_t sg, uint8_t sb,
                    bool capTop, uint8_t tr, uint8_t tg, uint8_t tb) {
  float yt = y0 + height;
  for (int i = 0; i < sides; i++) {
    float a0 = (float)i / sides * 2.0f * (float)M_PI;
    float a1 = (float)(i + 1) / sides * 2.0f * (float)M_PI;
    float c0 = cosf(a0), s0 = sinf(a0);
    float c1 = cosf(a1), s1 = sinf(a1);
    Vec3 b0 = vec3(cx + rb * c0, y0, cz + rb * s0);
    Vec3 b1 = vec3(cx + rb * c1, y0, cz + rb * s1);
    Vec3 t1 = vec3(cx + rt * c1, yt, cz + rt * s1);
    Vec3 t0 = vec3(cx + rt * c0, yt, cz + rt * s0);
    // Wound so the baked normal points radially OUTWARD.
    quad(b1, b0, t0, t1, sr, sg, sb);
  }
  if (capTop && rt > 0.01f) {
    Vec3 center = vec3(cx, yt, cz);
    int ic = addV(center);
    for (int i = 0; i < sides; i++) {
      float a0 = (float)i / sides * 2.0f * (float)M_PI;
      float a1 = (float)(i + 1) / sides * 2.0f * (float)M_PI;
      int i0 = addV(vec3(cx + rt * cosf(a0), yt, cz + rt * sinf(a0)));
      int i1 = addV(vec3(cx + rt * cosf(a1), yt, cz + rt * sinf(a1)));
      addF(ic, i1, i0, tr, tg, tb);   // wound so the cap normal points +y (up)
    }
  }
}

void Scene::cone(float cx, float cz, float y0, float radius, float height,
                 int sides, uint8_t r, uint8_t g, uint8_t b) {
  Vec3 apex = vec3(cx, y0 + height, cz);
  for (int i = 0; i < sides; i++) {
    float a0 = (float)i / sides * 2.0f * (float)M_PI;
    float a1 = (float)(i + 1) / sides * 2.0f * (float)M_PI;
    Vec3 p0 = vec3(cx + radius * cosf(a0), y0, cz + radius * sinf(a0));
    Vec3 p1 = vec3(cx + radius * cosf(a1), y0, cz + radius * sinf(a1));
    int ia = addV(apex), i0 = addV(p0), i1 = addV(p1);
    addF(ia, i1, i0, r, g, b);        // wound so the side normal points up-and-out
  }
}

void Scene::disc(float cx, float cz, float y, float radius, int sides,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t fl) {
  Vec3 center = vec3(cx, y, cz);
  int ic = addV(center);
  for (int i = 0; i < sides; i++) {
    float a0 = (float)i / sides * 2.0f * (float)M_PI;
    float a1 = (float)(i + 1) / sides * 2.0f * (float)M_PI;
    int i0 = addV(vec3(cx + radius * cosf(a0), y, cz + radius * sinf(a0)));
    int i1 = addV(vec3(cx + radius * cosf(a1), y, cz + radius * sinf(a1)));
    addF(ic, i1, i0, r, g, b, fl);    // wound so the disc normal points +y (up)
  }
}

// ----------------------------------------------------------------------------
// Camera basis helper — shared by renderScene and projectPoint.
// ----------------------------------------------------------------------------
struct Basis { Vec3 x, y, z; float cx, cy; };

static Basis makeBasis(const Camera &cam, Arduino_Canvas *cv) {
  Basis bs;
  Vec3 fwd = vnorm(vsub(cam.target, cam.eye));
  bs.z = vscale(fwd, -1.0f);                 // camera looks down -z
  bs.x = vnorm(vcross(cam.up, bs.z));
  bs.y = vcross(bs.z, bs.x);
  bs.cx = cv->width()  * 0.5f;
  bs.cy = cv->height() * 0.5f;
  return bs;
}

bool projectPoint(const Camera &cam, Arduino_Canvas *cv, Vec3 p,
                  float &sx, float &sy, float &depth) {
  Basis bs = makeBasis(cam, cv);
  Vec3 d = vsub(p, cam.eye);
  float vx = vdot(bs.x, d), vy = vdot(bs.y, d), vz = vdot(bs.z, d);
  if (vz > -0.05f) return false;             // behind / on the near plane
  float invz = cam.focal / (-vz);
  sx = bs.cx + vx * invz;
  sy = bs.cy - vy * invz;
  depth = -vz;
  return true;
}

// ----------------------------------------------------------------------------
// renderScene.
// ----------------------------------------------------------------------------
void renderScene(Arduino_Canvas *cv, const Scene &s,
                 const Camera &cam, const Lighting &L, bool cull) {
  Basis bs = makeBasis(cam, cv);

  // 1. Project every vertex once.
  for (int i = 0; i < s.nv; i++) {
    Vec3 d = vsub(s.vert[i], cam.eye);
    float vx = vdot(bs.x, d), vy = vdot(bs.y, d), vz = vdot(bs.z, d);
    if (vz > -0.05f) { vis[i] = false; continue; }
    float invz = cam.focal / (-vz);
    float X = bs.cx + vx * invz;
    float Y = bs.cy - vy * invz;
    // Clamp so a vertex hugging the near plane can't overflow the int16 cast
    // inside fillTriangle and wrap to a garbage scanline.
    if (X < -2000) X = -2000; else if (X > 2000) X = 2000;
    if (Y < -2000) Y = -2000; else if (Y > 2000) Y = 2000;
    px[i] = X; py[i] = Y; pz[i] = -vz; vis[i] = true;
  }

  // 2. Cull + collect visible faces with a depth key.
  int n = 0;
  for (int f = 0; f < s.nf; f++) {
    const Scene::Face &F = s.face[f];
    if (!vis[F.a] || !vis[F.b] || !vis[F.c]) continue;
    if (cull && !(F.flags & Scene::DOUBLE_SIDED)) {
      float area = (px[F.b] - px[F.a]) * (py[F.c] - py[F.a]) -
                   (px[F.c] - px[F.a]) * (py[F.b] - py[F.a]);
      if (area >= 0) continue;               // back-facing (CCW-front => area<0 on screen, y-down)
    }
    items[n].depth = pz[F.a] + pz[F.b] + pz[F.c];
    items[n].face  = (uint16_t)f;
    n++;
  }

  // 3. Sort far → near.
  qsort(items, n, sizeof(DrawItem), cmpFarFirst);

  // 4. Flat-shade + fill.
  for (int k = 0; k < n; k++) {
    int f = items[k].face;
    const Scene::Face &F = s.face[f];
    Vec3 nv = s.nrm[f];
    float ndl = vdot(nv, L.sunDir);
    // Double-sided faces (water, grass) get lit from whichever side faces the
    // sun so they never go fully black when the normal points away.
    if (F.flags & Scene::DOUBLE_SIDED) ndl = fabsf(ndl);
    else if (ndl < 0) ndl = 0;
    int r = (int)(F.cr * (L.ambR + L.sunR * ndl));
    int g = (int)(F.cg * (L.ambG + L.sunG * ndl));
    int b = (int)(F.cb * (L.ambB + L.sunB * ndl));
    cv->fillTriangle((int16_t)px[F.a], (int16_t)py[F.a],
                     (int16_t)px[F.b], (int16_t)py[F.b],
                     (int16_t)px[F.c], (int16_t)py[F.c],
                     rgb565(r, g, b));
  }
}
