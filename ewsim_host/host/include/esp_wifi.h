#pragma once
// ewsim_host: WiFi is not simulated (radio always off).
#include "esp_err.h"
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
static inline esp_err_t esp_wifi_stop(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_max_tx_power(int8_t) { return ESP_OK; }
static inline esp_err_t esp_wifi_get_max_tx_power(int8_t *p) { if (p) *p = 0; return ESP_OK; }
