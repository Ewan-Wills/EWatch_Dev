// Claude Face — pure drawing (see claude_face_draw.h). Host-portable: only
// the Arduino_GFX API, <math.h>, <string.h>, <stdio.h>, the fonts and the
// protocol header. The fonts are ASCII-only, so an em dash ("unknown") is
// drawn as a short bar and the '|' separator in current_tool as a middle dot.
#include <Arduino_GFX.h>          // core GFX class only (host-portable)
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "claude_face_draw.h"
#include "cf_util.h"
#include "fonts/CF_SpaceGroteskBold40.h"
#include "fonts/CF_SpaceGroteskBold22.h"
#include "fonts/CF_SpaceGroteskMed12.h"
#include "fonts/CF_SpaceGroteskMed9.h"
#include "fonts/CF_JetBrainsMono10.h"

namespace {

// ---------------------------------------------------------------------------
// Palette (RGB565). Warm near-black ground, burnt orange brand accent.
// ---------------------------------------------------------------------------
const uint16_t C_BG     = 0x1082;   // #141210
const uint16_t C_TRACK  = 0x2924;   // #2A2622 ring track / bar track
const uint16_t C_TEXT   = 0xF77C;   // #F2EDE6 cream
const uint16_t C_DIM    = 0x8C2F;   // #8A847C secondary text
const uint16_t C_FAINT  = 0x4A49;   // #4A4848 tertiary / disabled
const uint16_t C_ORANGE = 0xEB45;   // #E86B2B brand
const uint16_t C_HOT    = 0xFBC3;   // #FF7A1A BLOCKED
const uint16_t C_RED    = 0xF904;   // #F82020 LIMITED / over-pace
const uint16_t C_GREEN  = 0x3E8B;   // #3CD25A DONE
const uint16_t C_AMBER  = 0xFD84;   // #FFB020 WAITING_INPUT / cache cold
const uint16_t C_VIOLET = 0xA3FE;   // #A07CF0 COMPACTING
const uint16_t C_GREY   = 0x5AAA;   // #5A5550 offline rings
const uint16_t C_DIMORG = 0x6A43;   // #6A4A18-ish dimmed fill (stale RL)
const uint16_t C_PULSE  = 0x6180;   // dark orange for the BLOCKED pulse

const int16_t W = 240;
const int16_t CX = 120, CY = 108;             // ring centre
const int16_t R5O = 58, R5I = 50;             // 5h ring (outer)
const int16_t R7O = 45, R7I = 39;             // 7d ring (inner)
const int16_t RDISC = 37;                     // centre disc

const GFXfont *F40 = &CF_SpaceGroteskBold40;
const GFXfont *F22 = &CF_SpaceGroteskBold22;
const GFXfont *F12 = &CF_SpaceGroteskMed12;
const GFXfont *F9  = &CF_SpaceGroteskMed9;
const GFXfont *FM  = &CF_JetBrainsMono10;

// In-string markers understood by the text helpers.
const char DASH = '\x01';     // "unknown" em dash -> short bar
// '|' -> middle dot

uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  if (t <= 0) return a;
  if (t >= 1) return b;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (int)((br - ar) * t + 0.5f);
  int g = ag + (int)((bg - ag) * t + 0.5f);
  int bl = ab + (int)((bb - ab) * t + 0.5f);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
const GFXglyph *glyphOf(const GFXfont *f, char c) {
  uint8_t u = (uint8_t)c;
  if (u < f->first || u > f->last) return nullptr;
  return &f->glyph[u - f->first];
}
int16_t advOf(const GFXfont *f, char c) {
  if (c == DASH) { const GFXglyph *z = glyphOf(f, '0'); return z ? z->xAdvance : 8; }
  if (c == '|') {
    const GFXglyph *s = glyphOf(f, ' ');
    return (int16_t)((s ? s->xAdvance : 4) * 2);
  }
  const GFXglyph *gl = glyphOf(f, c);
  if (!gl) gl = glyphOf(f, ' ');
  return gl ? gl->xAdvance : 0;
}
int16_t textW(const GFXfont *f, const char *s) {
  int16_t w = 0;
  for (; *s; s++) w += advOf(f, *s);
  return w;
}
// Height of the digit '0' (cap-ish height) for vertical placement.
int16_t capH(const GFXfont *f) {
  const GFXglyph *z = glyphOf(f, '0');
  return z ? z->height : 8;
}

// Draw at baseline y. Returns the x after the run.
int16_t text(Arduino_GFX *g, const GFXfont *f, int16_t x, int16_t y,
             const char *s, uint16_t color) {
  g->setFont(f);
  g->setTextSize(1);
  g->setTextWrap(false);
  g->setTextColor(color);
  int16_t ch = capH(f);
  for (; *s; s++) {
    char c = *s;
    int16_t a = advOf(f, c);
    if (c == DASH) {
      int16_t t = ch >= 20 ? 3 : 2;
      int16_t bw = a - (a / 4);
      g->fillRect(x + (a - bw) / 2, y - ch / 2 - t / 2, bw, t, color);
    } else if (c == '|') {
      int16_t r = ch >= 12 ? 2 : 1;
      g->fillRect(x + a / 2 - r / 2, y - ch / 2 - r / 2, r + 1, r + 1, color);
    } else if (glyphOf(f, c)) {
      g->setCursor(x, y);
      g->write((uint8_t)c);
    }
    x += a;
  }
  return x;
}
void textC(Arduino_GFX *g, const GFXfont *f, int16_t cx, int16_t y,
           const char *s, uint16_t color) {
  text(g, f, cx - textW(f, s) / 2, y, s, color);
}
void textR(Arduino_GFX *g, const GFXfont *f, int16_t rx, int16_t y,
           const char *s, uint16_t color) {
  text(g, f, rx - textW(f, s), y, s, color);
}
// Truncate s in place so it fits maxW, ending in '.' when cut.
void fit(const GFXfont *f, char *s, int16_t maxW) {
  if (textW(f, s) <= maxW) return;
  size_t n = strlen(s);
  int16_t dotW = advOf(f, '.');
  while (n > 0) {
    s[--n] = '\0';
    if (textW(f, s) + dotW <= maxW) break;
  }
  s[n] = '.'; s[n + 1] = '\0';
}

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
// Ring segment from pct p0 to p1 (0..100, 0 = 12 o'clock, clockwise).
// Arduino_GFX angles: 0 deg = 3 o'clock, increasing clockwise; split at 360
// so no call has to wrap.
void arcPct(Arduino_GFX *g, int16_t ro, int16_t ri, float p0, float p1, uint16_t c) {
  if (p1 <= p0 + 0.05f) return;
  if (p0 < 0) p0 = 0;
  if (p1 > 100) p1 = 100;
  float a0 = 270.0f + p0 * 3.6f, a1 = 270.0f + p1 * 3.6f;
  if (a1 <= 360.0f) {
    g->fillArc(CX, CY, ro, ri, a0, a1 >= 360.0f ? 359.9f : a1, c);
  } else if (a0 >= 360.0f) {
    g->fillArc(CX, CY, ro, ri, a0 - 360.0f, a1 - 360.0f, c);
  } else {
    g->fillArc(CX, CY, ro, ri, a0, 359.9f, c);
    g->fillArc(CX, CY, ro, ri, 0.0f, a1 - 360.0f, c);
  }
}
// Arc in plain degrees from 12 o'clock (for spinners). Handles any range.
void arcDeg(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t ro, int16_t ri,
            float d0, float span, uint16_t c) {
  float a0 = fmodf(270.0f + d0, 360.0f);
  if (a0 < 0) a0 += 360.0f;
  float a1 = a0 + span;
  if (a1 <= 360.0f) {
    g->fillArc(cx, cy, ro, ri, a0, a1 >= 360.0f ? 359.9f : a1, c);
  } else {
    g->fillArc(cx, cy, ro, ri, a0, 359.9f, c);
    g->fillArc(cx, cy, ro, ri, 0.0f, a1 - 360.0f, c);
  }
}
// Thick line as two triangles.
void thickLine(Arduino_GFX *g, float x0, float y0, float x1, float y1, float w,
               uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
  int16_t ax = (int16_t)lroundf(x0 + nx), ay = (int16_t)lroundf(y0 + ny);
  int16_t bx = (int16_t)lroundf(x0 - nx), by = (int16_t)lroundf(y0 - ny);
  int16_t cx = (int16_t)lroundf(x1 + nx), cy = (int16_t)lroundf(y1 + ny);
  int16_t ex = (int16_t)lroundf(x1 - nx), ey = (int16_t)lroundf(y1 - ny);
  g->fillTriangle(ax, ay, bx, by, cx, cy, c);
  g->fillTriangle(bx, by, cx, cy, ex, ey, c);
  int16_t r = (int16_t)(w * 0.5f);
  g->fillCircle((int16_t)lroundf(x0), (int16_t)lroundf(y0), r, c);
  g->fillCircle((int16_t)lroundf(x1), (int16_t)lroundf(y1), r, c);
}
void checkMark(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t s, uint16_t c) {
  float w = s * 0.22f;
  thickLine(g, cx - s * 0.5f, cy, cx - s * 0.15f, cy + s * 0.35f, w, c);
  thickLine(g, cx - s * 0.15f, cy + s * 0.35f, cx + s * 0.55f, cy - s * 0.4f, w, c);
}

// ---------------------------------------------------------------------------
// Derived values
// ---------------------------------------------------------------------------
bool linkLive(const CFDrawInput &in) { return in.link == CF_LINK_LIVE; }

// "Now" for countdowns: the bridge clock advanced by the local age of the
// snapshot (immune to a mis-set RTC); the RTC otherwise.
uint32_t effNow(const CFDrawInput &in) {
  if (in.haveState && in.st.bridge_now) return in.st.bridge_now + in.rxAgeMs / 1000UL;
  return in.nowEpoch;
}

bool rlUnknown(const CFDrawInput &in) {
  return !in.haveState || (in.st.flags & CF_FLAGS_NO_RATE_LIMITS) ||
         (in.st.five_used == CF_PCT_UNKNOWN && in.st.seven_used == CF_PCT_UNKNOWN);
}
bool rlStale(const CFDrawInput &in) {
  if (!in.haveState) return false;
  if (in.st.flags & CF_FLAGS_RL_STALE) return true;
  if (in.st.rl_age_s == CF_U16_UNKNOWN) return false;
  return (uint32_t)in.st.rl_age_s + in.rxAgeMs / 1000UL > CF_RL_STALE_S;
}

uint16_t stateColor(uint8_t s) {
  switch (s) {
    case CF_STATE_WORKING:       return C_ORANGE;
    case CF_STATE_COMPACTING:    return C_VIOLET;
    case CF_STATE_DONE:          return C_GREEN;
    case CF_STATE_WAITING_INPUT: return C_AMBER;
    case CF_STATE_BLOCKED:       return C_HOT;
    case CF_STATE_LIMITED:       return C_RED;
    default:                     return C_DIM;
  }
}

void fmtLocalHM(const CFDrawInput &in, uint32_t epochUtc, char *out, size_t n) {
  CFCivil c = cfCivilFromEpoch((int64_t)epochUtc + (int64_t)in.utcOffsetMin * 60);
  snprintf(out, n, "%02u:%02u", c.hour, c.minute);
}

// Which window's reset matters while LIMITED.
uint32_t limitedResetAt(const CFDrawInput &in) {
  const CFStateMsg &s = in.st;
  if (s.seven_used != CF_PCT_UNKNOWN && s.seven_used >= 100 && s.seven_resets_at)
    return s.seven_resets_at;
  return s.five_resets_at;
}
void limitedCountdown(const CFDrawInput &in, char *out, size_t n) {
  uint32_t at = limitedResetAt(in), now = effNow(in);
  if (!at || !now) { out[0] = DASH; out[1] = '\0'; return; }
  cfFmtDur(out, n, at > now ? at - now : 0);
}

void heroCaption(const CFDrawInput &in, uint8_t vis, char *out, size_t n) {
  if (vis == CF_VIS_OFFLINE) {
    if (in.link == CF_LINK_NEVER || !in.haveState) { snprintf(out, n, "waiting for bridge"); return; }
    // floor, not round-up: 4m10s ago reads "4m"
    uint32_t ago = in.rxAgeMs / 1000UL;
    char d[12];
    if (ago < 60) snprintf(d, sizeof d, "<1m");
    else cfFmtDur(d, sizeof d, (ago / 60UL) * 60UL);
    snprintf(out, n, "last seen %s", d);
    return;
  }
  const CFStateMsg &s = in.st;
  uint32_t now = effNow(in);
  if (vis == CF_STATE_LIMITED) {
    uint32_t at = limitedResetAt(in);
    if (at) { char hm[8]; fmtLocalHM(in, at, hm, sizeof hm); snprintf(out, n, "resets %s", hm); }
    else snprintf(out, n, "resets %c", DASH);
    return;
  }
  if (rlUnknown(in)) { snprintf(out, n, "5h %c   7d %c", DASH, DASH); return; }
  if (s.projected_hit && now && s.projected_hit > now) {
    char hm[8]; fmtLocalHM(in, s.projected_hit, hm, sizeof hm);
    snprintf(out, n, "hits limit ~%s", hm);
    return;
  }
  if (s.five_resets_at && now && s.five_resets_at > now) {
    char d[12]; cfFmtDur(d, sizeof d, s.five_resets_at - now);
    snprintf(out, n, "5h resets %s", d);
    return;
  }
  snprintf(out, n, "5h resets %c", DASH);
}

bool cacheColdSoon(const CFDrawInput &in, char *out, size_t n) {
  if (!in.haveState || !linkLive(in)) return false;
  const CFStateMsg &s = in.st;
  if (s.p_state != CF_STATE_DONE && s.p_state != CF_STATE_WAITING_INPUT) return false;
  uint32_t now = effNow(in);
  if (!s.p_cache_expires_at || !now || s.p_cache_expires_at <= now) return false;
  uint32_t left = s.p_cache_expires_at - now;
  if (left > 300) return false;
  char d[12]; cfFmtDur(d, sizeof d, left);
  snprintf(out, n, "cache cold in %s", d);
  return true;
}

uint32_t fnv(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  while (n--) { h ^= *b++; h *= 16777619UL; }
  return h;
}
uint32_t fnvStr(uint32_t h, const char *s) { return fnv(h, s, strlen(s) + 1); }
uint32_t fnvU(uint32_t h, uint32_t v) { return fnv(h, &v, sizeof v); }

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------
void drawLinkGlyph(Arduino_GFX *g, int16_t x, int16_t y, const CFDrawInput &in) {
  uint16_t c = in.versionMismatch ? C_RED
             : in.link == CF_LINK_LIVE ? C_ORANGE
             : in.link == CF_LINK_OFFLINE ? C_DIM : C_FAINT;
  // two interlocking links
  g->drawRoundRect(x, y, 12, 7, 3, c);
  g->drawRoundRect(x + 7, y + 4, 12, 7, 3, c);
  if (in.link != CF_LINK_LIVE && !in.versionMismatch)
    g->drawLine(x + 1, y + 12, x + 18, y - 1, c);
}

void drawStatus(Arduino_GFX *g, const CFDrawInput &in) {
  g->fillRect(0, 0, W, 48, C_BG);
  char t[8];
  if (in.rtcOk) snprintf(t, sizeof t, "%02u:%02u", in.hour, in.minute);
  else          snprintf(t, sizeof t, "--:--");
  text(g, F40, 10, 43, t, C_TEXT);
  drawLinkGlyph(g, 210, 10, in);
  char b[8];
  if (in.batOk) snprintf(b, sizeof b, "%u%%", in.batPct);
  else          snprintf(b, sizeof b, "%c%%", DASH);
  textR(g, F12, 230, 42, b, in.batOk && in.batPct <= 15 ? C_RED : C_DIM);
}

enum RingMode { RING_NORMAL, RING_STALE, RING_OFFLINE, RING_LIMITED };

void drawRing(Arduino_GFX *g, int16_t ro, int16_t ri, uint8_t used, uint8_t elapsed,
              int8_t pace, RingMode mode) {
  arcPct(g, ro, ri, 0, 100, C_TRACK);
  if (used == CF_PCT_UNKNOWN) return;          // "—": track only
  float u = used > 100 ? 100.0f : (float)used;
  bool haveEl = elapsed != CF_PCT_UNKNOWN;
  float e = haveEl ? (elapsed > 100 ? 100.0f : (float)elapsed) : u;
  uint16_t behind, ahead;
  switch (mode) {
    case RING_OFFLINE: behind = ahead = C_GREY; break;
    case RING_STALE:   behind = ahead = C_DIMORG; break;
    case RING_LIMITED: behind = ahead = C_RED; break;
    default: {
      behind = C_ORANGE;
      float over = (pace != CF_PACE_UNKNOWN) ? (float)pace : (u - e);
      ahead = lerp565(C_ORANGE, C_RED, over / 25.0f);
      if (ahead == C_ORANGE) ahead = lerp565(C_ORANGE, C_RED, 0.2f);
      break;
    }
  }
  float split = u < e ? u : e;
  arcPct(g, ro, ri, 0, split, behind);
  if (u > e) arcPct(g, ro, ri, e, u, ahead);
  if (haveEl && mode != RING_LIMITED) {
    uint16_t tc = mode == RING_OFFLINE ? C_DIM : C_TEXT;
    float a = e * 3.6f;
    arcDeg(g, CX, CY, ro + 2, ri - 3, a - 1.6f, 3.2f, tc);
  }
}

void drawCentre(Arduino_GFX *g, const CFDrawInput &in, uint8_t vis) {
  float pulse = 0;
  if (vis == CF_STATE_BLOCKED)
    pulse = 0.5f + 0.5f * sinf((float)(in.nowMs % 1000UL) / 1000.0f * 6.2831853f);
  g->fillCircle(CX, CY, RDISC, vis == CF_STATE_BLOCKED ? lerp565(C_BG, C_PULSE, pulse) : C_BG);

  switch (vis) {
    case CF_VIS_MISMATCH:
      break;
    case CF_VIS_OFFLINE:
      g->fillRect(CX - 12, CY - 12, 9, 3, C_DIM);
      g->fillRect(CX + 3, CY - 12, 9, 3, C_DIM);
      textC(g, F9, CX, CY + 12, in.link == CF_LINK_NEVER ? "NO LINK" : "OFFLINE", C_DIM);
      break;
    case CF_STATE_WORKING: {
      float a = (float)(in.nowMs % 2000UL) * 0.18f;          // 1 rev / 2 s
      arcPct(g, 34, 30, 0, 100, C_TRACK);
      arcDeg(g, CX, CY, 34, 30, a, 100.0f, C_ORANGE);
      textC(g, F9, CX, CY + 4, "WORKING", C_TEXT);
      break;
    }
    case CF_STATE_COMPACTING: {
      // dotted spinner running counter-clockwise
      int head = 11 - (int)((in.nowMs % 1500UL) * 12UL / 1500UL);
      for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 0.0174533f;
        int16_t x = (int16_t)lroundf(CX + sinf(a) * 31.0f);
        int16_t y = (int16_t)lroundf(CY - cosf(a) * 31.0f);
        int d = (i - head + 12) % 12;                        // trail behind head
        uint16_t c = d == 0 ? C_VIOLET : d < 4 ? lerp565(C_VIOLET, C_TRACK, d / 4.0f) : C_TRACK;
        g->fillCircle(x, y, d == 0 ? 3 : 2, c);
      }
      textC(g, F9, CX, CY + 4, "COMPACT", C_TEXT);
      break;
    }
    case CF_STATE_DONE:
      checkMark(g, CX, CY - 8, 30, C_GREEN);
      textC(g, F9, CX, CY + 20, "YOUR TURN", C_TEXT);
      break;
    case CF_STATE_WAITING_INPUT:
      checkMark(g, CX, CY - 8, 30, C_AMBER);
      textC(g, F9, CX, CY + 20, "WAITING", C_AMBER);
      break;
    case CF_STATE_BLOCKED: {
      uint16_t c = lerp565(C_HOT, C_TEXT, pulse * 0.35f);
      g->fillRoundRect(CX - 3, CY - 26, 7, 22, 3, c);
      g->fillCircle(CX, CY + 1, 4, c);
      textC(g, F9, CX, CY + 20, "NEEDS YOU", C_TEXT);
      break;
    }
    case CF_STATE_LIMITED: {
      char d[12]; limitedCountdown(in, d, sizeof d);
      textC(g, F22, CX, CY + 6, d, C_RED);
      textC(g, F9, CX, CY + 22, "LIMITED", C_RED);
      break;
    }
    default: {                                   // NONE / IDLE / STALE
      g->drawCircle(CX, CY - 8, 7, C_FAINT);
      const char *w = vis == CF_STATE_IDLE ? "IDLE" : vis == CF_STATE_STALE ? "STALE" : "NO AGENTS";
      textC(g, F9, CX, CY + 16, w, C_DIM);
      break;
    }
  }
}

void drawHero(Arduino_GFX *g, const CFDrawInput &in, uint8_t vis) {
  g->fillRect(0, 48, W, 137, C_BG);
  if (vis == CF_VIS_MISMATCH) {
    textC(g, F22, CX, CY - 4, "UPDATE", C_RED);
    textC(g, F22, CX, CY + 24, "BRIDGE", C_RED);
    return;
  }
  // BLOCKED halo pulse around the outer ring.
  if (vis == CF_STATE_BLOCKED) {
    float p = 0.5f + 0.5f * sinf((float)(in.nowMs % 1000UL) / 1000.0f * 6.2831853f);
    // r 59..60: top row y=48 stays inside HERO (status strip ends at 47)
    arcPct(g, R5O + 2, R5O + 1, 0, 100, lerp565(C_BG, C_HOT, p));
  }
  RingMode mode = RING_NORMAL;
  if (vis == CF_VIS_OFFLINE) mode = RING_OFFLINE;
  else if (vis == CF_STATE_LIMITED) mode = RING_LIMITED;
  else if (rlStale(in)) mode = RING_STALE;
  bool unk = rlUnknown(in);
  uint8_t fu = unk ? CF_PCT_UNKNOWN : in.st.five_used;
  uint8_t su = unk ? CF_PCT_UNKNOWN : in.st.seven_used;
  drawRing(g, R5O, R5I, fu, in.haveState ? in.st.five_elapsed : CF_PCT_UNKNOWN,
           in.haveState ? in.st.five_pace : (int8_t)CF_PACE_UNKNOWN, mode);
  drawRing(g, R7O, R7I, su, in.haveState ? in.st.seven_elapsed : CF_PCT_UNKNOWN,
           in.haveState ? in.st.seven_pace : (int8_t)CF_PACE_UNKNOWN, mode);
  drawCentre(g, in, vis);
  char cap[32]; heroCaption(in, vis, cap, sizeof cap);
  uint16_t cc = vis == CF_STATE_LIMITED ? C_RED : C_DIM;
  if (vis != CF_VIS_OFFLINE && !unk && in.st.projected_hit && effNow(in) &&
      in.st.projected_hit > effNow(in) && vis != CF_STATE_LIMITED)
    cc = C_AMBER;
  textC(g, F12, CX, 180, cap, cc);   // 'g' descender ends at y=184
}

void drawActivity(Arduino_GFX *g, const CFDrawInput &in, uint8_t vis) {
  g->fillRect(0, 185, W, 69, C_BG);
  if (vis == CF_VIS_MISMATCH) return;
  if (!in.haveState) {
    textC(g, F12, CX, 222, in.link == CF_LINK_NEVER ? "bridge not connected"
                                                    : "no data", C_FAINT);
    return;
  }
  const CFStateMsg &s = in.st;
  bool dim = vis == CF_VIS_OFFLINE;
  if (s.primary_index == 0xFF && s.p_state == CF_STATE_NONE && !s.p_project[0]) {
    textC(g, F12, CX, 222, "no active session", dim ? C_FAINT : C_DIM);
    return;
  }
  uint16_t cText = dim ? C_DIM : C_TEXT, cDim = dim ? C_FAINT : C_DIM;
  uint16_t cState = dim ? C_GREY : stateColor(s.p_state);

  // Row 1: state dot + project ........ turn timer (live) or last turn (dim)
  g->fillCircle(16, 196, 4, cState);
  char tm[20];                        // "last " + up to "h:mm:ss" fits
  uint32_t te = cfTurnElapsed(in);
  bool live = s.p_state == CF_STATE_WORKING || s.p_state == CF_STATE_BLOCKED ||
              s.p_state == CF_STATE_COMPACTING;
  uint16_t tc = cDim;
  if (te || live) {
    cfFmtMMSS(tm, sizeof tm, te);
    if (!dim && live) tc = stateColor(s.p_state);
  } else if (s.p_last_turn_s) {
    char d[12]; cfFmtMMSS(d, sizeof d, s.p_last_turn_s);
    snprintf(tm, sizeof tm, "last %s", d);
  } else {
    tm[0] = DASH; tm[1] = '\0';
  }
  int16_t tw = textW(F12, tm);
  textR(g, F12, 228, 201, tm, tc);
  char proj[20]; cfCopyStr(proj, sizeof proj, s.p_project, sizeof s.p_project);
  if (!proj[0]) { proj[0] = DASH; proj[1] = '\0'; }
  fit(F12, proj, (int16_t)(228 - tw - 10 - 26));
  text(g, F12, 26, 201, proj, cText);

  // Row 2: current tool (monospace, '|' drawn as a middle dot)
  char tool[28]; cfCopyStr(tool, sizeof tool, s.p_current_tool, sizeof s.p_current_tool);
  if (tool[0]) {
    fit(FM, tool, 216);
    text(g, FM, 12, 222, tool, (s.p_flags & CF_PFLAGS_HUNG) && !dim ? C_AMBER : cDim);
  } else {
    char tc2[24];
    snprintf(tc2, sizeof tc2, "%u tools", (unsigned)s.p_tool_count);
    text(g, FM, 12, 222, tc2, C_FAINT);
  }

  // Row 3: context bar + % ........ subagent dots
  const int16_t bx = 12, by = 234, bw = 110, bh = 4;
  g->fillRoundRect(bx, by, bw, bh, 2, C_TRACK);
  char cx[12];
  if (s.p_ctx != CF_PCT_UNKNOWN) {
    uint8_t p = s.p_ctx > 100 ? 100 : s.p_ctx;
    uint16_t c = dim ? C_GREY : p >= 90 ? C_RED : p >= 70 ? C_ORANGE : C_TEXT;
    int16_t fw = (int16_t)((int32_t)bw * p / 100);
    if (fw > 0) g->fillRoundRect(bx, by, fw < bh ? bh : fw, bh, 2, c);
    snprintf(cx, sizeof cx, "ctx %u%%", p);
  } else {
    snprintf(cx, sizeof cx, "ctx %c", DASH);
  }
  text(g, F9, bx + bw + 5, by + 6, cx, cDim);
  uint8_t n = s.subagent_count > 5 ? 5 : s.subagent_count;
  for (uint8_t i = 0; i < n; i++)
    g->fillCircle(226 - i * 8, by + 2, 3, dim ? C_GREY : C_ORANGE);
}

void drawFooter(Arduino_GFX *g, const CFDrawInput &in, uint8_t vis) {
  g->fillRect(0, 254, W, 26, C_BG);
  if (vis == CF_VIS_MISMATCH || !in.haveState) return;
  bool dim = vis == CF_VIS_OFFLINE;
  char cache[28];
  bool cold = cacheColdSoon(in, cache, sizeof cache);
  int16_t x = 12;
  if (cold) x = text(g, F9, 12, 271, cache, C_AMBER) + 10;
  int shown = 0;
  if (in.haveSessions) {
    uint8_t cnt = in.ss.count > 4 ? 4 : in.ss.count;
    for (uint8_t i = 0; i < cnt; i++) {
      if (i == in.st.primary_index) continue;
      const CFSessionRowMsg &r = in.ss.rows[i];
      uint16_t c = dim ? C_GREY : stateColor(r.state);
      if (cold) {                                   // compact: dots only
        g->fillCircle(x + 5, 267, 5, c);
        x += 14;
      } else {
        g->fillRoundRect(x, 258, 56, 18, 9, c);
        char p[16]; cfCopyStr(p, sizeof p, r.project, sizeof r.project);
        if (!p[0]) { p[0] = DASH; p[1] = '\0'; }
        fit(F9, p, 46);
        textC(g, F9, x + 28, 271, p, C_BG);
        x += 62;
      }
      shown++;
    }
  }
  uint8_t listed = in.haveSessions ? (in.ss.count > 4 ? 4 : in.ss.count) : 0;
  if (in.st.session_count > 4 && in.st.session_count > listed) {
    char more[8]; snprintf(more, sizeof more, "+%u", (unsigned)(in.st.session_count - listed));
    text(g, F12, x + 2, 272, more, dim ? C_FAINT : C_DIM);
  }
  (void)shown;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
uint8_t cfVisualState(const CFDrawInput &in) {
  if (in.versionMismatch) return CF_VIS_MISMATCH;
  if (in.link != CF_LINK_LIVE || !in.haveState) return CF_VIS_OFFLINE;
  uint8_t s = in.st.global_state;
  return s <= CF_STATE_STALE ? s : (uint8_t)CF_STATE_NONE;
}

uint32_t cfTurnElapsed(const CFDrawInput &in) {
  if (!in.haveState) return 0;
  return cfTurnSeconds(in.st.p_state, in.st.p_turn_elapsed_s, in.rxAgeMs, linkLive(in));
}

CFRect cfRegionRect(uint8_t region) {
  switch (region) {
    case CF_RGN_STATUS:   { CFRect r = { 0, 0, 240, 48 };   return r; }
    case CF_RGN_HERO:     { CFRect r = { 0, 48, 240, 137 }; return r; }
    case CF_RGN_CENTRE:   { CFRect r = { CX - RDISC, CY - RDISC,
                                         2 * RDISC + 1, 2 * RDISC + 1 }; return r; }
    case CF_RGN_ACTIVITY: { CFRect r = { 0, 185, 240, 69 }; return r; }
    case CF_RGN_FOOTER:   { CFRect r = { 0, 254, 240, 26 }; return r; }
    default:              { CFRect r = { 0, 0, 240, 280 };  return r; }
  }
}

void cfDrawRegions(Arduino_GFX *g, const CFDrawInput &in, uint8_t regions) {
  if (!g) return;
  uint8_t vis = cfVisualState(in);
  if (regions & CF_RGN_STATUS)   drawStatus(g, in);
  if (regions & CF_RGN_HERO)     drawHero(g, in, vis);
  else if (regions & CF_RGN_CENTRE) drawCentre(g, in, vis);
  if (regions & CF_RGN_ACTIVITY) drawActivity(g, in, vis);
  if (regions & CF_RGN_FOOTER)   drawFooter(g, in, vis);
}

CFAnim cfAnimation(const CFDrawInput &in) {
  CFAnim a = { 0, 0 };
  switch (cfVisualState(in)) {
    // 70 ms (~14 fps) keeps the render task under the 66 ms threshold that
    // makes controller.cpp raise the CPU to 240 MHz.
    case CF_STATE_WORKING:
    case CF_STATE_COMPACTING: a.region = CF_RGN_CENTRE; a.frameMs = 70; break;
    case CF_STATE_BLOCKED:    a.region = CF_RGN_HERO;   a.frameMs = 100; break;
    default: break;
  }
  return a;
}

uint32_t cfRegionKey(const CFDrawInput &in, uint8_t region) {
  uint32_t h = 2166136261UL;
  uint8_t vis = cfVisualState(in);
  h = fnvU(h, region);
  switch (region) {
    case CF_RGN_STATUS:
      h = fnvU(h, in.rtcOk); h = fnvU(h, in.hour); h = fnvU(h, in.minute);
      h = fnvU(h, in.batOk); h = fnvU(h, in.batOk ? in.batPct : 0);
      h = fnvU(h, in.link); h = fnvU(h, in.versionMismatch);
      break;
    case CF_RGN_HERO:
    case CF_RGN_CENTRE: {
      h = fnvU(h, vis);
      if (vis == CF_VIS_MISMATCH) break;
      char cap[32]; heroCaption(in, vis, cap, sizeof cap); h = fnvStr(h, cap);
      if (vis == CF_STATE_LIMITED) { char d[12]; limitedCountdown(in, d, sizeof d); h = fnvStr(h, d); }
      if (in.haveState) {
        const CFStateMsg &s = in.st;
        h = fnvU(h, rlUnknown(in)); h = fnvU(h, rlStale(in));
        h = fnvU(h, s.five_used); h = fnvU(h, s.seven_used);
        h = fnvU(h, s.five_elapsed); h = fnvU(h, s.seven_elapsed);
        h = fnvU(h, (uint8_t)s.five_pace); h = fnvU(h, (uint8_t)s.seven_pace);
      }
      h = fnvU(h, in.link);
      break;
    }
    case CF_RGN_ACTIVITY:
      h = fnvU(h, vis == CF_VIS_MISMATCH ? 1 : vis == CF_VIS_OFFLINE ? 2 : 0);
      h = fnvU(h, in.haveState); h = fnvU(h, in.link);
      if (in.haveState) {
        const CFStateMsg &s = in.st;
        h = fnvU(h, s.primary_index); h = fnvU(h, s.p_state); h = fnvU(h, s.p_ctx);
        h = fnvU(h, s.subagent_count); h = fnvU(h, s.p_flags & CF_PFLAGS_HUNG);
        h = fnvU(h, s.p_tool_count);
        h = fnv(h, s.p_project, sizeof s.p_project);
        h = fnv(h, s.p_current_tool, sizeof s.p_current_tool);
        h = fnvU(h, cfTurnElapsed(in));
      }
      break;
    case CF_RGN_FOOTER: {
      h = fnvU(h, vis == CF_VIS_MISMATCH ? 1 : vis == CF_VIS_OFFLINE ? 2 : 0);
      h = fnvU(h, in.haveState); h = fnvU(h, in.haveSessions);
      if (in.haveState) { h = fnvU(h, in.st.session_count); h = fnvU(h, in.st.primary_index); }
      if (in.haveSessions) {
        h = fnvU(h, in.ss.count);
        for (int i = 0; i < 4; i++) {
          h = fnvU(h, in.ss.rows[i].state);
          h = fnv(h, in.ss.rows[i].project, sizeof in.ss.rows[i].project);
        }
      }
      char cache[28];
      if (cacheColdSoon(in, cache, sizeof cache)) h = fnvStr(h, cache);
      break;
    }
    default: break;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Detail page (long press)
// ---------------------------------------------------------------------------
namespace {
const char *effortName(uint8_t e) {
  switch (e) {
    case CF_EFFORT_LOW: return "low";   case CF_EFFORT_MEDIUM: return "medium";
    case CF_EFFORT_HIGH: return "high"; case CF_EFFORT_XHIGH:  return "xhigh";
    case CF_EFFORT_MAX: return "max";   default: return nullptr;
  }
}
const char *prName(uint8_t r) {
  switch (r) {
    case CF_PRREVIEW_PENDING: return "pending";
    case CF_PRREVIEW_APPROVED: return "approved";
    case CF_PRREVIEW_CHANGES_REQUESTED: return "changes req";
    case CF_PRREVIEW_COMMENTED: return "commented";
    case CF_PRREVIEW_DRAFT: return "draft";
    case CF_PRREVIEW_MERGED: return "merged";
    case CF_PRREVIEW_CLOSED: return "closed";
    case CF_PRREVIEW_OTHER: return "other";
    default: return "";
  }
}
void dash(char *o) { o[0] = DASH; o[1] = '\0'; }
void pctStr(char *o, size_t n, uint8_t p) { if (p == CF_PCT_UNKNOWN) dash(o); else snprintf(o, n, "%u%%", p); }
void centsStr(char *o, size_t n, uint16_t c) {
  if (c == CF_U16_UNKNOWN) dash(o);
  else snprintf(o, n, "$%u.%02u", (unsigned)(c / 100), (unsigned)(c % 100));
}
void resetStr(const CFDrawInput &in, char *o, size_t n, uint32_t at, bool withDay) {
  if (!at) { dash(o); return; }
  CFCivil c = cfCivilFromEpoch((int64_t)at + (int64_t)in.utcOffsetMin * 60);
  static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  if (withDay) snprintf(o, n, "%s %02u:%02u", wd[c.wday % 7], c.hour, c.minute);
  else         snprintf(o, n, "%02u:%02u", c.hour, c.minute);
}
void row(Arduino_GFX *g, int16_t y, const char *label, const char *val, uint16_t vc) {
  text(g, F12, 14, y, label, C_DIM);
  char v[32]; strncpy(v, val, sizeof v - 1); v[sizeof v - 1] = '\0';
  fit(F12, v, 140);
  textR(g, F12, 226, y, v, vc);
}
}  // namespace

void cfDrawDetail(Arduino_GFX *g, const CFDrawInput &in) {
  if (!g) return;
  g->fillScreen(C_BG);
  textC(g, F9, CX, 16, "CLAUDE DETAIL", C_ORANGE);
  g->drawFastHLine(20, 22, 200, C_TRACK);
  if (in.versionMismatch) {
    textC(g, F22, CX, 130, "UPDATE", C_RED);
    textC(g, F22, CX, 158, "BRIDGE", C_RED);
    return;
  }
  if (!in.haveState) {
    textC(g, F12, CX, 140, "no data from bridge", C_DIM);
    return;
  }
  const CFStateMsg &s = in.st;
  bool noRl = (s.flags & CF_FLAGS_NO_RATE_LIMITS) != 0;
  char v[40], a[16], b[16];
  int16_t y = 48;
  const int16_t dy = 23;

  // "46% | 17:27" -> '|' renders as a middle dot
  pctStr(a, sizeof a, noRl ? CF_PCT_UNKNOWN : s.five_used);
  resetStr(in, b, sizeof b, noRl ? 0 : s.five_resets_at, false);
  snprintf(v, sizeof v, "%s | %s", a, b);
  row(g, y, "5h", v, C_TEXT); y += dy;
  pctStr(a, sizeof a, noRl ? CF_PCT_UNKNOWN : s.seven_used);
  resetStr(in, b, sizeof b, noRl ? 0 : s.seven_resets_at, true);
  snprintf(v, sizeof v, "%s | %s", a, b);
  row(g, y, "7d", v, C_TEXT); y += dy;
  if (s.projected_hit) { resetStr(in, a, sizeof a, s.projected_hit, false); snprintf(v, sizeof v, "~%s", a); }
  else dash(v);
  row(g, y, "limit hit", v, s.projected_hit ? C_AMBER : C_TEXT); y += dy;

  cfCopyStr(v, sizeof v, s.p_model_name, sizeof s.p_model_name);
  if (!v[0]) dash(v);
  row(g, y, "model", v, C_TEXT); y += dy;
  const char *en = effortName(s.p_effort);
  snprintf(v, sizeof v, "%s%s%s", en ? en : "\x01",
           (s.p_flags & CF_PFLAGS_FAST_MODE) ? " fast" : "",
           (s.p_flags & CF_PFLAGS_THINKING) ? " think" : "");
  row(g, y, "effort", v, C_TEXT); y += dy;
  centsStr(v, sizeof v, s.p_cost_cents);
  row(g, y, "cost", v, C_TEXT); y += dy;
  centsStr(v, sizeof v, s.cost_today_cents);
  row(g, y, "today", v, C_TEXT); y += dy;
  snprintf(v, sizeof v, "+%u -%u", (unsigned)s.p_lines_added, (unsigned)s.p_lines_removed);
  row(g, y, "lines", v, C_TEXT); y += dy;
  if (s.p_pr_number) snprintf(v, sizeof v, "#%u %s", (unsigned)s.p_pr_number, prName(s.p_pr_review));
  else dash(v);
  row(g, y, "PR", v, C_TEXT); y += dy;
  pctStr(a, sizeof a, s.p_ctx);
  snprintf(v, sizeof v, "%s  %u tools", a, (unsigned)s.p_tool_count);
  row(g, y, "context", v, C_TEXT);
}
