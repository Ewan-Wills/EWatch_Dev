#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *b, size_t n) { size_t r = 0; while (n--) r += write(*b++); return r; }
  size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
  size_t print(const char *s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { char b[16]; snprintf(b, sizeof b, "%d", v); return write(b); }
  size_t print(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v); return write(b); }
  size_t print(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); return write(b); }
  size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); return write(b); }
  size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return write(b); }
  size_t println(const char *s = "") { return write(s) + write((uint8_t)'\n'); }
  size_t printf(const char *f, ...) { char b[256]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return write(b); }
};
