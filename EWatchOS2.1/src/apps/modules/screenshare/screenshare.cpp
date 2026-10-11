// Screenshare — see screenshare.h; packet handling lives in core/ewstream.
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <hal/usb_serial_jtag_ll.h>
#include "screenshare.h"
#include "display.h"   // gfx
#include "haptic.h"    // hapticBuzz
#include "model.h"
#include "ewlog.h"     // ewlogConsolePause, EWLOGI
#include "ewsim.h"     // ewsimHandleLine
#include "ewstream.h"

static TaskHandle_t  sRxTask = nullptr;
static volatile bool sRxStop = false;

static SemaphoreHandle_t sTxMtx = nullptr;

// Replies go straight into the USB IN FIFO, not through HWCDC: its write()
// marks the port disconnected (and throws the data away) whenever the host
// hasn't collected the previous 64 B packet yet, which back-to-back >>R/>>D
// lines hit several times a second. Other Serial output (ewlog) still uses
// HWCDC; a line could interleave with one of ours, and the host treats a
// lost reply as a short timeout, not an error.
static void usbWrite(const uint8_t *p, size_t n) {
  xSemaphoreTake(sTxMtx, portMAX_DELAY);
  while (n) {
    int64_t t0 = esp_timer_get_time();
    while (!usb_serial_jtag_ll_txfifo_writable()) {
      if (esp_timer_get_time() - t0 > 50000) { xSemaphoreGive(sTxMtx); return; }   // host gone
      taskYIELD();
    }
    uint32_t k = usb_serial_jtag_ll_write_txfifo(p, n > 64 ? 64 : n);
    p += k; n -= k;
    if (!n || k < 64) usb_serial_jtag_ll_txfifo_flush();
  }
  xSemaphoreGive(sTxMtx);
}

static void simLine(char *line) { ewsimHandleLine(line); }

// Core 0 RX. Two modes:
//  idle:   HWCDC owns RX as usual (its ISR -> queue); we read Serial. ewt and
//          everything else on the console behave exactly as without the app.
//  direct: from the first packet header until 500 ms of quiet, the HWCDC RX
//          interrupt is masked and we read the 64 B USB FIFO ourselves. While
//          it's full the host is NAKed, so nothing is dropped, and there is no
//          per-byte queue cost. (Masking it for good broke HWCDC's TX: only its
//          RX interrupt re-sets the `connected` flag its write() depends on.)
static void rxTask(void *) {
  uint8_t buf[64];
  bool direct = false;
  int64_t lastData = esp_timer_get_time();
  while (!sRxStop) {
    if (!direct) {
      int n = Serial.read(buf, sizeof(buf));
      if (n > 0) {
        ewsFeed(buf, n, simLine);
        lastData = esp_timer_get_time();
        if (ewsInPacket()) {
          usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
          delayMicroseconds(50);                       // an ISR already running finishes
          while ((n = Serial.read(buf, sizeof(buf))) > 0) ewsFeed(buf, n, simLine);
          direct = true;
        }
        continue;
      }
      vTaskDelay(1);
      continue;
    }
    if (usb_serial_jtag_ll_rxfifo_data_available()) {
      uint32_t n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
      if (n) { ewsFeed(buf, n, simLine); lastData = esp_timer_get_time(); }
      continue;
    }
    int64_t quiet = esp_timer_get_time() - lastData;
    if (quiet > 500000 && !ewsInPacket()) {             // stream over: give RX back
      usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
      direct = false;
      continue;
    }
    // Spin through the gaps between USB packets inside a frame (sleeping a
    // 1 ms tick there NAKs the host and capped the link at ~180 KiB/s);
    // sleep once the burst is over so core 0's idle task runs.
    if (quiet > 4000) vTaskDelay(1);
  }
  if (direct) usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  sRxTask = nullptr;
  vTaskDelete(nullptr);
}

void ScreenshareView::onEnter() {
  showing = waitingDrawn = false;
  lastStatMs = millis(); statFrames = 0;
  drawWaiting();

  // ewt keepcon: the console keeps Serial and this app stays passive.
  ewlogConsolePause(true);
  if (!sTxMtx) sTxMtx = xSemaphoreCreateMutex();
  live = ewlogConsolePaused() && ewsBegin(usbWrite);
  if (!live) { EWLOGI("SSHR", "passive (console kept or no memory)"); return; }

  sRxStop = false;
  xTaskCreatePinnedToCore(rxTask, "ssrx", 4096, nullptr, 2, &sRxTask, 0);
  ewsReply(">>READY screenshare");
}

void ScreenshareView::onExit() {
  if (live) {
    sRxStop = true;
    ewsStop();
    for (int i = 0; i < 100 && sRxTask; i++) vTaskDelay(pdMS_TO_TICKS(2));
    ewsReply(">>BYE screenshare");
    ewsEnd();
    live = false;
  }
  ewlogConsolePause(false);
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
  gfx->print("Run screenshare.py");
  gfx->setCursor(16, 214);
  gfx->print("on the Mac over USB.");
  waitingDrawn = true;
}

void ScreenshareView::render() {
  if (!gfx) return;
  if (!live) { if (!waitingDrawn) drawWaiting(); return; }

  if (ewsService(10)) { showing = true; statFrames++; }

  uint32_t now = millis();
  // Stream stalled (companion quit / cable out): back to the banner.
  if (showing && now - ewsStats().lastPktMs > 3000) {
    ewsResetParser();
    showing = false;
    drawWaiting();
  }
  if (now - lastStatMs >= 5000) {
    const EwsStats &s = ewsStats();
    if (statFrames)
      EWLOGI("SSHR", "fps=%lu dec_us=%lu push_us=%lu bad=%lu",
             (unsigned long)(statFrames * 1000 / (now - lastStatMs)),
             (unsigned long)s.decUs, (unsigned long)s.pushUs, (unsigned long)s.bad);
    lastStatMs = now; statFrames = 0;
  }
}

void ScreenshareView::onEvent(const Event &e) {
  // Back: side button / right-swipe (both ButtonShort), or the back chevron
  // while the waiting banner is up (frames cover the whole panel).
  if (e.type == EventType::ButtonShort ||
      (e.type == EventType::Touch && !showing && tappedBack(e.x, e.y))) {
    hapticBuzz(120, 20);
    switchTo(Screen::AppList);
  }
}
