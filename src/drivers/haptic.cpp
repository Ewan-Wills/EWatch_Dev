#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "pins.h"
#include "haptic.h"

static const uint32_t HAPTIC_FREQ_HZ  = 10000;
static const uint8_t  HAPTIC_RES_BITS = 8;
static const uint8_t  HAPTIC_LEDC_CH  = 1;

struct BuzzReq { uint8_t intensity; uint16_t durationMs; };
static QueueHandle_t buzzQueue = nullptr;

// Strength scale [0..100]. 0 silences all haptic feedback, 100 = pass-through.
// Reads on the I/O hot path, writes from settings tasks — uint8_t writes are
// atomic on Xtensa so no lock needed.
static volatile uint8_t g_strengthPct = 100;

static void hapticTask(void *) {
  BuzzReq r;
  for (;;) {
    if (xQueueReceive(buzzQueue, &r, portMAX_DELAY) != pdPASS) continue;
    digitalWrite(PIN_MOTOR_EN, HIGH);
    ledcWrite(HAPTIC_LEDC_CH, r.intensity);
    vTaskDelay(pdMS_TO_TICKS(r.durationMs));
    ledcWrite(HAPTIC_LEDC_CH, 0);
    digitalWrite(PIN_MOTOR_EN, LOW);
  }
}

void hapticBegin() {
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, LOW);
  ledcSetup(HAPTIC_LEDC_CH, HAPTIC_FREQ_HZ, HAPTIC_RES_BITS);
  ledcAttachPin(PIN_MOTOR_PWM, HAPTIC_LEDC_CH);
  ledcWrite(HAPTIC_LEDC_CH, 0);

  buzzQueue = xQueueCreate(4, sizeof(BuzzReq));
  xTaskCreatePinnedToCore(hapticTask, "haptic", 2048, nullptr, 2, nullptr, 1);
}

// Non-blocking. Just posts the request; the haptic task owns the timing.
// If the queue is full (motor already busy with several queued buzzes), the
// new request is dropped — UI feedback is not worth backpressure. Intensity
// is scaled by the user-configured strength percentage before queueing.
void hapticBuzz(uint8_t intensity, uint16_t duration_ms) {
  if (!buzzQueue) return;
  uint8_t pct = g_strengthPct;
  if (pct == 0) return;            // silent mode — drop the buzz entirely
  uint16_t scaled = ((uint16_t)intensity * pct) / 100;
  if (scaled > 255) scaled = 255;
  BuzzReq r{(uint8_t)scaled, duration_ms};
  xQueueSend(buzzQueue, &r, 0);
}

void hapticSetStrengthPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_strengthPct = pct;
}
