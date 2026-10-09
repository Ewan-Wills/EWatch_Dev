// ewsim_host: esp32-hal subset (GPIO, LEDC, ADC, timing, CPU clock, log macros).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_attr.h"
#include "esp32-hal-log.h"
#include "esp_system.h"
#include "esp_sleep.h"

#define LOW               0x0
#define HIGH              0x1
#define INPUT             0x01
#define OUTPUT            0x03
#define PULLUP            0x04
#define INPUT_PULLUP      0x05
#define PULLDOWN          0x08
#define INPUT_PULLDOWN    0x09
#define OPEN_DRAIN        0x10
#define OUTPUT_OPEN_DRAIN 0x12
#define ANALOG            0xC0

#define FSPI 1
#define HSPI 2

#ifdef __cplusplus
extern "C" {
#endif
unsigned long millis(void);
unsigned long micros(void);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield(void);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
uint16_t analogRead(uint8_t pin);
uint32_t analogReadMilliVolts(uint8_t pin);
typedef enum { ADC_0db, ADC_2_5db, ADC_6db, ADC_11db } adc_attenuation_t;
void analogSetPinAttenuation(uint8_t pin, adc_attenuation_t attenuation);
void analogReadResolution(uint8_t bits);
void analogWrite(uint8_t pin, int value);

uint32_t ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolution_bits);
void ledcWrite(uint8_t channel, uint32_t duty);
uint32_t ledcRead(uint8_t channel);
uint32_t ledcWriteTone(uint8_t channel, uint32_t freq);
void ledcAttachPin(uint8_t pin, uint8_t channel);
void ledcDetachPin(uint8_t pin);

bool setCpuFrequencyMhz(uint32_t cpu_freq_mhz);
uint32_t getCpuFrequencyMhz(void);
uint32_t getApbFrequency(void);
float temperatureRead(void);

void attachInterruptArg(uint8_t pin, void (*)(void *), void *arg, int mode);
void attachInterrupt(uint8_t pin, void (*)(void), int mode);
void detachInterrupt(uint8_t pin);

void *ps_malloc(size_t size);
void *ps_calloc(size_t n, size_t size);
void *ps_realloc(void *ptr, size_t size);
bool psramFound(void);
#ifdef __cplusplus
}
#endif
