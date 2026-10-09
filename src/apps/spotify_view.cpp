// SpotifyView — renders the Spotify service state and turns touch/gestures into
// transport commands. Draws on the render task only; the network lives in
// spotify.cpp. Album-art JPEGs are decoded here (on the render task) so the
// single-painter rule holds — the service just hands us the raw bytes.
#include <Arduino_GFX_Library.h>
#include <IPAddress.h>
#include <qrcode.h>
#include <JPEGDEC.h>
#include "spotify.h"
#include "display.h"     // gfx
#include "haptic.h"      // hapticBuzz
#include "model.h"       // model (wifi status / IP) + ModelLock

// ---------- geometry (240 x 280) ----------
static const int16_t W = 240, H = 280;
static const int16_t ART_X = 0,  ART_Y = 0,  ART_W = 240, ART_H = 180;
static const int16_t TITLE_Y = 188, ARTIST_Y = 210;
static const int16_t BAR_X = 12, BAR_Y = 234, BAR_W = 216, BAR_H = 6;
static const int16_t TIME_Y = 244;
static const int16_t BOT_Y = 256, BOT_H = 22;
static const int16_t BACK_W = 56, BACK_H = 46;   // top-left back hit corner

// ---------- shared local helpers ----------
static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
static int16_t txtCenterX(const char *s, uint8_t size) {
  return (W - (int16_t)strlen(s) * 6 * size) / 2;
}
static void fmtTime(uint32_t ms, char *buf, size_t n) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

// ---------- JPEG decode (album art) ----------
static JPEGDEC jpeg;
static int16_t gDstX, gDstY, gBoxX, gBoxY, gBoxW, gBoxH;

// Draw one decoded block, clipped + centered/cropped into the art box. Drawing
// per in-box row run keeps it to one bus window per row.
static int jpegDraw(JPEGDRAW *p) {
  for (int row = 0; row < p->iHeight; row++) {
    int dy = gDstY + p->y + row;
    if (dy < gBoxY || dy >= gBoxY + gBoxH) continue;
    int srcX0 = p->x;
    int colStart = 0, colEnd = p->iWidth;
    int dx0 = gDstX + srcX0;
    if (dx0 < gBoxX)                 colStart = gBoxX - dx0;
    if (dx0 + p->iWidth > gBoxX + gBoxW) colEnd = (gBoxX + gBoxW) - dx0;
    if (colStart >= colEnd) continue;
    gfx->draw16bitRGBBitmap(dx0 + colStart, dy,
                            &p->pPixels[row * p->iWidth + colStart],
                            colEnd - colStart, 1);
  }
  return 1;
}

// ===================================================================
void SpotifyView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw   = true;
  artDrawn    = false;
  lastArtGen  = 0xFFFFFFFF;
  shownTrackId[0] = '\0';
  shownAuth   = (SpotifyAuth)0xFF;
  lastBarPx   = -1;
  lastElapsed = 0xFFFFFFFF;
  lastCtrlMode = -1;
  volOverlayVal = -1;
  haveOptimistic = false;
  touchDown = false;
  swipedSinceDown = false;
  lastActionMs = 0;
  holdCount = 0;
  lastUpMs = 0;
  shownArtTrackId[0] = '\0';
  placeholderTrackId[0] = '\0';
  spotifyPostCmd(SpotifyCmd::Resync);   // get fresh state immediately
}

void SpotifyView::onExit() {}

// ---------- album art ----------
void SpotifyView::drawArtPlaceholder() {
  ThemeColors t = theme();
  gfx->fillRect(ART_X, ART_Y, ART_W, ART_H, t.bg);
  // simple music-note glyph in the accent colour
  int cx = ART_X + ART_W / 2, cy = ART_Y + ART_H / 2;
  gfx->fillCircle(cx - 14, cy + 20, 13, t.accent);
  gfx->fillRect(cx - 2, cy - 28, 5, 48, t.accent);
  gfx->fillRect(cx - 2, cy - 28, 22, 6, t.accent);
}

void SpotifyView::drawProgress(const SpotifySnapshot &s, bool full) {
  ThemeColors t = theme();
  uint32_t elapsed = s.progressMs;
  // Only advance locally while actually playing and online — a frozen bar is
  // the honest display when we've lost the connection.
  if (s.isPlaying && s.playback != SpotifyPlayback::Offline)
    elapsed += millis() - s.progressAnchorMs;
  if (s.durationMs && elapsed > s.durationMs) elapsed = s.durationMs;

  int barPx = (s.durationMs) ? (int)((uint64_t)elapsed * BAR_W / s.durationMs) : 0;
  if (barPx < 0) barPx = 0; if (barPx > BAR_W) barPx = BAR_W;

  if (full || barPx != lastBarPx) {
    gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, t.line);
    if (barPx > 0) gfx->fillRoundRect(BAR_X, BAR_Y, barPx, BAR_H, BAR_H / 2, t.accent);
    lastBarPx = barPx;
  }

  uint32_t elSec = elapsed / 1000;
  if (full || elSec != lastElapsed) {
    char a[8], b[8];
    fmtTime(elapsed, a, sizeof(a));
    fmtTime(s.durationMs, b, sizeof(b));
    gfx->fillRect(0, TIME_Y, W, 10, t.bg);
    gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(t.fg, t.bg);
    gfx->setCursor(BAR_X, TIME_Y); gfx->print(a);
    gfx->setCursor(BAR_X + BAR_W - (int)strlen(b) * 6, TIME_Y); gfx->print(b);
    lastElapsed = elSec;
  }
}

void SpotifyView::drawControls(const SpotifySnapshot &s) {
  ThemeColors t = theme();
  bool playing = haveOptimistic ? optimisticPlaying : s.isPlaying;
  int cy = BOT_Y + BOT_H / 2;

  bool offline = (s.playback == SpotifyPlayback::Offline);
  // Recently changed volume? Show a readout in the control band instead of the
  // transport icons; it reverts automatically when the overlay expires.
  bool volMode = !offline &&
                 (volOverlayVal >= 0 && (int32_t)(volOverlayUntil - millis()) > 0);

  // Delta guard: only repaint the band when its content actually changes —
  // render() runs ~20 Hz and an unconditional repaint would flicker the icons.
  int mode = offline ? 2 : (volMode ? 1 : 0);
  if (mode == lastCtrlMode && playing == lastCtrlPlaying &&
      s.premium == lastCtrlPremium &&
      (mode != 1 || (volOverlayVal == lastCtrlVol && s.volumeSupported == lastCtrlVolSup)))
    return;
  lastCtrlMode = mode; lastCtrlPlaying = playing;
  lastCtrlPremium = s.premium; lastCtrlVol = volOverlayVal; lastCtrlVolSup = s.volumeSupported;

  gfx->fillRect(0, BOT_Y, W, BOT_H, t.bg);
  gfx->setFont(nullptr);

  if (offline) {
    const char *m = "OFFLINE";
    gfx->setTextSize(2); gfx->setTextColor(t.line, t.bg);
    gfx->setCursor((W - (int)strlen(m) * 12) / 2, BOT_Y + 4); gfx->print(m);
    return;
  }

  if (volMode) {
    char buf[16];
    if (s.volumeSupported) snprintf(buf, sizeof(buf), "VOL %d%%", volOverlayVal);
    else                   snprintf(buf, sizeof(buf), "VOL N/A");
    gfx->setTextSize(2); gfx->setTextColor(s.volumeSupported ? t.accent : t.line, t.bg);
    gfx->setCursor((W - (int)strlen(buf) * 12) / 2, BOT_Y + 4); gfx->print(buf);
    return;
  }

  uint16_t ic = s.premium ? t.accent : t.line;   // dim controls if no Premium

  // prev (|<) and next (>|) chevrons
  gfx->fillTriangle(44, cy - 8, 44, cy + 8, 36, cy, ic);
  gfx->fillRect(45, cy - 8, 3, 16, ic);
  gfx->fillTriangle(196, cy - 8, 196, cy + 8, 204, cy, ic);
  gfx->fillRect(192, cy - 8, 3, 16, ic);

  // centre play / pause
  if (playing) {
    gfx->fillRect(112, cy - 9, 6, 18, ic);
    gfx->fillRect(123, cy - 9, 6, 18, ic);
  } else {
    gfx->fillTriangle(113, cy - 9, 113, cy + 9, 130, cy, ic);
  }
}

void SpotifyView::drawNowPlaying(const SpotifySnapshot &s) {
  ThemeColors t = theme();

  bool trackChanged = strncmp(s.trackId, shownTrackId, sizeof(shownTrackId)) != 0;

  // ----- album art -----
  // Decode the cached JPEG when a newer image arrived for the current track, or
  // when the painted art is for a different track (e.g. resuming after idle).
  // Otherwise leave the art up. With no art yet, paint a placeholder once.
  uint32_t gen = spotifyArtGen();
  bool artMatches = s.trackId[0] && s.artTrackId[0] &&
                    strncmp(s.artTrackId, s.trackId, sizeof(s.trackId)) == 0;
  bool realShown  = s.trackId[0] &&
                    strncmp(shownArtTrackId, s.trackId, sizeof(shownArtTrackId)) == 0;

  if (artMatches && (gen != lastArtGen || !realShown)) {
    size_t len = 0;
    const uint8_t *buf = spotifyArtLock(&len);   // holds artMutex only if non-null
    if (buf) {
      if (jpeg.openRAM((uint8_t *)buf, len, jpegDraw)) {
        // Native little-endian uint16_t to pair with draw16bitRGBBitmap. If album
        // art comes out colour-swapped (red/blue), switch this to
        // RGB565_BIG_ENDIAN and use draw16bitBeRGBBitmap in jpegDraw().
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        int sw = jpeg.getWidth(), sh = jpeg.getHeight();
        int scale = 1, opt = 0;
        if (sw >= ART_W * 4)      { opt = JPEG_SCALE_QUARTER; scale = 4; }
        else if (sw >= ART_W * 2) { opt = JPEG_SCALE_HALF;    scale = 2; }
        int ew = sw / scale, eh = sh / scale;
        gBoxX = ART_X; gBoxY = ART_Y; gBoxW = ART_W; gBoxH = ART_H;
        gDstX = ART_X + (ART_W - ew) / 2;
        gDstY = ART_Y + (ART_H - eh) / 2;
        gfx->fillRect(ART_X, ART_Y, ART_W, ART_H, t.bg);   // letterbox fill
        jpeg.decode(0, 0, opt);
        jpeg.close();
        strncpy(shownArtTrackId, s.trackId, sizeof(shownArtTrackId));
        shownArtTrackId[sizeof(shownArtTrackId) - 1] = '\0';
        placeholderTrackId[0] = '\0';
      }
      spotifyArtUnlock();
    }
    lastArtGen = gen;
  } else if (!artMatches && !realShown &&
             strncmp(placeholderTrackId, s.trackId, sizeof(placeholderTrackId)) != 0) {
    drawArtPlaceholder();     // no art yet for this track
    strncpy(placeholderTrackId, s.trackId, sizeof(placeholderTrackId));
    placeholderTrackId[sizeof(placeholderTrackId) - 1] = '\0';
    shownArtTrackId[0] = '\0';
  }

  // ----- title + artist (redraw on track change) -----
  if (trackChanged || firstDraw) {
    gfx->setFont(nullptr);
    gfx->fillRect(0, TITLE_Y - 2, W, ARTIST_Y - TITLE_Y + 18, t.bg);
    gfx->setTextSize(2); gfx->setTextColor(t.fg, t.bg);
    const char *title = s.track[0] ? s.track : "(nothing playing)";
    int tw = (int)strlen(title) * 12;
    int tx = tw <= W - 8 ? (W - tw) / 2 : 4;
    gfx->setCursor(tx, TITLE_Y); gfx->print(title);

    gfx->setTextSize(2); gfx->setTextColor(t.line, t.bg);
    int aw = (int)strlen(s.artist) * 12;
    int ax = aw <= W - 8 ? (W - aw) / 2 : 4;
    gfx->setCursor(ax, ARTIST_Y); gfx->print(s.artist);

    strncpy(shownTrackId, s.trackId, sizeof(shownTrackId));
    shownTrackId[sizeof(shownTrackId) - 1] = '\0';
    lastBarPx = -1; lastElapsed = 0xFFFFFFFF;   // force progress repaint
  }

  drawProgress(s, trackChanged || firstDraw);
  drawControls(s);

  // clear optimistic echo once the server agrees
  if (haveOptimistic && s.isPlaying == optimisticPlaying) haveOptimistic = false;
}

// ---------- setup / auth screens ----------
static void deviceIp(char *out, size_t n) {
  uint32_t ip; bool conn;
  { ModelLock lk; ip = model.wifiIpV4; conn = model.wifiConnected; }
  if (conn && ip) { IPAddress a(ip); snprintf(out, n, "%s", a.toString().c_str()); }
  else            { snprintf(out, n, "ewatch.local"); }
}

// QR byte-mode data capacity (bytes) per version at ECC level L (index = version).
// The QRCode library does NOT bounds-check data length against the chosen version
// — see its source "@TODO: Return error if data is too big" — and overflows a
// stack VLA if the version is too small. So we MUST pick a fitting version up
// front and call qrcode_initText exactly once; never probe undersized versions.
static const uint16_t kQrByteCapL[] = {
  0,                                                    // [0] unused
  17,  32,  53,  78,  106, 134, 154, 192, 230, 271,     // v1..v10
  321, 367, 425, 458, 523, 586, 644, 718, 792, 858      // v11..v20
};
static const int kQrMaxVer = 20;   // qrBuf below is sized for v20 (1177 bytes)

static int qrVersionFor(size_t len) {
  for (int v = 1; v <= kQrMaxVer; v++) if (kQrByteCapL[v] >= len) return v;
  return 0;   // too long for our max version / buffer
}

static void drawQr(const char *text) {
  static uint8_t qrBuf[1200];   // >= qrcode_getBufferSize(20) == 1177
  QRCode qr;
  ThemeColors t = theme();
  int ver = qrVersionFor(strlen(text));
  Serial.printf("SPOT: QR len=%u -> ver=%d\n", (unsigned)strlen(text), ver);
  if (ver == 0 || qrcode_initText(&qr, qrBuf, ver, ECC_LOW, text) != 0) {
    gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(RED, t.bg);
    gfx->setCursor(10, 150); gfx->print("auth URL too long for QR");
    return;
  }
  int avail = 176;
  int scale = avail / qr.size; if (scale < 1) scale = 1;
  int dim = qr.size * scale;
  int ox = (W - dim) / 2, oy = 54;
  gfx->fillRect(ox - 6, oy - 6, dim + 12, dim + 12, WHITE);   // quiet zone
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        gfx->fillRect(ox + x * scale, oy + y * scale, scale, scale, BLACK);
}

void SpotifyView::drawSetup(const SpotifySnapshot &s) {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawBackButton();
  gfx->setFont(nullptr); gfx->setTextColor(t.fg, t.bg); gfx->setTextSize(2);
  const char *title = "Spotify";
  gfx->setCursor(txtCenterX(title, 2), 12); gfx->print(title);
  gfx->drawFastHLine(20, 40, 200, t.line);

  char ip[40]; deviceIp(ip, sizeof(ip));
  gfx->setTextSize(1); gfx->setTextColor(t.fg, t.bg);

  auto line = [&](int y, const char *str) { gfx->setCursor(12, y); gfx->print(str); };

  switch (s.auth) {
    case SpotifyAuth::NeedWifi:
      line(70,  "WiFi not connected.");
      line(90,  "Settings > WiFi: enable,");
      line(104, "set Client mode, and join");
      line(118, "your network. Then return.");
      break;
    case SpotifyAuth::NeedClientId:
      line(64,  "No Spotify Client ID yet.");
      line(84,  "On a browser, open:");
      gfx->setTextColor(t.accent, t.bg);
      gfx->setCursor(12, 100); gfx->print("http://"); gfx->print(ip);
      gfx->setTextColor(t.fg, t.bg);
      line(122, "and paste your Client ID");
      line(136, "(Spotify Developer app).");
      line(160, "Redirect URI to register:");
      gfx->setTextColor(t.accent, t.bg);
      line(174, "http://127.0.0.1:8888/callback");
      break;
    case SpotifyAuth::NeedAuth: {
      char url[600]; size_t n = spotifyAuthorizeUrl(url, sizeof(url));
      if (n == 0) { line(120, "preparing sign-in..."); break; }
      drawQr(url);
      // the "scan + paste" caption is drawn under the QR by render()
      break;
    }
    case SpotifyAuth::Authorizing:
      line(120, "Signing in...");
      break;
    case SpotifyAuth::AuthError:
      gfx->setTextColor(RED, t.bg);
      line(80,  "Sign-in failed.");
      gfx->setTextColor(t.fg, t.bg);
      line(104, "Open the web page again and");
      line(118, "re-scan / paste a fresh code.");
      break;
    default: break;
  }
}

// ===================================================================
// subtle back chevron over the art top-left corner (drawn on top of the art)
static void drawBackChevron() {
  ThemeColors t = theme();
  gfx->fillTriangle(18, 12, 18, 28, 9, 20, contrastFor(t.bg));
}

void SpotifyView::render() {
  if (!gfx) return;
  SpotifySnapshot s; spotifyGetSnapshot(&s);

  // ---------- setup / auth screens ----------
  if (s.auth != SpotifyAuth::Ready) {
    if (s.auth != shownAuth) {
      drawSetup(s);
      shownAuth = s.auth;
      firstDraw = true;             // force a clean now-playing repaint later
      artDrawn  = false;            // here: "QR + caption painted" latch
    }
    // NeedAuth: the authorize URL is built asynchronously by the service task;
    // (re)draw the QR + caption once it's available.
    if (s.auth == SpotifyAuth::NeedAuth && !artDrawn) {
      char url[600];
      if (spotifyAuthorizeUrl(url, sizeof(url)) > 0) {
        drawSetup(s);
        ThemeColors t = theme();
        gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(t.fg, t.bg);
        char ip[40]; deviceIp(ip, sizeof(ip));
        gfx->setCursor(6, H - 30); gfx->print("Scan, approve, then paste the");
        gfx->setCursor(6, H - 18); gfx->print("code at http://"); gfx->print(ip);
        artDrawn = true;
      }
    }
    return;
  }

  // ---------- Ready: now playing ----------
  if (shownAuth != SpotifyAuth::Ready) {
    ThemeColors t = theme();
    gfx->fillScreen(t.bg);
    drawArtPlaceholder();
    shownAuth   = SpotifyAuth::Ready;
    firstDraw   = true;
    artDrawn    = false;
    lastArtGen  = 0xFFFFFFFF;
    lastCtrlMode = -1;
    lastBarPx   = -1;
    lastElapsed = 0xFFFFFFFF;
    shownTrackId[0] = '\0';
    shownArtTrackId[0] = '\0';
    placeholderTrackId[0] = '\0';
  }

  if (s.playback == SpotifyPlayback::Idle) {
    if (firstDraw || strncmp(shownTrackId, "<idle>", 6) != 0) {
      ThemeColors t = theme();
      drawArtPlaceholder();
      gfx->fillRect(0, TITLE_Y - 2, W, H - TITLE_Y, t.bg);
      gfx->setFont(nullptr); gfx->setTextSize(2); gfx->setTextColor(t.fg, t.bg);
      const char *m = "Nothing playing";
      gfx->setCursor(txtCenterX(m, 2), TITLE_Y); gfx->print(m);
      gfx->setTextSize(1); gfx->setTextColor(t.line, t.bg);
      const char *m2 = "Start playback on a device";
      gfx->setCursor(txtCenterX(m2, 1), ARTIST_Y + 4); gfx->print(m2);
      strncpy(shownTrackId, "<idle>", sizeof(shownTrackId));
      // the idle screen overwrote the art area — drop the art ownership so a
      // resume re-decodes the cached image instead of trusting a stale paint.
      shownArtTrackId[0] = '\0';
      placeholderTrackId[0] = '\0';
      firstDraw = false;
    }
    drawBackChevron();
    return;
  }

  drawNowPlaying(s);
  drawBackChevron();
  firstDraw = false;
}

// Predict the resulting volume from the last shown value (so rapid swipes
// accumulate), show it in the control band, and command it absolutely.
void SpotifyView::adjustVolume(const SpotifySnapshot &s, int delta) {
  bool overlayLive = (volOverlayVal >= 0 && (int32_t)(volOverlayUntil - millis()) > 0);
  int base = overlayLive ? volOverlayVal : (s.hasVolume ? s.volumePct : 50);
  base += delta;
  if (base < 0) base = 0; if (base > 100) base = 100;
  volOverlayVal = base;
  volOverlayUntil = millis() + 1300;
  hapticBuzz(50, 35);
  Serial.printf("SPOT: volume swipe %+d -> %d%% (hasVol=%d)\n", delta, base, s.hasVolume);
  spotifyPostCmd(SpotifyCmd::VolumeSet, (int16_t)base);
}

// ===================================================================
void SpotifyView::onEvent(const Event &e) {
  // hardware button is always "back"
  if (e.type == EventType::ButtonShort) { switchTo(Screen::AppList); return; }

  SpotifySnapshot s; spotifyGetSnapshot(&s);

  // On the setup screens only the back corner / button do anything.
  if (s.auth != SpotifyAuth::Ready) {
    if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
      switchTo(Screen::AppList); return;
    }
    if (e.type == EventType::Touch) {        // tap to re-check (e.g. WiFi came up)
      spotifyPostCmd(SpotifyCmd::Resync);
    }
    return;
  }

  // ----- Ready: transport -----
  // Anti-noise: USB-charging / EM interference can trip the CST816S with
  // phantom touches and gestures (see main.cpp). We therefore require evidence
  // of a real, *sustained* finger (>=N TouchHolds, ~30ms each) before acting,
  // plus a global rate-limit. A brief noise blip produces a Touch/TouchUp with
  // no holds and is rejected.
  static const uint16_t HOLD_FOR_TAP   = 3;   // ~90ms finger contact for a tap
  static const uint16_t HOLD_FOR_SWIPE = 2;   // ~60ms for a swipe
  static const uint32_t ACTION_DEBOUNCE = 450;

  switch (e.type) {
    case EventType::Touch:
      downX = e.x; downY = e.y; downMs = millis();
      touchDown = true; swipedSinceDown = false; holdCount = 0;
      return;

    case EventType::TouchHold:
      if (touchDown) holdCount++;
      return;

    case EventType::Gesture: {
      // Honour a swipe only if it was backed by a sustained finger that is
      // still (or was just) down — phantom gestures have no such sequence.
      bool realFinger = (touchDown || (millis() - lastUpMs) < 250) &&
                        holdCount >= HOLD_FOR_SWIPE;
      if (!realFinger) return;
      if (millis() - lastActionMs < ACTION_DEBOUNCE) return;
      bool acted = true;
      switch (e.gesture) {                  // flipped: left = next, right = prev
        case Gesture::SwipeLeft:  hapticBuzz(60, 45); spotifyPostCmd(SpotifyCmd::Next); break;
        case Gesture::SwipeRight: hapticBuzz(60, 45); spotifyPostCmd(SpotifyCmd::Prev); break;
        case Gesture::SwipeUp:    adjustVolume(s, +10); break;
        case Gesture::SwipeDown:  adjustVolume(s, -10); break;
        default: acted = false; break;      // SingleTap/DoubleTap/LongPress
      }
      if (acted) { swipedSinceDown = true; lastActionMs = millis(); }
      return;
    }

    case EventType::TouchUp: {
      lastUpMs = millis();
      bool wasDown = touchDown; uint16_t hc = holdCount;
      touchDown = false;
      if (!wasDown || swipedSinceDown) return;
      if (hc < HOLD_FOR_TAP) return;                      // not a sustained press
      int dx = (int)e.x - (int)downX, dy = (int)e.y - (int)downY;
      if (dx * dx + dy * dy > 30 * 30) return;            // moved -> swipe/drag
      if (millis() - downMs > 1500) return;               // very long hold -> ignore
      if (millis() - lastActionMs < ACTION_DEBOUNCE) return;
      lastActionMs = millis();

      uint16_t x = downX, y = downY;
      if (inRect(x, y, 0, 0, BACK_W, BACK_H)) { switchTo(Screen::AppList); return; }
      if (y >= BOT_Y - 6) {                               // bottom transport row
        if (x < 80)  { hapticBuzz(60, 45); spotifyPostCmd(SpotifyCmd::Prev); return; }
        if (x > 160) { hapticBuzz(60, 45); spotifyPostCmd(SpotifyCmd::Next); return; }
      }
      hapticBuzz(80, 55);                                 // centre / art -> play/pause
      optimisticPlaying = !(haveOptimistic ? optimisticPlaying : s.isPlaying);
      haveOptimistic = true;
      spotifyPostCmd(SpotifyCmd::PlayPause);
      return;
    }

    default: return;
  }
}
