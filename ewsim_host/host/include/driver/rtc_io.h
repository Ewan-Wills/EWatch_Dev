#pragma once
#include "driver/gpio.h"
static inline esp_err_t rtc_gpio_pullup_en(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_pullup_dis(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_en(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_en(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_dis(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_init(gpio_num_t) { return ESP_OK; }
static inline esp_err_t rtc_gpio_deinit(gpio_num_t) { return ESP_OK; }
