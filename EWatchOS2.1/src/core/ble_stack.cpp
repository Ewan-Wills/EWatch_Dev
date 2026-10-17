#include "ble_stack.h"
#include <BLEDevice.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_heap_caps.h>
#include "ewlog.h"
#include "power_mgr.h"
#include "wifi_svc.h"

static SemaphoreHandle_t sMux = nullptr;
static bool sUp = false;

// Link RSSI, read through the one custom GAP hook (BLEDevice allows only
// one, so it lives here, shared by every BLE feature).
static volatile int8_t   sRssi   = 0;
static volatile uint32_t sRssiMs = 0;     // 0 = no reading
static void gapHook(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  if (ev == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT &&
      p->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
    sRssi = (int8_t)p->read_rssi_cmpl.rssi;
    sRssiMs = millis() | 1;
  }
}

void bleRequestRssi(const uint8_t bda[6]) {
  if (!sUp) return;
  esp_bd_addr_t a; memcpy(a, bda, sizeof a);
  esp_ble_gap_read_rssi(a);              // async: result lands in gapHook
}

bool bleLastRssi(int8_t &rssi, uint32_t maxAgeMs) {
  uint32_t at = sRssiMs;
  if (!at || millis() - at > maxAgeMs) return false;
  rssi = sRssi;
  return true;
}
static int  sUsers = 0;

bool bleStackAcquire(const char *who, const char *devName) {
  if (!sMux) sMux = xSemaphoreCreateMutex();
  xSemaphoreTake(sMux, portMAX_DELAY);
  if (!sUp) {
    wifiSvcSuspend(true, 3000);
    powerPerfPin(true);
    size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    BLEDevice::init(devName);
    // BLEDevice::init flags itself initialised even when a step fails.
    sUp = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
    size_t h1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (!sUp) {
      EWLOGE("BLE", "stack init failed (%s) int_free=%u", who, (unsigned)h1);
      BLEDevice::deinit(false);
      powerPerfPin(false);
      wifiSvcSuspend(false, 0);
      xSemaphoreGive(sMux);
      return false;
    }
    BLEDevice::setCustomGapHandler(gapHook);
    EWLOGI("BLE", "stack up (%s) int_used=%d int_free=%u", who, (int)(h0 - h1), (unsigned)h1);
  }
  sUsers++;
  xSemaphoreGive(sMux);
  return true;
}

void bleStackRelease(const char *who) {
  if (!sMux) return;
  xSemaphoreTake(sMux, portMAX_DELAY);
  if (sUsers > 0 && --sUsers == 0 && sUp) {
    BLEDevice::deinit(false);          // keep memory so a later acquire can init
    sUp = false;
    sRssiMs = 0;
    powerPerfPin(false);
    wifiSvcSuspend(false, 0);
    EWLOGI("BLE", "stack down (%s) heap=%u", who, (unsigned)ESP.getFreeHeap());
  }
  xSemaphoreGive(sMux);
}

int bleStackUsers() { return sUsers; }
