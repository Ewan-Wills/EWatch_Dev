// AnimDemoView — showcase for the anim framework. The render task blocks
// inside our render() (same pattern the video player uses) so we get a
// real ~30 fps frame budget without needing a global timer.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_random.h>
#include <math.h>

#include "anim_demo.h"
#include "anim.h"
#include "display.h"
#include "haptic.h"
#include "controller.h"

// ---------- screen + canvas ----------
static const int16_t SCREEN_W = 240;
static const int16_t SCREEN_H = 280;
static Arduino_Canvas *canvas = nullptr;
static UI             *ui     = nullptr;

// ---------- shape ids + reset state ----------
static ElementId idRect = 0, idCircle = 0, idTri = 0;
struct ResetPose {
  float x, y, w, h, radius, rotation, scaleX, scaleY, opacity;
  uint8_t r, g, b;
};
static ResetPose poseRect, poseCircle, poseTri;

// ---------- button pad layout (2 rows x 3 cols at the bottom) ----------
static const int16_t BTN_W = 72,  BTN_H = 38;
static const int16_t BTN_Y0 = 192, BTN_Y_STRIDE = 42;
static const int16_t BTN_X[3] = { 8, 84, 160 };

static const char *kBtnLabel[6] = { "Move", "Rotate", "Scale", "Fade", "Color", "Reset" };
static const uint16_t kBtnColor[6] = { NAVY, DARKGREEN, OLIVE, PURPLE, MAROON, DARKGREY };

// ---------- back chevron hit zone (mirrors view.cpp helpers) ----------
static const int16_t BACK_W = 60, BACK_H = 42;
static bool inRect(uint16_t x, uint16_t y, int16_t rx, int16_t ry,
                   int16_t rw, int16_t rh) {
  return (int16_t)x >= rx && (int16_t)x < rx + rw &&
         (int16_t)y >= ry && (int16_t)y < ry + rh;
}
static bool tappedBack(uint16_t x, uint16_t y) {
  return inRect(x, y, 0, 0, BACK_W + 12, BACK_H + 10);
}

// ---------- frame helpers ----------
static void capturePose(ResetPose &p, const Element &e) {
  p.x = e.x; p.y = e.y;
  p.w = e.w; p.h = e.h;
  p.radius = e.radius;
  p.rotation = e.rotation;
  p.scaleX = e.scaleX; p.scaleY = e.scaleY;
  p.opacity = e.opacity;
  p.r = e.r; p.g = e.g; p.b = e.b;
}
static void restorePose(ElementId id, const ResetPose &p) {
  ui->setPos     (id, p.x, p.y);
  ui->setSize    (id, p.w, p.h);
  ui->setRadius  (id, p.radius);
  ui->setRotation(id, p.rotation);
  ui->setScale   (id, p.scaleX, p.scaleY);
  ui->setOpacity (id, p.opacity);
  ui->setColor   (id, p.r, p.g, p.b);
}

static float frand(float lo, float hi) {
  return lo + (hi - lo) * ((float)esp_random() / (float)UINT32_MAX);
}

static void triggerAction(int idx) {
  ElementId ids[3] = { idRect, idCircle, idTri };
  switch (idx) {
    case 0: {  // Move — slide to a random position then back home.
      for (int i = 0; i < 3; i++) {
        float nx = frand(30, SCREEN_W - 30);
        float ny = frand(50, 170);
        ui->animate(ids[i], Prop::X, nx, 600, Easing::EaseOutQuad);
        ui->animate(ids[i], Prop::Y, ny, 600, Easing::EaseOutQuad);
        const ResetPose *p = (i == 0) ? &poseRect : (i == 1) ? &poseCircle : &poseTri;
        ui->animate(ids[i], Prop::X, p->x, 600, Easing::EaseInOutCubic, 700);
        ui->animate(ids[i], Prop::Y, p->y, 600, Easing::EaseInOutCubic, 700);
      }
      break;
    }
    case 1: {  // Rotate — full revolution, eased.
      for (int i = 0; i < 3; i++) {
        Element *e = ui->get(ids[i]);
        float target = (e ? e->rotation : 0.f) + 6.283185f;
        ui->animate(ids[i], Prop::Rotation, target, 1200, Easing::EaseInOutCubic);
      }
      break;
    }
    case 2: {  // Scale — pulse up with overshoot then back.
      for (int i = 0; i < 3; i++) {
        ui->animate(ids[i], Prop::ScaleX, 1.6f, 300, Easing::EaseOutBack);
        ui->animate(ids[i], Prop::ScaleY, 1.6f, 300, Easing::EaseOutBack);
        ui->animate(ids[i], Prop::ScaleX, 1.0f, 350, Easing::EaseOutBounce, 320);
        ui->animate(ids[i], Prop::ScaleY, 1.0f, 350, Easing::EaseOutBounce, 320);
      }
      break;
    }
    case 3: {  // Fade — opacity to 0.2 and back.
      for (int i = 0; i < 3; i++) {
        ui->animate(ids[i], Prop::Opacity, 0.2f, 400, Easing::EaseInOutQuad);
        ui->animate(ids[i], Prop::Opacity, 1.0f, 400, Easing::EaseInOutQuad, 420);
      }
      break;
    }
    case 4: {  // Color — swap palette then restore.
      static const uint16_t kCycle[3] = { CYAN, MAGENTA, YELLOW };
      for (int i = 0; i < 3; i++) {
        ui->animateColor(ids[i], kCycle[i], 500, Easing::EaseInOutCubic);
        const ResetPose *p = (i == 0) ? &poseRect : (i == 1) ? &poseCircle : &poseTri;
        ui->animate(ids[i], Prop::R, (float)p->r, 500, Easing::EaseInOutCubic, 520);
        ui->animate(ids[i], Prop::G, (float)p->g, 500, Easing::EaseInOutCubic, 520);
        ui->animate(ids[i], Prop::B, (float)p->b, 500, Easing::EaseInOutCubic, 520);
      }
      break;
    }
    case 5: {  // Reset — instantaneous restore.
      ui->cancelAllAnimations();
      restorePose(idRect,   poseRect);
      restorePose(idCircle, poseCircle);
      restorePose(idTri,    poseTri);
      break;
    }
  }
}

// "Tap a shape" — kick a quick pulse on that one element.
static void shapePulse(ElementId id) {
  if (!id) return;
  ui->animate(id, Prop::ScaleX, 1.8f, 200, Easing::EaseOutBack);
  ui->animate(id, Prop::ScaleY, 1.8f, 200, Easing::EaseOutBack);
  ui->animate(id, Prop::ScaleX, 1.0f, 300, Easing::EaseOutBounce, 220);
  ui->animate(id, Prop::ScaleY, 1.0f, 300, Easing::EaseOutBounce, 220);
}

static int hitButton(int x, int y) {
  for (int r = 0; r < 2; r++) {
    int16_t by = BTN_Y0 + r * BTN_Y_STRIDE;
    for (int c = 0; c < 3; c++) {
      if (inRect((uint16_t)x, (uint16_t)y, BTN_X[c], by, BTN_W, BTN_H)) {
        return r * 3 + c;
      }
    }
  }
  return -1;
}

// Drawn on top of the UI canvas after ui->render() each frame.
static void drawChrome() {
  // Back chevron
  canvas->fillRoundRect(2, 2, BACK_W, BACK_H, 6, DARKGREY);
  canvas->setTextColor(WHITE, DARKGREY);
  canvas->setTextSize(3);
  canvas->setCursor(18, 12);
  canvas->print('<');
  // Title
  canvas->setTextSize(2);
  canvas->setTextColor(WHITE, BLACK);
  canvas->setCursor(86, 14);
  canvas->print("Animator");
  canvas->drawFastHLine(20, 40, 200, DARKGREY);
  // Buttons
  for (int r = 0; r < 2; r++) {
    int16_t by = BTN_Y0 + r * BTN_Y_STRIDE;
    for (int c = 0; c < 3; c++) {
      int idx = r * 3 + c;
      int16_t bx = BTN_X[c];
      canvas->fillRoundRect(bx, by, BTN_W, BTN_H, 6, kBtnColor[idx]);
      canvas->drawRoundRect(bx, by, BTN_W, BTN_H, 6, WHITE);
      canvas->setTextColor(WHITE, kBtnColor[idx]);
      canvas->setTextSize(2);
      int16_t tw = (int16_t)strlen(kBtnLabel[idx]) * 12;
      canvas->setCursor(bx + (BTN_W - tw) / 2, by + 11);
      canvas->print(kBtnLabel[idx]);
    }
  }
}

// ---------- view methods ----------

void AnimDemoView::onEnter() {
  if (!canvas) {
    canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx, 0, 0);
    if (!canvas->begin()) {
      delete canvas; canvas = nullptr;
    }
  }
  if (canvas && !ui) ui = new UI(canvas);
  if (!ui) return;

  ui->cancelAllAnimations();
  ui->removeAll();
  ui->setBackground(BLACK);

  // Stage origin around y=110. Each shape stays inside x = 30 .. 210.
  idRect   = ui->rect  ( 56, 110,  44, 44, RED);
  idCircle = ui->circle(120, 110, 24,      GREEN);
  idTri    = ui->triangle(180 - 24, 130, 180 + 24, 130, 180, 86, BLUE);

  capturePose(poseRect,   *ui->get(idRect));
  capturePose(poseCircle, *ui->get(idCircle));
  capturePose(poseTri,    *ui->get(idTri));
}

void AnimDemoView::render() {
  if (!canvas || !ui) return;

  bool exitRequested = false;
  while (!exitRequested) {
    uint32_t t0 = millis();

    ui->tick();
    ui->render();
    drawChrome();
    canvas->flush();

    // Spend the remainder of the frame budget polling events. ~33 ms = 30 fps.
    int32_t budget = 33 - (int32_t)(millis() - t0);
    if (budget < 1) budget = 1;
    while (budget > 0 && !exitRequested) {
      Event e;
      if (xQueueReceive(eventQueue, &e, pdMS_TO_TICKS(budget)) != pdPASS) break;
      if (e.type == EventType::ButtonShort) { exitRequested = true; break; }
      if (e.type == EventType::Touch) {
        if (tappedBack(e.x, e.y)) { exitRequested = true; break; }
        int b = hitButton(e.x, e.y);
        if (b >= 0) {
          hapticBuzz(40, 50);
          triggerAction(b);
        } else {
          ElementId hit = ui->hitTest(e.x, e.y);
          if (hit) {
            hapticBuzz(60, 60);
            shapePulse(hit);
          }
        }
      }
      budget = 33 - (int32_t)(millis() - t0);
    }
  }
  switchTo(Screen::AppList);
}
