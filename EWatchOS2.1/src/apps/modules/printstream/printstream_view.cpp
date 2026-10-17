// printstream — on-watch UI. Two pages (Camera / Stats) toggled by swipe
// up/down. Control actions (pause/resume/home) each pop a CONFIRM modal before
// a command is ever posted to the printer. JPEG camera frames are decoded on
// the render task (here), exactly like Spotify_Controller's album art.
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <string.h>
#include "printstream.h"
#include "display.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "model.h"
#include "view.h"

// ---- layout ----
static const int CAM_X = 0,  CAM_Y = 0,   CAM_W = 240, CAM_H = 256;
static const int STRIP_Y = 256, STRIP_H = 24;

// stats content region (below the title bar)
static const int CONTENT_Y = 50;

// control buttons
static const int BTN_Y = 182, BTN_H = 46, BTN_W = 70;
static const int BTN_PAUSE_X = 10, BTN_RESUME_X = 85, BTN_HOME_X = 160;

// ---- JPEG decode plumbing (render task only) ----
static JPEGDEC jpeg;
static int16_t gDstX, gDstY, gBoxX, gBoxY, gBoxW, gBoxH;

static int jpegDraw(JPEGDRAW *p) {
  for (int row = 0; row < p->iHeight; row++) {
    int dy = gDstY + p->y + row;
    if (dy < gBoxY || dy >= gBoxY + gBoxH) continue;
    int dx0 = gDstX + p->x;
    int colStart = 0, colEnd = p->iWidth;
    if (dx0 < gBoxX)                       colStart = gBoxX - dx0;
    if (dx0 + p->iWidth > gBoxX + gBoxW)   colEnd   = (gBoxX + gBoxW) - dx0;
    if (colStart >= colEnd) continue;
    gfx->draw16bitRGBBitmap(dx0 + colStart, dy,
                            &p->pPixels[row * p->iWidth + colStart],
                            colEnd - colStart, 1);
  }
  return 1;
}

// ---- helpers ----
static const char *stateText(PsState s) {
  switch (s) {
    case PsState::Standby:   return "Standby";
    case PsState::Printing:  return "Printing";
    case PsState::Paused:    return "Paused";
    case PsState::Complete:  return "Complete";
    case PsState::Cancelled: return "Cancelled";
    case PsState::Error:     return "Error";
    default:                 return "--";
  }
}
static uint16_t stateColor(PsState s, const ThemeColors &t) {
  switch (s) {
    case PsState::Printing:  return GREEN;
    case PsState::Paused:    return YELLOW;
    case PsState::Error:
    case PsState::Cancelled: return RED;
    case PsState::Complete:  return CYAN;
    default:                 return t.fg;
  }
}

// draw a small label, truncating to fit a pixel width (built-in 6x8 * size)
static void drawClipped(int x, int y, const char *s, uint8_t sz,
                        uint16_t fg, uint16_t bg, int maxW) {
  gfx->setFont(nullptr);
  gfx->setTextSize(sz);
  gfx->setTextColor(fg, bg);
  int charW = 6 * sz;
  int maxChars = maxW / charW;
  char buf[48];
  int n = strlen(s);
  if (n > maxChars) {
    if (maxChars > 3) { strncpy(buf, s, maxChars - 1); buf[maxChars - 1] = '\0';
      strncat(buf, "~", sizeof(buf) - strlen(buf) - 1); }
    else { strncpy(buf, s, maxChars); buf[maxChars] = '\0'; }
    s = buf;
  }
  gfx->setCursor(x, y);
  gfx->print(s);
}

// ===================================================================
void PrintStreamView::onEnter() {
  printstreamEnsureStarted();   // OS2.1: lazy, idempotent background-task spawn
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw    = true;
  confirm      = Confirm::None;
  lastCamGen   = 0xFFFFFFFF;
  lastCamState = (PsCamState)0xFF;
  shownLink    = (PsLink)0xFF;
  shownState   = (PsState)0xFF;
  shownPct     = -1;
  shownNozzle  = -1000; shownBed = -1000;
  shownFile[0] = '\0';
  psSetCameraWanted(page == Page::Camera);
  printstreamPostCmd(PsCmd::Resync);
}

void PrintStreamView::onExit() {
  psSetCameraWanted(false);
}

// ---- camera page ----
void PrintStreamView::drawCamera(const PsSnapshot &s, bool full) {
  ThemeColors t = theme();

  if (full) gfx->fillScreen(t.bg);

  uint32_t gen = psCamGen();
  bool newFrame = (gen != lastCamGen);

  if (newFrame && (s.cam == PsCamState::Streaming || s.cam == PsCamState::SnapshotOk)) {
    size_t len = 0;
    const uint8_t *buf = psCamLock(&len);   // holds camMutex if non-null
    if (buf && len > 64) {
      if (jpeg.openRAM((uint8_t *)buf, len, jpegDraw)) {
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        int sw = jpeg.getWidth(), sh = jpeg.getHeight();
        int scale = 1, opt = 0;
        if      (sw >= CAM_W * 8) { opt = JPEG_SCALE_EIGHTH;  scale = 8; }
        else if (sw >= CAM_W * 4) { opt = JPEG_SCALE_QUARTER; scale = 4; }
        else if (sw >= CAM_W * 2) { opt = JPEG_SCALE_HALF;    scale = 2; }
        int ew = sw / scale, eh = sh / scale;
        gBoxX = CAM_X; gBoxY = CAM_Y; gBoxW = CAM_W; gBoxH = CAM_H;
        gDstX = CAM_X + (CAM_W - ew) / 2;
        gDstY = CAM_Y + (CAM_H - eh) / 2;
        jpeg.decode(0, 0, opt);
        jpeg.close();
      }
    }
    psCamUnlock();
    lastCamGen = gen;
  }

  // status strip (redraw on state/pct/cam change or full repaint)
  bool stripDirty = full || s.cam != lastCamState ||
                    (int)s.state != shownState2_ || (int)s.progressPct != shownPct2_;
  if (s.cam == PsCamState::Connecting || s.cam == PsCamState::Unavailable || s.cam == PsCamState::Off) {
    if (newFrame || full || s.cam != lastCamState) {
      gfx->fillRect(CAM_X, CAM_Y, CAM_W, CAM_H, t.bg);
      const char *msg = (s.cam == PsCamState::Connecting) ? "Connecting camera..."
                       : (s.cam == PsCamState::Off)        ? "Camera paused"
                       : "Camera unavailable";
      drawClipped(24, CAM_H / 2 - 4, msg, 1, t.fg, t.bg, CAM_W - 40);
      drawClipped(24, CAM_H / 2 + 12, "(stats on swipe down)", 1, t.line, t.bg, CAM_W - 40);
    }
  }

  if (stripDirty) {
    gfx->fillRect(0, STRIP_Y, 240, STRIP_H, t.accent);
    char line[40];
    snprintf(line, sizeof(line), "%s  %u%%", stateText(s.state), s.progressPct);
    uint16_t fg = contrastFor(t.accent);
    drawClipped(8, STRIP_Y + 8, line, 1, fg, t.accent, 150);
    drawClipped(150, STRIP_Y + 8, "up:stats", 1, fg, t.accent, 88);
    lastCamState = s.cam;
    shownState2_ = (int)s.state;
    shownPct2_   = (int)s.progressPct;
  }
}

// ---- stats page ----
void PrintStreamView::drawStats(const PsSnapshot &s, bool full) {
  ThemeColors t = theme();

  if (full) {
    gfx->fillScreen(t.bg);
    drawTitleBar("Printer");
    drawBackButton();
  }

  bool changed = full || s.link != shownLink || s.state != shownState ||
                 (int)s.progressPct != shownPct ||
                 (int)s.nozzleTemp != shownNozzle || (int)s.bedTemp != shownBed ||
                 strncmp(s.filename, shownFile, sizeof(shownFile)) != 0;

  if (changed) {
    // wipe content region (keep title + buttons area; buttons redrawn below)
    gfx->fillRect(0, CONTENT_Y, 240, BTN_Y - CONTENT_Y - 4, t.bg);

    if (s.link == PsLink::NeedWifi) {
      drawClipped(16, 70, "WiFi not connected.", 1, t.fg, t.bg, 220);
      drawClipped(16, 90, "Enable WiFi (client) in", 1, t.line, t.bg, 220);
      drawClipped(16, 104, "Settings to reach the printer.", 1, t.line, t.bg, 220);
    } else if (s.link == PsLink::Connecting) {
      drawClipped(16, 80, "Connecting to Moonraker...", 1, t.fg, t.bg, 220);
    } else {
      // state line
      gfx->setFont(nullptr); gfx->setTextSize(2);
      gfx->setTextColor(stateColor(s.state, t), t.bg);
      gfx->setCursor(16, 56); gfx->print(stateText(s.state));
      if (s.link == PsLink::Offline) {
        drawClipped(150, 60, "(offline)", 1, RED, t.bg, 88);
      }
      // filename
      const char *fn = s.filename[0] ? s.filename : "(no job)";
      drawClipped(16, 82, fn, 1, t.fg, t.bg, 220);

      // progress bar
      int bx = 16, by = 100, bw = 208, bh = 18;
      gfx->drawRect(bx, by, bw, bh, t.line);
      int fillW = (bw - 2) * s.progressPct / 100;
      if (fillW > 0) gfx->fillRect(bx + 1, by + 1, fillW, bh - 2, t.accent);
      if (fillW < bw - 2) gfx->fillRect(bx + 1 + fillW, by + 1, (bw - 2) - fillW, bh - 2, t.bg);
      char pb[8]; snprintf(pb, sizeof(pb), "%u%%", s.progressPct);
      gfx->setTextSize(1); gfx->setTextColor(t.fg, t.bg);
      gfx->setCursor(bx + bw / 2 - strlen(pb) * 3, by + 24); gfx->print(pb);

      // temps
      char tn[28], tbed[28];
      snprintf(tn,   sizeof(tn),   "Nozzle %d/%d C", (int)s.nozzleTemp, (int)s.nozzleTarget);
      snprintf(tbed, sizeof(tbed), "Bed    %d/%d C", (int)s.bedTemp,    (int)s.bedTarget);
      drawClipped(16, 140, tn,   1, t.fg, t.bg, 220);
      drawClipped(16, 156, tbed, 1, t.fg, t.bg, 220);
    }

    shownLink = s.link; shownState = s.state; shownPct = s.progressPct;
    shownNozzle = (int)s.nozzleTemp; shownBed = (int)s.bedTemp;
    strncpy(shownFile, s.filename, sizeof(shownFile)); shownFile[sizeof(shownFile) - 1] = '\0';
  }

  // control buttons (static chrome; draw on full repaint)
  if (full) {
    auto btn = [&](int x, const char *label, uint16_t col) {
      gfx->fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 6, col);
      uint16_t fg = contrastFor(col);
      int tw = strlen(label) * 6;
      gfx->setFont(nullptr); gfx->setTextSize(1); gfx->setTextColor(fg, col);
      gfx->setCursor(x + (BTN_W - tw) / 2, BTN_Y + BTN_H / 2 - 4);
      gfx->print(label);
    };
    btn(BTN_PAUSE_X,  "Pause",  YELLOW);
    btn(BTN_RESUME_X, "Resume", GREEN);
    btn(BTN_HOME_X,   "Home",   t.accent);
    drawClipped(16, 244, "swipe up: camera", 1, t.line, t.bg, 220);
  }
}

// ---- confirm modal ----
void PrintStreamView::drawConfirmOverlay() {
  ThemeColors t = theme();
  const char *q = confirm == Confirm::Pause  ? "Pause print?"
                : confirm == Confirm::Resume ? "Resume print?"
                : "Home all axes (G28)?";
  int mx = 20, my = 90, mw = 200, mh = 110;
  gfx->fillRoundRect(mx, my, mw, mh, 8, t.bg);
  gfx->drawRoundRect(mx, my, mw, mh, 8, t.accent);
  drawClipped(mx + 14, my + 16, q, 1, t.fg, t.bg, mw - 28);
  // CONFIRM (accent) / CANCEL (line)
  gfx->fillRoundRect(mx + 14, my + 48, 80, 44, 6, t.accent);
  gfx->fillRoundRect(mx + 106, my + 48, 80, 44, 6, DARKGREY);
  uint16_t cf = contrastFor(t.accent);
  gfx->setFont(nullptr); gfx->setTextSize(1);
  gfx->setTextColor(cf, t.accent); gfx->setCursor(mx + 14 + 16, my + 48 + 18); gfx->print("CONFIRM");
  gfx->setTextColor(WHITE, DARKGREY); gfx->setCursor(mx + 106 + 22, my + 48 + 18); gfx->print("CANCEL");
}
void PrintStreamView::clearConfirm() {
  confirm = Confirm::None;
  firstDraw = true;          // force a clean repaint of whatever page we're on
}

// ===================================================================
void PrintStreamView::render() {
  if (!gfx) return;
  PsSnapshot s; printstreamGetSnapshot(&s);

  bool full = firstDraw;
  firstDraw = false;

  if (page == Page::Camera) drawCamera(s, full);
  else                      drawStats(s, full);

  if (confirm != Confirm::None) drawConfirmOverlay();
}

// ===================================================================
void PrintStreamView::onEvent(const Event &e) {
  // Back: hardware button or right-swipe. If a confirm is open, back cancels it.
  if (e.type == EventType::ButtonShort) {
    if (confirm != Confirm::None) { clearConfirm(); haptic(HapticRole::Back); return; }
    switchTo(Screen::AppList); return;
  }

  // Confirm modal is modal: it eats taps.
  if (confirm != Confirm::None) {
    if (e.type == EventType::Touch) {
      int mx = 20, my = 90;
      if (inRect(e.x, e.y, mx + 14, my + 48, 80, 44)) {        // CONFIRM
        PsCmd c = confirm == Confirm::Pause  ? PsCmd::Pause
                : confirm == Confirm::Resume ? PsCmd::Resume : PsCmd::Home;
        printstreamPostCmd(c);
        haptic(HapticRole::Confirm);
        clearConfirm();
      } else if (inRect(e.x, e.y, mx + 106, my + 48, 80, 44)) { // CANCEL
        haptic(HapticRole::Back);
        clearConfirm();
      }
    }
    return;
  }

  // Page toggle via vertical swipe.
  if (e.type == EventType::Gesture) {
    if (e.gesture == Gesture::SwipeUp && page == Page::Stats) {
      page = Page::Camera; psSetCameraWanted(true); firstDraw = true; haptic(HapticRole::Scroll); return;
    }
    if (e.gesture == Gesture::SwipeDown && page == Page::Camera) {
      page = Page::Stats; psSetCameraWanted(false); firstDraw = true; haptic(HapticRole::Scroll); return;
    }
  }

  if (e.type == EventType::Touch) {
    if (page == Page::Stats) {
      if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
      // control buttons -> arm a confirm
      if (inRect(e.x, e.y, BTN_PAUSE_X, BTN_Y, BTN_W, BTN_H)) {
        confirm = Confirm::Pause; haptic(HapticRole::Tap); firstDraw = false; drawConfirmOverlay(); return;
      }
      if (inRect(e.x, e.y, BTN_RESUME_X, BTN_Y, BTN_W, BTN_H)) {
        confirm = Confirm::Resume; haptic(HapticRole::Tap); drawConfirmOverlay(); return;
      }
      if (inRect(e.x, e.y, BTN_HOME_X, BTN_Y, BTN_W, BTN_H)) {
        confirm = Confirm::Home; haptic(HapticRole::Tap); drawConfirmOverlay(); return;
      }
    } else {
      // camera page: tapping the bottom strip returns to stats
      if (e.y >= STRIP_Y) { page = Page::Stats; psSetCameraWanted(false); firstDraw = true; haptic(HapticRole::Scroll); return; }
    }
  }
}
