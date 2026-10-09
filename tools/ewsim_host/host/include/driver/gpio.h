#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE, GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL } gpio_int_type_t;
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
#define GPIO_NUM_0 0
static inline esp_err_t gpio_hold_en(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_hold_dis(gpio_num_t) { return ESP_OK; }
static inline void gpio_deep_sleep_hold_en(void) {}
static inline void gpio_deep_sleep_hold_dis(void) {}
static inline esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t) { return ESP_OK; }
static inline esp_err_t gpio_intr_disable(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_intr_enable(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_wakeup_enable(gpio_num_t, gpio_int_type_t) { return ESP_OK; }
static inline esp_err_t gpio_wakeup_disable(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_isr_register(void (*)(void *), void *, int, void *) { return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
static inline esp_err_t gpio_reset_pin(gpio_num_t) { return ESP_OK; }
