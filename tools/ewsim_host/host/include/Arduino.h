// ewsim_host: host (macOS) replacement for arduino-esp32's Arduino.h.
// Time is simulated (see host/src/sched.cpp): millis()/micros() return the
// scheduler's clock, delay() blocks the calling simulated task.
#ifndef Arduino_h
#define Arduino_h
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp32-hal.h"
#include "stdlib_noniso.h"
#include "binary.h"
#include "pgmspace.h"

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352
#define SERIAL  0x0
#define DISPLAY 0x1
#define LSBFIRST 0
#define MSBFIRST 1
#define RISING    0x01
#define FALLING   0x02
#define CHANGE    0x03
#define ONLOW     0x04
#define ONHIGH    0x05
#define DEFAULT 1
#define EXTERNAL 0
#ifndef __STRINGIFY
#define __STRINGIFY(a) #a
#endif
#define _min(a,b) ((a)<(b)?(a):(b))
#define _max(a,b) ((a)>(b)?(a):(b))
#define _abs(x) ((x)>0?(x):-(x))
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#define _round(x)     ((x)>=0?(long)((x)+0.5):(long)((x)-0.5))
#define radians(deg) ((deg)*DEG_TO_RAD)
#define degrees(rad) ((rad)*RAD_TO_DEG)
#define sq(x) ((x)*(x))
#define sei()
#define cli()
#define interrupts()
#define noInterrupts()
#define lowByte(w) ((uint8_t) ((w) & 0xff))
#define highByte(w) ((uint8_t) ((w) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit) ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))
#define _BV(b) (1UL << (b))
#define NOT_A_PIN -1
#define NOT_AN_INTERRUPT -1
#define digitalPinToInterrupt(p) (p)
#define NUM_DIGITAL_PINS 49

typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

#ifdef __cplusplus
void setup(void);
void loop(void);
long random(long);
long random(long, long);
void randomSeed(unsigned long);
long map(long, long, long, long, long);
#include <algorithm>
#include <functional>
#include <cmath>
#include "WCharacter.h"
#include "WString.h"
#include "Stream.h"
#include "Printable.h"
#include "Print.h"
#include "IPAddress.h"
#include "host_serial.h"
#include "host_esp.h"
using std::abs;
using std::isinf;
using std::isnan;
using std::max;
using std::min;
using std::round;
uint16_t makeWord(uint16_t w);
uint16_t makeWord(uint8_t h, uint8_t l);
#define word(...) makeWord(__VA_ARGS__)
extern "C" bool getLocalTime(struct tm * info, uint32_t ms = 5000);
extern "C" void configTime(long gmtOffset_sec, int daylightOffset_sec,
        const char* server1, const char* server2 = nullptr, const char* server3 = nullptr);
#endif

#endif
