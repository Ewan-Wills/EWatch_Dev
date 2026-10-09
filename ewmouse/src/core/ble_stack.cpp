#include "ble_stack.h"
#include <BLEDevice.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>
#include "ewlog.h"
#include "power_mgr.h"
#include "wifi_svc.h"

static SemaphoreHandle_t sMux = nullptr;
static int  sUsers = 0;
static bool sUp = false;

bool bleStackAcquire(const char *who) {
  if (!sMux) sMux = xSemaphoreCreateMutex();
  xSemaphoreTake(sMux, portMAX_DELAY);
  if (!sUp) {
    wifiSvcSuspend(true, 3000);
    powerPerfPin(true);
    size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    BLEDevice::init("EWatch");
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
    powerPerfPin(false);
    wifiSvcSuspend(false, 0);
    EWLOGI("BLE", "stack down (%s) heap=%u", who, (unsigned)ESP.getFreeHeap());
  }
  xSemaphoreGive(sMux);
}

int bleStackUsers() { return sUsers; }
