// src/apps/screenshare.cpp — see screenshare.h for the wire protocol.
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include "screenshare.h"
#include "display.h"   // gfx, frameCanvas
#include "haptic.h"    // hapticBuzz
#include "haptic_roles.h"
#include "model.h"     // model + ModelLock
#include "ewlog.h"     // ewlogConsolePause

// Panel geometry (see _PRIMER.md / display.cpp).
static constexpr int16_t  SW = 240;   // screen width
static constexpr int16_t  SH = 280;   // screen height

// Hard ceiling on a single JPEG frame. A 240x280 frame at q40-60 is ~4-15 KiB;
// 96 KiB is a very generous guard that also rejects a desynced length field.
static constexpr uint32_t MAX_JPEG = 96 * 1024;

// Flow control (2026-10-05). USB-Serial/JTAG has no flow control: once the
// 4 KiB RX queue is full the ISR silently drops bytes. The companion therefore
// sends at most one block of the payload, then waits for ">>ACK <got>"; after
// the last block + csum it waits for ">>FRAME ..." before the next frame.
// Must match BLOCK in pc/screenshare.py and stay well under the RX queue.
static constexpr uint32_t kBlock = 2048;

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
  lastRxMs     = lastFrameMs;
  lastHelloMs  = 0;

  // This screen owns USB serial: stop the ewlog console task from reading
  // (it would steal frame bytes and answer "unknown: ..." into the stream).
  ewlogConsolePause(true);

  // Announce ourselves so the companion knows the app is up and can start.
  Serial.println(">>READY screenshare");
  Serial.flush();
}

void ScreenshareView::onExit() {
  Serial.println(">>BYE screenshare");
  Serial.flush();
  ewlogConsolePause(false);
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
// While a frame is in flight the companion is blocked on our ACK, so keep
// pumping (briefly waiting for the next block) instead of returning and
// costing a whole 33 ms render tick per 2 KiB block. Bounded so events and
// the WDT are still serviced.
void ScreenshareView::pumpSerial() {
  if (!ewlogConsolePaused()) return;  // ewt keepcon: console owns Serial
  // The cap is only a WDT backstop; copying already-arrived bytes is
  // memcpy-cheap, so keep it well above one max frame.
  int budget = (int)MAX_JPEG + 4096;
  const uint32_t t0 = millis();
  uint32_t lastByteMs = t0;
  for (;;) {
    if (Serial.available() <= 0 || budget <= 0) {
      uint32_t now = millis();
      bool midFrame = (st != Parse::Magic0);
      if (midFrame && budget > 0 && now - lastByteMs < 100 && now - t0 < 500) {
        vTaskDelay(1);
        continue;
      }
      // Companion vanished mid-frame: drop the partial frame so the next
      // one (or a restarted companion) starts from a clean parser.
      if (midFrame && now - lastRxMs > 1000) { bad++; resetParser(); }
      return;
    }
    budget--;
    uint8_t b = (uint8_t)Serial.read();
    lastByteMs = lastRxMs = millis();
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
            Serial.printf(">>FRAME BAD %lu\n", (unsigned long)bad);
            Serial.flush();
          } else {
            got = 0; csumAcc = 0; st = Parse::Payload;
          }
        }
        break;
      case Parse::Payload: {
        // Copy a run of bytes at once, never across a block boundary.
        uint32_t remain = frameLen - got;
        uint32_t toBlock = kBlock - (got % kBlock);
        int avail = Serial.available();
        uint32_t run = remain;
        if (toBlock < run) run = toBlock;
        if ((uint32_t)avail + 1 < run) run = (uint32_t)avail + 1;  // +1: current b
        jpegBuf[got++] = b; csumAcc ^= b; run--;
        while (run > 0 && budget-- > 0) {
          uint8_t c = (uint8_t)Serial.read();
          jpegBuf[got++] = c; csumAcc ^= c; run--;
        }
        if (got >= frameLen) {
          st = Parse::Csum;
        } else if (got % kBlock == 0) {
          // Block complete: release the companion for the next one.
          Serial.printf(">>ACK %lu\n", (unsigned long)got);
          Serial.flush();   // HWCDC holds short writes; the PC is waiting on this
        }
        break;
      }
      case Parse::Csum: {
        bool ok = false;
        if (b == csumAcc) {
          if (decodeAndBlit(frameLen)) { frames++; haveFrame = true; lastFrameMs = millis(); ok = true; }
          else bad++;
        } else {
          bad++;
        }
        resetParser();
        Serial.printf(">>FRAME %s %lu\n", ok ? "OK" : "BAD",
                      (unsigned long)(ok ? frames : bad));
        Serial.flush();     // the PC waits on this before sending the next frame
        return;             // one frame per pump; let the render loop breathe
      }
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
    if (now - lastHelloMs > 500) {
      Serial.println(">>READY screenshare"); Serial.flush(); lastHelloMs = now;
    }
  }

  // OS2.1: continuous re-rendering is driven by desiredFrameMs()==33 (see the
  // header), not a model.revision bump. Auto-sleep is suppressed for this
  // screen in power_mgr.cpp's screenBlocksSleep().
}

void ScreenshareView::onEvent(const Event &e) {
  // Back affordance: hardware button and right-swipe both arrive as ButtonShort.
  if (e.type == EventType::ButtonShort) {
    haptic(HapticRole::Back);
    switchTo(Screen::AppList);
    return;
  }
  if (e.type == EventType::Touch && tappedBack(e.x, e.y)) {
    haptic(HapticRole::Back);
    switchTo(Screen::AppList);
    return;
  }
}
