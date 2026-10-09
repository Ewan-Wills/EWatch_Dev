// Host stub of the tiny slice of Arduino.h that Arduino_GFX needs.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "Print.h"
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define pgm_read_ptr(a) (*(void *const *)(a))
#ifndef PI
#define PI 3.14159265358979323846
#endif
typedef bool boolean;
typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
class __FlashStringHelper;
class String {
public:
  String(const char *s = "") { strncpy(b, s ? s : "", sizeof b - 1); b[sizeof b - 1] = 0; }
  size_t length() const { return strlen(b); }
  const char *c_str() const { return b; }
private:
  char b[128];
};
