// ewsim_host: I2C bus with three emulated chips at the EWatch addresses.
//   0x52 RV-3028 RTC   time = start time (--time/--date) + simulated clock; writes to
//                      0x00..0x06 reset it, other registers are plain storage
//   0x1C MMA8451       WHO_AM_I 0x1A; accel = flat face-up (0, 0, +1 g). ewsim's tilt/shake
//                      override what the controller sees, as on the real watch
//   0x15 CST816S       chip present, no finger, no gesture (simulated input comes in
//                      through ewsim, exactly like `ewt` drives the real watch)
#include <Wire.h>
#include "host_sim.h"
#include <ctime>

TwoWire Wire;
TwoWire Wire1;

static uint8_t sRtcRegs[256];
static uint8_t sMmaRegs[256];
static uint8_t sTchRegs[256];
static int64_t sRtcBase = 0;        // unix seconds at sim time 0

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void host_set_rtc(int year, int mon, int day, int hour, int min, int sec) {
  sRtcBase = daysFromCivil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

static void rtcSnapshot() {
  int64_t t = sRtcBase + (int64_t)(sim_now_us() / 1000000ULL);
  int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  int64_t secs = t - days * 86400;
  // civil from days
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)(yoe + era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  int wd = (int)((days % 7 + 11) % 7);   // 1970-01-01 was a Thursday (4); 0 = Sunday
  sRtcRegs[0] = bcd((int)(secs % 60));
  sRtcRegs[1] = bcd((int)(secs / 60 % 60));
  sRtcRegs[2] = bcd((int)(secs / 3600));
  sRtcRegs[3] = (uint8_t)wd;
  sRtcRegs[4] = bcd((int)d);
  sRtcRegs[5] = bcd((int)m);
  sRtcRegs[6] = bcd(y % 100);
}

static void rtcWrite(uint8_t reg, const uint8_t *d, size_t n) {
  bool timeTouched = false;
  rtcSnapshot();
  for (size_t i = 0; i < n; i++) {
    uint8_t r = (uint8_t)(reg + i);
    sRtcRegs[r] = d[i];
    if (r <= 6) timeTouched = true;
  }
  if (timeTouched) {
    int s = unbcd(sRtcRegs[0] & 0x7F), mi = unbcd(sRtcRegs[1] & 0x7F), h = unbcd(sRtcRegs[2] & 0x3F);
    int dd = unbcd(sRtcRegs[4] & 0x3F), mo = unbcd(sRtcRegs[5] & 0x1F), y = 2000 + unbcd(sRtcRegs[6]);
    int64_t t = daysFromCivil(y, mo, dd) * 86400 + h * 3600 + mi * 60 + s;
    sRtcBase = t - (int64_t)(sim_now_us() / 1000000ULL);
  }
}

static void initChips() {
  static bool done = false;
  if (done) return;
  done = true;
  sRtcRegs[0x28] = 0x30;           // RV-3028 ID
  sMmaRegs[0x0D] = 0x1A;           // MMA8451 WHO_AM_I
  // flat, face up: z = +1 g = 4096 counts (14-bit, left-justified in 16)
  int16_t z = 4096 << 2;
  sMmaRegs[0x05] = (uint8_t)(z >> 8); sMmaRegs[0x06] = (uint8_t)z;
  sTchRegs[0xA7] = 0xB5;           // CST816S chip id (CST816S)
  sTchRegs[0xA9] = 0x01;           // firmware version
  if (!sRtcBase) host_set_rtc(2026, 10, 7, 10, 9, 30);
}

static uint8_t *regsFor(uint16_t a) {
  switch (a) {
    case 0x52: return sRtcRegs;
    case 0x1C: return sMmaRegs;
    case 0x15: return sTchRegs;
    default: return nullptr;
  }
}

bool TwoWire::begin(int, int, uint32_t) { initChips(); return true; }

void TwoWire::beginTransmission(uint16_t address) {
  initChips();
  txAddr = address; txLen = 0;
}

size_t TwoWire::write(uint8_t b) {
  if (txLen < sizeof(txBuf)) txBuf[txLen++] = b;
  return 1;
}
size_t TwoWire::write(const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) write(b[i]);
  return n;
}

uint8_t TwoWire::endTransmission(bool) {
  uint8_t *regs = regsFor(txAddr);
  sim_advance_us(25 + 23 * txLen);          // 400 kHz: ~23 us per byte + start/addr
  if (!regs) return 2;                       // NACK on address
  if (txLen >= 1) {
    lastReg = txBuf[0];
    if (txLen > 1) {
      if (txAddr == 0x52) rtcWrite(txBuf[0], txBuf + 1, txLen - 1);
      else for (size_t i = 1; i < txLen; i++) regs[(uint8_t)(txBuf[0] + i - 1)] = txBuf[i];
    }
  }
  return 0;
}

size_t TwoWire::requestFrom(uint16_t address, size_t size, bool) {
  initChips();
  rxLen = rxPos = 0;
  uint8_t *regs = regsFor(address);
  sim_advance_us(25 + 23 * size);
  if (!regs) return 0;
  if (address == 0x52) rtcSnapshot();
  int r = lastReg < 0 ? 0 : lastReg;
  for (size_t i = 0; i < size && i < sizeof(rxBuf); i++) rxBuf[rxLen++] = regs[(uint8_t)(r + i)];
  return rxLen;
}

int TwoWire::available(void) { return (int)(rxLen - rxPos); }
int TwoWire::read(void) { return rxPos < rxLen ? rxBuf[rxPos++] : -1; }
int TwoWire::peek(void) { return rxPos < rxLen ? rxBuf[rxPos] : -1; }
