// $P / $Q point-cloud recogniser. See write_recog.h for the references.
#include "write_recog.h"
#include "write_templates.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#if defined(ARDUINO)
  #include <esp_heap_caps.h>
#endif

namespace wrec {

namespace {

struct Tpl { char ch; bool user; Pt p[kN]; };

Tpl *sBuiltin = nullptr;
int  sBuiltinN = 0;
Tpl *sUser = nullptr;          // kMaxUser entries (PSRAM)
int  sUserN = 0;
constexpr int kMaxRaw = 1024;  // raw input points accepted by normalize()

void *bigAlloc(size_t n) {
#if defined(ARDUINO)
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return malloc(n);
}

inline float sq(float v) { return v * v; }

// ---- $P preprocessing ------------------------------------------------------
// Working copy as floats with stroke ids.
struct WPt { float x, y; uint8_t s; };

float pathLength(const WPt *p, int n) {
  float d = 0;
  for (int i = 1; i < n; i++)
    if (p[i].s == p[i - 1].s) d += sqrtf(sq(p[i].x - p[i - 1].x) + sq(p[i].y - p[i - 1].y));
  return d;
}

// Resample to exactly kN points, equidistant along the strokes' path; the
// gaps between strokes don't count as path ($P "Resample").
bool resample(const WPt *p, int n, Pt out[kN]) {
  float I = pathLength(p, n) / (kN - 1);
  if (!(I > 1e-6f)) return false;
  float D = 0;
  int k = 0;
  out[k++] = { p[0].x, p[0].y };
  WPt prev = p[0];
  for (int i = 1; i < n && k < kN; i++) {
    if (p[i].s != prev.s) { prev = p[i]; continue; }
    float d = sqrtf(sq(p[i].x - prev.x) + sq(p[i].y - prev.y));
    while (D + d >= I && k < kN) {
      float t = (I - D) / d;
      WPt q = { prev.x + t * (p[i].x - prev.x), prev.y + t * (p[i].y - prev.y), prev.s };
      out[k++] = { q.x, q.y };
      d -= (I - D);           // remaining length of this segment after q
      prev = q;
      D = 0;
    }
    D += d;
    prev = p[i];
  }
  // Float round-off can leave us one short: pad with the last point.
  while (k < kN) { out[k] = { p[n - 1].x, p[n - 1].y }; k++; }
  return true;
}

// Uniform scale into the unit box, then centroid to the origin.
bool scaleTranslate(Pt p[kN]) {
  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (int i = 0; i < kN; i++) {
    if (p[i].x < minX) minX = p[i].x; if (p[i].x > maxX) maxX = p[i].x;
    if (p[i].y < minY) minY = p[i].y; if (p[i].y > maxY) maxY = p[i].y;
  }
  float size = fmaxf(maxX - minX, maxY - minY);
  if (!(size > 1e-6f)) return false;
  float cx = 0, cy = 0;
  for (int i = 0; i < kN; i++) {
    p[i].x = (p[i].x - minX) / size; p[i].y = (p[i].y - minY) / size;
    cx += p[i].x; cy += p[i].y;
  }
  cx /= kN; cy /= kN;
  for (int i = 0; i < kN; i++) { p[i].x -= cx; p[i].y -= cy; }
  return true;
}

// ---- $P matching with $Q early abandoning -----------------------------------
// Weighted greedy matching of every point of a to its nearest unmatched point
// of b, starting at a[start]; weights fall from 1 to 1/n. Squared distances
// ($Q). Gives up as soon as the running sum reaches `minSoFar`.
float cloudDistance(const Pt *a, const Pt *b, int start, float minSoFar) {
  bool matched[kN];
  memset(matched, 0, sizeof matched);
  float sum = 0;
  int i = start, w = 0;
  do {
    int idx = -1;
    float best = 1e30f;
    for (int j = 0; j < kN; j++) {
      if (matched[j]) continue;
      float d = sq(a[i].x - b[j].x) + sq(a[i].y - b[j].y);
      if (d < best) { best = d; idx = j; }
    }
    matched[idx] = true;
    sum += (1.0f - (float)w / kN) * best;
    if (sum >= minSoFar) return sum;
    w++;
    i = (i + 1) % kN;
  } while (i != start);
  return sum;
}

float greedyCloudMatch(const Pt *pts, const Pt *tpl, float minSoFar) {
  // step = floor(n^(1 - eps)), eps = 0.5 -> 5 starting points for n = 32.
  const int step = (int)floorf(powf((float)kN, 0.5f));
  float m = minSoFar;
  for (int i = 0; i < kN; i += step) {
    float d1 = cloudDistance(pts, tpl, i, m);
    if (d1 < m) m = d1;
    float d2 = cloudDistance(tpl, pts, i, m);
    if (d2 < m) m = d2;
  }
  return m;
}

int charIndex(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= '0' && c <= '9') return 26 + (c - '0');
  return -1;
}
char indexChar(int i) { return i < 26 ? (char)('A' + i) : (char)('0' + i - 26); }
constexpr int kClasses = 36;

// Sum of the weights 1 - w/n for w = 0..n-1 = (n + 1) / 2.
constexpr float kWeightSum = (kN + 1) * 0.5f;

float scoreOf(float dist) {
  float rms = sqrtf(dist / kWeightSum);
  float s = 1.0f - 2.0f * rms;
  return s < 0 ? 0 : (s > 1 ? 1 : s);
}

WPt *sWork = nullptr;          // kMaxRaw working points (PSRAM, not the task stack)

bool buildFromStrokes(const uint8_t *&b, Pt out[kN], char &ch) {
  ch = (char)*b++;
  int ns = *b++;
  WPt *tmp = sWork;
  int n = 0;
  for (int s = 0; s < ns; s++) {
    int np = *b++;
    for (int k = 0; k < np; k++) {
      if (n < kMaxRaw) tmp[n++] = { (float)b[0], (float)b[1], (uint8_t)s };
      b += 2;
    }
  }
  return resample(tmp, n, out) && scaleTranslate(out);
}

}  // namespace

bool initFrom(const uint8_t *blob, int count) {
  if (sBuiltin) { free(sBuiltin); sBuiltin = nullptr; sBuiltinN = 0; }
  if (!sWork) sWork = (WPt *)bigAlloc(sizeof(WPt) * kMaxRaw);
  if (!sUser) sUser = (Tpl *)bigAlloc(sizeof(Tpl) * kMaxUser);
  Tpl *t = (Tpl *)bigAlloc(sizeof(Tpl) * (count ? count : 1));
  if (!t || !sWork || !sUser) return false;
  const uint8_t *b = blob;
  int n = 0;
  for (int i = 0; i < count; i++) {
    char ch;
    if (buildFromStrokes(b, t[n].p, ch)) { t[n].ch = ch; t[n].user = false; n++; }
  }
  sBuiltin = t;
  sBuiltinN = n;
  return true;
}

bool init() {
  if (sBuiltin) return true;
  if (!sWork) sWork = (WPt *)bigAlloc(sizeof(WPt) * kMaxRaw);
  if (!sUser) sUser = (Tpl *)bigAlloc(sizeof(Tpl) * kMaxUser);
  Tpl *t = (Tpl *)bigAlloc(sizeof(Tpl) * kWriteTplCount);
  if (!t || !sWork || !sUser) return false;
  const uint8_t *b = kWriteTplBlob;
  int n = 0;
  for (int i = 0; i < kWriteTplCount; i++) {
    char ch;
    if (buildFromStrokes(b, t[n].p, ch)) { t[n].ch = ch; t[n].user = false; n++; }
  }
  sBuiltin = t;
  sBuiltinN = n;
  return true;
}

int builtinCount() { return sBuiltinN; }
int userCount() { return sUserN; }

bool normalize(const RawPt *pts, int n, Pt out[kN]) {
  if (n < 2 || !sWork) return false;
  if (n > kMaxRaw) n = kMaxRaw;
  for (int i = 0; i < n; i++) sWork[i] = { (float)pts[i].x, (float)pts[i].y, pts[i].stroke };
  return resample(sWork, n, out) && scaleTranslate(out);
}

int classify(const Pt cloud[kN], Cand *out, int maxOut) {
  float best[kClasses];
  bool  bestUser[kClasses];
  for (int i = 0; i < kClasses; i++) { best[i] = 1e30f; bestUser[i] = false; }
  if (maxOut > kClasses) maxOut = kClasses;
  auto kthBest = [&](int k) {      // k-th smallest of best[] (k >= 1)
    float a[kClasses];
    memcpy(a, best, sizeof a);
    for (int r = 0; r < k; r++) {  // partial selection
      int m = r;
      for (int j = r + 1; j < kClasses; j++) if (a[j] < a[m]) m = j;
      float t = a[r]; a[r] = a[m]; a[m] = t;
    }
    return a[k - 1];
  };
  auto run = [&](const Tpl &t) {
    int ci = charIndex(t.ch);
    if (ci < 0) return;
    // Only matters if it can improve this char AND reach the top maxOut.
    float limit = fminf(best[ci], kthBest(maxOut));
    float d = greedyCloudMatch(cloud, t.p, limit);
    if (d < best[ci]) { best[ci] = d; bestUser[ci] = t.user; }
  };
  for (int i = 0; i < sBuiltinN; i++) run(sBuiltin[i]);
  for (int i = 0; i < sUserN; i++) run(sUser[i]);

  int n = 0;
  bool used[kClasses] = {};
  for (; n < maxOut; n++) {
    int m = -1;
    for (int j = 0; j < kClasses; j++)
      if (!used[j] && best[j] < 1e29f && (m < 0 || best[j] < best[m])) m = j;
    if (m < 0) break;
    used[m] = true;
    out[n] = { indexChar(m), best[m], scoreOf(best[m]), bestUser[m] };
  }
  return n;
}

Edit editGesture(const RawPt *pts, int n, int minLenPx) {
  if (n < 2) return Edit::None;
  for (int i = 1; i < n; i++) if (pts[i].stroke != pts[0].stroke) return Edit::None;
  float dx = pts[n - 1].x - pts[0].x, dy = pts[n - 1].y - pts[0].y;
  float chord = sqrtf(dx * dx + dy * dy);
  if (chord < minLenPx) return Edit::None;
  float len = 0, maxDev = 0;
  for (int i = 1; i < n; i++) {
    len += sqrtf(sq((float)pts[i].x - pts[i - 1].x) + sq((float)pts[i].y - pts[i - 1].y));
    // perpendicular distance from the chord
    float dev = fabsf(dx * (pts[0].y - pts[i].y) - dy * (pts[0].x - pts[i].x)) / chord;
    if (dev > maxDev) maxDev = dev;
  }
  if (chord / len < 0.90f || maxDev > 0.14f * chord) return Edit::None;   // not straight
  float ang = atan2f(dy, dx) * 57.29578f;      // 0 = right, 90 = down (screen y)
  if (fabsf(ang) <= 25.0f) return Edit::Space;                 // left -> right
  if (fabsf(ang) >= 155.0f) return Edit::Backspace;            // right -> left
  if (ang >= 110.0f && ang <= 160.0f) return Edit::Enter;      // top-right -> bottom-left
  return Edit::None;
}

const char *editName(Edit e) {
  switch (e) {
    case Edit::Space: return "space";
    case Edit::Backspace: return "backspace";
    case Edit::Enter: return "enter";
    default: return "none";
  }
}

bool addUser(char ch, const Pt cloud[kN]) {
  if (charIndex(ch) < 0 || !sUser) return false;
  // Keep the newest kUserPerChar per char, and kMaxUser overall: drop the
  // oldest entry of this char (or the oldest overall) when full.
  int count = 0, oldestSame = -1;
  for (int i = 0; i < sUserN; i++)
    if (sUser[i].ch == ch) { if (oldestSame < 0) oldestSame = i; count++; }
  int drop = -1;
  if (count >= kUserPerChar) drop = oldestSame;
  else if (sUserN >= kMaxUser) drop = 0;
  if (drop >= 0) {
    memmove(&sUser[drop], &sUser[drop + 1], sizeof(Tpl) * (sUserN - drop - 1));
    sUserN--;
  }
  sUser[sUserN].ch = ch;
  sUser[sUserN].user = true;
  memcpy(sUser[sUserN].p, cloud, sizeof(Pt) * kN);
  sUserN++;
  return true;
}

void clearUser() { sUserN = 0; }

bool userAt(int i, char *ch, Pt out[kN]) {
  if (i < 0 || i >= sUserN) return false;
  *ch = sUser[i].ch;
  memcpy(out, sUser[i].p, sizeof(Pt) * kN);
  return true;
}

size_t userSerialize(uint8_t *buf, size_t cap) {
  size_t need = 5 + (size_t)sUserN * (1 + 2 * kN);
  if (cap < need) return 0;
  memcpy(buf, "WRT1", 4);
  buf[4] = (uint8_t)sUserN;
  uint8_t *p = buf + 5;
  for (int i = 0; i < sUserN; i++) {
    *p++ = (uint8_t)sUser[i].ch;
    for (int k = 0; k < kN; k++) {
      float vx = sUser[i].p[k].x * 100.f, vy = sUser[i].p[k].y * 100.f;
      p[k]      = (uint8_t)(int8_t)(vx > 127 ? 127 : vx < -127 ? -127 : lroundf(vx));
      p[kN + k] = (uint8_t)(int8_t)(vy > 127 ? 127 : vy < -127 ? -127 : lroundf(vy));
    }
    p += 2 * kN;
  }
  return need;
}

bool userDeserialize(const uint8_t *buf, size_t len) {
  if (!sUser || len < 5 || memcmp(buf, "WRT1", 4) != 0) return false;
  int n = buf[4];
  if (n > kMaxUser || len < 5 + (size_t)n * (1 + 2 * kN)) return false;
  const uint8_t *p = buf + 5;
  sUserN = 0;
  for (int i = 0; i < n; i++) {
    char ch = (char)*p++;
    if (charIndex(ch) < 0) { p += 2 * kN; continue; }
    Tpl &t = sUser[sUserN++];
    t.ch = ch; t.user = true;
    for (int k = 0; k < kN; k++) {
      t.p[k].x = (int8_t)p[k] / 100.f;
      t.p[k].y = (int8_t)p[kN + k] / 100.f;
    }
    p += 2 * kN;
  }
  return true;
}

}  // namespace wrec
