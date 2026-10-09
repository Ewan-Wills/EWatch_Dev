#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include "display.h"
#include "haptic.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "power_mgr.h"
#include <esp_heap_caps.h>
#include "launcher.h"

using namespace aura;

static const int16_t HEADER_H  = 52;    // big title zone (scrolls away)
static const int16_t SECTION_H = 26;
static const int16_t ICON_BOX  = 44;
static const int16_t VIEW_TOP  = 0;

// ---------------------------------------------------------------------------
// Layout: compute content-space Y for every row (sections add a header gap).
// ---------------------------------------------------------------------------
// Pre-rendered 44x44 icon tiles (PSRAM). The vector icons cost hundreds of
// primitive calls each (the WiFi glyph alone is ~90 fillCircles); drawing
// them per frame capped the scroll at ~15 fps. Rendered once per onEnter
// (theme may have changed), then row-blitted with explicit clipping — never
// through the library's unclipped bitmap path.
void AuraListView::buildTileCache() {
  const int T = 44;
  if (!tileCache) {
    tileCache = (uint16_t *)heap_caps_malloc(
        (size_t)N * T * T * 2, MALLOC_CAP_SPIRAM);
    if (!tileCache) return;                     // fall back to live drawing
  }
  AuraTheme th = auraTheme();
  uint16_t *fb = canvas->getFramebuffer();
  for (int i = 0; i < N; i++) {
    // Stamp the tile into the canvas top-left, then copy it out.
    canvas->fillRect(0, 0, T, T, th.card);
    drawIconTile(canvas, entries[i].icon, 0, 0, T,
                 entries[i].tileColor, aura::kText);
    uint16_t *dst = tileCache + (size_t)i * T * T;
    for (int r = 0; r < T; r++)
      memcpy(dst + r * T, fb + r * W, (size_t)T * 2);
  }
}

void AuraListView::blitTile(int i, int16_t x, int16_t y) {
  const int T = 44;
  if (!tileCache) {                             // cache alloc failed: draw live
    drawIconTile(canvas, entries[i].icon, x, y, T,
                 entries[i].tileColor, aura::kText);
    return;
  }
  uint16_t *fb  = canvas->getFramebuffer();
  const uint16_t *src = tileCache + (size_t)i * T * T;
  for (int r = 0; r < T; r++) {
    int16_t dy = y + r;
    if (dy < 0 || dy >= H) continue;            // explicit top/bottom clip
    memcpy(fb + (size_t)dy * W + x, src + (size_t)r * T, (size_t)T * 2);
  }
}

void AuraListView::layout() {
  int y = HEADER_H + 6;
  for (int i = 0; i < N && i < 40; i++) {
    if (entries[i].section) y += SECTION_H;
    rowY[i] = y;
    y += kRowH + kRowGap;
  }
  contentH = y + 8;
  phys.maxScroll = contentH > H ? (contentH - H) : 0;
}

void AuraListView::onEnter() {
  canvas = frameCanvas();
  layout();
  if (canvas) buildTileCache();
  // Keep scroll position across visits (feels like coming back, not starting
  // over) — but clamp in case the table changed.
  if (phys.pos > phys.maxScroll) phys.pos = phys.maxScroll;
  if (phys.pos < 0) phys.pos = 0;
  phys.velocity = 0;
  pressed = -1; dragMode = false;
  launchAtMs = 0; launchRow = -1;
  entrance.start(24.0f, 0.0f, 260, easeOutCubic);   // rows rise 24px into place
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static void drawMiniStatus(Arduino_GFX *d, const AuraTheme &th) {
  // Small clock + battery dot, top-right (visible on menu pages, Apple-style).
  uint8_t hh, mm, pct; bool ok, low;
  { ModelLock lk; hh = model.hour; mm = model.minute;
    pct = model.batPct; ok = model.batOk; low = model.batLow; }
  char buf[8];
  snprintf(buf, sizeof buf, "%02u:%02u", hh, mm);
  d->setFont(nullptr);
  d->setTextSize(1);
  d->setTextColor(th.textDim);
  int16_t tw = auraTextWidth(buf, 1);
  d->setCursor(W - kGutter - tw, 8);
  d->print(buf);
  if (ok) {
    uint16_t c = low ? kRed : (pct <= 25 ? kOrange : kGreen);
    d->fillCircle(W - kGutter - tw - 10, 11, 3, c);
  }
}

void AuraListView::drawFrame() {
  AuraTheme th = auraTheme();
  canvas->fillScreen(th.bg);

  float scroll = phys.pos;
  float rise   = entrance.value();               // entrance cascade offset

  // Big title — scrolls off with the content (drawn first, rows overpaint).
  // Drawn ONLY while fully below the top edge: partially-off-top text writes
  // out of the framebuffer (library clip hole — see note above).
  int16_t titleY = (int16_t)(HEADER_H - 30 - scroll * 0.85f);
  if (titleY >= 0) {
    canvas->setFont(nullptr);
    canvas->setTextSize(3);
    canvas->setTextColor(th.text);
    canvas->setCursor(kGutter + 2, titleY);
    canvas->print(title);
  }
  drawMiniStatus(canvas, th);

  for (int i = 0; i < N && i < 40; i++) {
    float cascade = rise * (1.0f + 0.35f * i);
    if (cascade > 60) cascade = 60;
    int16_t y = (int16_t)(rowY[i] - scroll + cascade);

    if (entries[i].section) {
      int16_t sy = y - SECTION_H + 6;
      if (sy >= 0 && sy < H) {     // >= 0: top-clipped text corrupts the heap
        canvas->setFont(nullptr);
        canvas->setTextSize(1);
        canvas->setTextColor(th.textDim);
        canvas->setCursor(kGutter + 4, sy);
        canvas->print(entries[i].section);
      }
    }
    if (y + kRowH < 0 || y >= H) continue;

    bool hot = (i == pressed) || (i == launchRow);
    auraCard(canvas, kGutter, y, W - 2 * kGutter, kRowH, kRadius,
             hot ? th.cardHi : th.card);

    int16_t ix = kGutter + 10, iy = y + (kRowH - ICON_BOX) / 2;
    blitTile(i, ix, iy);

    int16_t ly = y + kRowH / 2 - 7;
    if (ly >= 0) {                 // same top-clip rule as the title
      canvas->setFont(nullptr);
      canvas->setTextSize(2);
      canvas->setTextColor(th.text);
      canvas->setCursor(ix + ICON_BOX + 12, ly);
      canvas->print(entries[i].label);
    }

    // Trailing chevron affordance.
    int16_t chx = W - kGutter - 18, chy = y + kRowH / 2;
    canvas->drawLine(chx, chy - 5, chx + 5, chy, th.textDim);
    canvas->drawLine(chx + 5, chy, chx, chy + 5, th.textDim);
    canvas->drawLine(chx - 1, chy - 5, chx + 4, chy, th.textDim);
    canvas->drawLine(chx + 4, chy, chx - 1, chy + 5, th.textDim);
  }

  auraScrollbar(canvas, phys.pos, phys.maxScroll, H);
}

bool AuraListView::paintTo(Arduino_Canvas *cv) {
  if (!cv) return false;
  canvas = cv;
  drawFrame();
  return true;
}

bool AuraListView::animating() const {
  return phys.moving() || !entrance.done() || launchAtMs != 0;
}

uint16_t AuraListView::desiredFrameMs() const {
  // 33 while in motion so taskRender holds 240 MHz for the physics frames;
  // 0 when settled (event-driven, back to the 80 MHz base clock).
  return animating() ? 33 : 0;
}

// ---------------------------------------------------------------------------
// The blocking render loop (carousel pattern): draw, flush, drain events on a
// ~30 fps budget while anything is in motion; return when settled.
// ---------------------------------------------------------------------------
void AuraListView::render() {
  if (!canvas) {
    // PSRAM canvas unavailable — degrade to a static, readable page rather
    // than an invisible-but-live UI. Button still goes back.
    if (gfx) {
      AuraTheme th = auraTheme();
      gfx->fillScreen(th.bg);
      auraTextCentered(gfx, W / 2, 120, title, 2, th.text);
      auraTextCentered(gfx, W / 2, 150, "UI unavailable (no mem)", 1, th.textDim);
      auraTextCentered(gfx, W / 2, 170, "press button to go back", 1, th.textDim);
    }
    return;
  }
  // Hold 240 MHz for the whole blocking animation. taskRender's governor
  // only re-evaluates BETWEEN render() calls — without this, a drag that
  // starts from a settled (80 MHz) list runs its entire life at 80 MHz.
  powerPerfDemand(true);
  for (;;) {
    esp_task_wdt_reset();
    uint32_t t0 = millis();
    if ((phys.dragging || dragMode || pressed >= 0) &&
        millis() - lastFingerMs > 400) {
      phys.release();
      pressed = -1;
      dragMode = false;
    }
    // Drain whatever is already queued so this frame tracks the freshest
    // finger sample (drawing first meant every frame lagged a full ~33 ms).
    {
      Event e;
      while (xQueueReceive(eventQueue, &e, 0) == pdPASS) {
        handleEvent(e);
        if (currentView != this) return;   // navigated away: no stale paint
      }
    }

    if (launchAtMs && (int32_t)(millis() - launchAtMs) >= 0) {
      int r = launchRow;
      launchAtMs = 0; launchRow = -1;
      if (r >= 0 && r < N) switchToAnimated(entries[r].target, Trans::PushLeft);
      return;
    }

    phys.step();
    drawFrame();
    canvas->flush();

    if (!animating() && !launchAtMs) return;

    // Sleep out the rest of the frame ON the queue: a new event wakes us
    // early and the next pass consumes it immediately.
    int32_t budget = 33 - (int32_t)(millis() - t0);
    if (budget > 0) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) == pdPASS) {
        handleEvent(e);
        if (currentView != this) return;
      }
    }
  }
}

void AuraListView::onEvent(const Event &e) {
  handleEvent(e);
  if (animating()) { ModelLock lk; model.revision++; }   // re-enter render()
}

int AuraListView::hitRow(int16_t x, int16_t y) const {
  if (x < kGutter || x > W - kGutter) return -1;
  float cy = y + phys.pos;
  for (int i = 0; i < N && i < 40; i++) {
    if (cy >= rowY[i] && cy < rowY[i] + kRowH) return i;
  }
  return -1;
}

void AuraListView::goBack() {
  hapticBuzz(20, 22);
  switchToAnimated(backScreen, backTrans);
}

void AuraListView::handleEvent(const Event &e) {
  if (launchAtMs) return;                        // mid-launch: ignore input

  powerOnActivity();               // in-loop drains bypass taskRender's call

  switch (e.type) {
    case EventType::TimerExpired:    // countdown due mid-scroll: don't eat it
      switchTo(Screen::Timer);
      return;
    case EventType::ButtonVeryLong:  // global power-off gesture
      switchTo(Screen::PowerOff);
      return;
    case EventType::Touch:
      lastFingerMs = millis();
      downX = e.x; downY = e.y;
      dragMode = false;
      // While the entrance cascade is still moving rows into place the drawn
      // positions differ from rowY[] — suppress row-arming (scroll still ok).
      pressed = entrance.done() ? hitRow(e.x, e.y) : -1;
      pressedMs = millis();
      phys.grab(e.y);
      break;

    case EventType::TouchHold: {
      lastFingerMs = millis();
      int16_t dx = (int16_t)e.x - (int16_t)downX;
      int16_t dy = (int16_t)e.y - (int16_t)downY;
      if (!dragMode && (int32_t)dx * dx + (int32_t)dy * dy > 10 * 10) {
        dragMode = true;
        pressed = -1;                            // moving = not a tap anymore
      }
      if (dragMode) phys.drag(e.y);
      break;
    }

    case EventType::TouchUp: {
      int16_t dy = (int16_t)downY - (int16_t)e.y;
      int16_t dx = (int16_t)e.x - (int16_t)downX;
      bool travelled = (int32_t)dx * dx + (int32_t)dy * dy > 14 * 14;
      if (!dragMode && travelled) {
        // Flick faster than the 30 Hz TouchHold stream: no Hold frames ever
        // arrived, so dragMode never armed. Without this it registered as a
        // TAP (launching whatever row was under the finger). Convert the
        // travel into a fling instead.
        uint32_t dt = millis() - pressedMs;
        if (dt < 1) dt = 1;
        float v = (float)dy * 1000.0f / (float)dt;
        if (v > 6000.0f) v = 6000.0f;
        if (v < -6000.0f) v = -6000.0f;
        phys.velocity = v;
        pressed = -1;
      }
      phys.release();
      if (!dragMode && !travelled && pressed >= 0) {
        hapticBuzz(24, 30);
        launchRow  = pressed;
        launchAtMs = millis() + 110;             // one press-flash frame first
      }
      pressed = -1;
      dragMode = false;
      break;
    }

    case EventType::Gesture:
      if (e.gesture == Gesture::SwipeRight) goBack();
      break;

    case EventType::ButtonShort:
      goBack();
      break;

    default:
      break;
  }
}
