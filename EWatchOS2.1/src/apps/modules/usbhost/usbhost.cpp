// UsbHostView — see usbhost.h. The USB host stack itself is the shared
// usb_kbd_host service; this screen is a 30 s scan window + device list +
// typed-text box.
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>

#include "usbhost.h"
#include "usb_kbd_host.h"
#include "display.h"
#include "haptic.h"
#include "haptic_roles.h"
#include "ewlog.h"

static const int16_t W = 240, H = 280;
static const int16_t LIST_Y = 92, ROW_H = 46;
static const int     ROWS = (H - LIST_Y) / ROW_H;   // 4
static const int     MAX_DEV = USBH_MAX_DEV;
static const uint32_t WINDOW_MS = 30000;

// --------------------------------------------------------------------------

static const char *stateText(UsbhSt s) {
  switch (s) {
    case UsbhSt::Starting: return "Starting host...";
    case UsbhSt::Scanning: return "Host mode";
    case UsbhSt::Stopping: return "Stopping...";
    case UsbhSt::Done:     return "Scan finished";
    case UsbhSt::Failed:   return "Host failed";
    default:           return "Opening host in a moment";
  }
}

void UsbHostView::drawStatus() {
  ThemeColors t = theme();
  gfx->fillRect(0, 50, W, LIST_Y - 52, t.bg);
  gfx->setTextSize(1);
  UsbhSt s = usbhState();
  uint16_t dot = s == UsbhSt::Scanning ? GREEN : s == UsbhSt::Failed ? RED : ORANGE;
  gfx->fillCircle(14, 60, 5, dot);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(26, 56);
  gfx->print(stateText(s));
  if (s == UsbhSt::Scanning) {
    if (usbhKbdUp()) gfx->print(" - keyboard");
    else {
      int secs = max(0, usbhSecsLeft());
      char b[24]; snprintf(b, sizeof(b), " - %d s left", secs);
      gfx->print(b);
    }
  }
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(26, 70);
  if (s == UsbhSt::Failed)        gfx->print(usbhError());
  else if (s == UsbhSt::Scanning) gfx->print("Side button: stop (serial back)");
  else                        gfx->print("Tap to scan again (30 s)");
}

void UsbHostView::drawList() {
  ThemeColors t = theme();
  gfx->fillRect(0, LIST_Y, W, H - LIST_Y, t.bg);
  gfx->drawFastHLine(20, LIST_Y - 4, 200, t.line);
  UsbDev snap[MAX_DEV];
  int n = usbhSnapshot(snap, MAX_DEV);
  gfx->setTextSize(1);
  if (n == 0) {
    gfx->setTextColor(t.line, t.bg);
    gfx->setCursor(16, LIST_Y + 20); gfx->print("No devices yet.");
    gfx->setCursor(16, LIST_Y + 38); gfx->print("The watch can't power USB");
    gfx->setCursor(16, LIST_Y + 52); gfx->print("devices: use a powered hub.");
    gfx->setCursor(16, LIST_Y + 70); gfx->print("Keyboards work directly or");
    gfx->setCursor(16, LIST_Y + 84); gfx->print("behind one hub.");
    return;
  }
  bool kbd = usbhKbdUp() || usbhKeyCount() > 0;
  int rows = kbd ? 1 : ROWS;
  int first = scroll;
  if (kbd) {
    // Show the keyboard's own row above the typed text box.
    first = 0;
    for (int i = 0; i < n; i++) if (strcmp(usbhDevKind(snap[i]), "Keyboard") == 0) { first = i; break; }
    // Typed text box: wrap at 19 cols (2x font), newline honoured, show the tail.
    static char lines[6][20]; int nl = 0, col = 0;
    memset(lines, 0, sizeof(lines));
    static char txt[128];
    int tl = usbhText(txt, sizeof(txt));
    for (int i = 0; i < tl; i++) {
      char c = txt[i];
      if (c == '\n' || col == 19) { nl = (nl + 1) % 6; memset(lines[nl], 0, 20); col = 0; if (c == '\n') continue; }
      lines[nl][col++] = c;
    }
    uint32_t keys = usbhKeyCount();
    int16_t y0 = LIST_Y + ROW_H + 2;
    gfx->drawRoundRect(4, y0, W - 8, H - y0 - 4, 8, t.accent);
    gfx->setTextSize(1); gfx->setTextColor(t.line, t.bg);
    char b[40]; snprintf(b, sizeof(b), "typed (%lu keys, %d devices)", (unsigned long)keys, n);
    gfx->setCursor(12, y0 + 5); gfx->print(b);
    gfx->setTextSize(2); gfx->setTextColor(t.fg, t.bg);
    for (int i = 0; i < 6; i++) {                              // oldest -> newest, newest last
      int li = (nl + 1 + i) % 6;
      gfx->setCursor(10, y0 + 18 + i * 18); gfx->print(lines[li]);
    }
    gfx->fillRect(10 + col * 12, y0 + 18 + 5 * 18, 10, 15, t.accent);   // cursor
  } else if (first > n - 1) {
    first = scroll = max(0, n - rows);
  }
  for (int r = 0; r < rows && first + r < n; r++) {
    const UsbDev &d = snap[first + r];
    int16_t y = LIST_Y + r * ROW_H;
    uint16_t fg = d.gone ? t.line : t.fg;
    gfx->setTextSize(2);
    gfx->setTextColor(d.gone ? t.line : t.accent, t.bg);
    gfx->setCursor(10, y + 2);
    gfx->print(usbhDevKind(d));
    gfx->setTextSize(1);
    gfx->setTextColor(fg, t.bg);
    char b[48];
    if (d.port) snprintf(b, sizeof(b), "%04X:%04X %s hub port %u%s", d.vid, d.pid, d.speed ? "FS" : "LS",
                         d.port, d.gone ? " (gone)" : "");
    else        snprintf(b, sizeof(b), "%04X:%04X %s root%s", d.vid, d.pid, d.speed ? "FS" : "LS",
                         d.gone ? " (gone)" : "");
    gfx->setCursor(10, y + 21); gfx->print(b);
    snprintf(b, sizeof(b), "%.36s", d.prod[0] ? d.prod : (d.mfr[0] ? d.mfr : "(no name)"));
    gfx->setCursor(10, y + 32); gfx->print(b);
  }
}

void UsbHostView::drawAll() {
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("USB Host");
  drawStatus();
  drawList();
}

void UsbHostView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }
  firstDraw = true; shownGen = 0xFFFFFFFF; shownSecs = -1; shownState = -1; scroll = 0;
  startAt = millis() + 1500;
  EWLOGI("USBH", "enter: host window opens in 1.5 s");
}

void UsbHostView::onExit() { startAt = 0; usbhStop(); }

void UsbHostView::render() {
  if (!gfx) return;
  if (startAt && (int32_t)(millis() - startAt) >= 0) { startAt = 0; usbhStart(WINDOW_MS); }
  if (firstDraw) {
    drawAll(); firstDraw = false; shownGen = usbhGen(); shownState = (int)usbhState();
    return;
  }
  int secs = usbhSecsLeft();
  if (shownGen != usbhGen() || shownState != (int)usbhState()) {
    shownGen = usbhGen(); shownState = (int)usbhState(); shownSecs = secs;
    drawStatus(); drawList();
  } else if (secs != shownSecs && !usbhKbdUp()) {
    shownSecs = secs; drawStatus();
  }
}

void UsbHostView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) {
    if (usbhRunning()) { usbhStop(); return; }   // first press: end the window, stay
    startAt = 0;
    switchTo(Screen::AppList);
    return;
  }
  if (e.type == EventType::Gesture) {
    Gesture g = e.gesture;
    if (g == Gesture::SwipeRight) { switchTo(Screen::AppList); return; }
    if (g == Gesture::SwipeUp && scroll + ROWS < usbhCount()) { scroll += ROWS; drawList(); }
    if (g == Gesture::SwipeDown && scroll > 0) { scroll = max(0, scroll - ROWS); drawList(); }
    return;
  }
  if (e.type != EventType::Touch) return;
  if (tappedBack(e.x, e.y)) { switchTo(Screen::AppList); return; }
  if (!usbhRunning()) { startAt = 0; haptic(HapticRole::ToggleOn); usbhStart(WINDOW_MS); }
}
