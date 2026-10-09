// src/apps/screenshare.cpp — see screenshare.h for the wire protocol.
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include "screenshare.h"
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "model.h"     // model + ModelLock

// Panel geometry (see _PRIMER.md / display.cpp).
static constexpr int16_t  SW = 240;   // screen width
static constexpr int16_t  SH = 280;   // screen height

// Hard ceiling on a single JPEG frame. A 240x280 frame at q40-60 is ~4-15 KiB;
// 96 KiB is a very generous guard that also rejects a desynced length field.
static constexpr uint32_t MAX_JPEG = 96 * 1024;

// ---- JPEG decode (shared, drawn on the render task) ----------------------
static JPEGDEC   sJpeg;
static uint16_t *sFb = nullptr;        // current frame-canvas framebuffer

// Blit one decoded MCU block into the 240x280 framebuffer, clipped.
static int jpegDraw(JPEGDRAW *d) {
  if (!sFb) return 0;
  for (int r = 0; r < d->iHeight; r++) {
    int y = d->y + r;
    if (y < 0 || y >= SH) continue;
    int x = d->x;
    int w = d->iWidth;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SW) w = SW - x;
    if (w <= 0) continue;
    memcpy(sFb + (size_t)y * SW + x,
           d->pPixels + (size_t)r * d->iWidth,
           (size_t)w * 2);
  }
  return 1;
}

void ScreenshareView::resetParser() {
  st = Parse::Magic0;
  frameLen = got = 0;
  lenBytes = 0;
  csumAcc  = 0;
}

void ScreenshareView::onEnter() {
  if (gfx) { ThemeColors t = theme(); gfx->fillScreen(t.bg); }

  // One-time PSRAM scratch for the inbound JPEG payload.
  if (!jpegBuf) {
    jpegCap = MAX_JPEG;
    jpegBuf = (uint8_t *)ps_malloc(jpegCap);
    if (!jpegBuf) {                       // PSRAM exhausted — fall back to heap
      jpegBuf = (uint8_t *)malloc(jpegCap);
    }
  }

  resetParser();
  haveFrame    = false;
  waitingDrawn = false;
  frames = bad = 0;
  lastFrameMs  = millis();
  lastHelloMs  = 0;

  // Announce ourselves so the companion knows the app is up and can start.
  Serial.println(">>READY screenshare");
}

void ScreenshareView::onExit() {
  Serial.println(">>BYE screenshare");
  sFb = nullptr;
  if (jpegBuf) { free(jpegBuf); jpegBuf = nullptr; jpegCap = 0; }
}

// Decode the JPEG currently in jpegBuf[0..len) into the frame canvas + flush.
bool ScreenshareView::decodeAndBlit(uint32_t len) {
  Arduino_Canvas *cv = frameCanvas();
  if (!cv) return false;                 // PSRAM for the canvas unavailable
  sFb = cv->getFramebuffer();
  if (!sFb) return false;

  bool ok = sJpeg.openRAM(jpegBuf, (int)len, jpegDraw);
  if (ok) {
    sJpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    ok = (sJpeg.decode(0, 0, 0) == 1);
    sJpeg.close();
  }
  if (ok) cv->flush();                   // blit the whole 240x280 buffer
  sFb = nullptr;
  return ok;
}

void ScreenshareView::drawWaiting() {
  if (!gfx) return;
  ThemeColors t = theme();
  gfx->fillScreen(t.bg);
  drawTitleBar("Screenshare");
  gfx->setFont(nullptr);
  gfx->setTextSize(2);
  gfx->setTextColor(t.fg, t.bg);
  gfx->setCursor(24, 120);
  gfx->print("Waiting for");
  gfx->setCursor(56, 150);
  gfx->print("Mac...");
  gfx->setTextSize(1);
  gfx->setTextColor(t.line, t.bg);
  gfx->setCursor(16, 200);
  gfx->print("Run pc/screenshare.py");
  gfx->setCursor(16, 214);
  gfx->print("on the Mac over USB.");
  waitingDrawn = true;
}

// Drain whatever bytes USB CDC has buffered, advancing the frame FSM. A
// complete, checksum-valid frame is decoded and blitted immediately.
void ScreenshareView::pumpSerial() {
  // Drain whatever USB CDC has buffered this cycle. The cap is only a WDT
  // backstop; copying already-arrived bytes is memcpy-cheap, so keep it well
  // above one max frame so a frame is assembled without artificial stalls.
  int budget = (int)MAX_JPEG + 4096;
  while (Serial.available() > 0 && budget-- > 0) {
    uint8_t b = (uint8_t)Serial.read();
    switch (st) {
      case Parse::Magic0:
        if (b == 'E') st = Parse::Magic1;
        break;
      case Parse::Magic1:
        if (b == 'W') { st = Parse::Len; frameLen = 0; lenBytes = 0; }
        else if (b == 'E') { /* stay, could be the real magic */ }
        else st = Parse::Magic0;
        break;
      case Parse::Len:
        frameLen |= (uint32_t)b << (8 * lenBytes);   // little-endian
        if (++lenBytes == 4) {
          if (frameLen == 0 || frameLen > jpegCap) {  // desync / oversize
            bad++;
            resetParser();
          } else {
            got = 0; csumAcc = 0; st = Parse::Payload;
          }
        }
        break;
      case Parse::Payload: {
        // Copy a run of bytes at once rather than one at a time.
        uint32_t remain = frameLen - got;
        int avail = Serial.available();
        uint32_t run = remain;
        if ((uint32_t)avail + 1 < run) run = (uint32_t)avail + 1;  // +1: current b
        jpegBuf[got++] = b; csumAcc ^= b; run--;
        while (run > 0 && budget-- > 0) {
          uint8_t c = (uint8_t)Serial.read();
          jpegBuf[got++] = c; csumAcc ^= c; run--;
        }
        if (got >= frameLen) st = Parse::Csum;
        break;
      }
      case Parse::Csum:
        if (b == csumAcc) {
          if (decodeAndBlit(frameLen)) { frames++; haveFrame = true; lastFrameMs = millis(); }
          else bad++;
        } else {
          bad++;
        }
        resetParser();
        break;
    }
  }
}

void ScreenshareView::render() {
  if (!gfx) return;

  pumpSerial();

  // Show the waiting state until a frame arrives, or if the stream stalls for
  // more than ~1.5 s (companion quit / unplugged). Once frames flow, the canvas
  // flush in decodeAndBlit() owns the screen.
  bool stale = haveFrame && (millis() - lastFrameMs > 1500);
  if (!haveFrame || stale) {
    if (!waitingDrawn || stale) drawWaiting();
    if (stale) haveFrame = false;        // drop back to the waiting banner
    uint32_t now = millis();
    if (now - lastHelloMs > 500) { Serial.println(">>READY screenshare"); lastHelloMs = now; }
  }

  // Keep the render task cycling so we keep pumping serial at ~frame rate.
  // (Screenshare is excluded from auto-sleep in controller.cpp.)
  { ModelLock lk; model.revision++; }
}

void ScreenshareView::onEvent(const Event &e) {
  // Back affordance: hardware button and right-swipe both arrive as ButtonShort.
  if (e.type == EventType::ButtonShort) {
    hapticBuzz(120, 20);
    switchTo(Screen::AppList);
    return;
  }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    hapticBuzz(120, 20);
    switchTo(Screen::AppList);
    return;
  }
}
