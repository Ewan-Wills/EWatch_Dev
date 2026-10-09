#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
static inline esp_err_t esp_task_wdt_init(uint32_t, bool) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void *) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void *) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_deinit(void) { return ESP_OK; }
