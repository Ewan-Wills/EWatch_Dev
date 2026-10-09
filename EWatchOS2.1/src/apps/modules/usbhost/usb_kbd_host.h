// USB keyboard host service: turns the watch's USB-C port into a USB host
// (root device + one level of hub) and delivers HID boot-keyboard presses.
// Used by the USB Host scanner and the Bluetooth remote (keyboard -> media keys).
//
// While it runs, the S3's single USB PHY belongs to the OTG controller, so the
// Mac loses the serial port (console/ewt/flashing). usbhStop() hands the PHY
// back to USB-Serial/JTAG. The watch can't power USB devices: use a powered hub.
#pragma once
#include <Arduino.h>

#define USBH_MAX_DEV 8

struct UsbDev {
  uint8_t  addr;
  uint8_t  port;                     // 0 = root, n = hub port n
  bool     gone;
  uint16_t vid, pid, bcdUSB;
  uint8_t  cls, sub, proto;          // device-level class
  uint8_t  ifCls, ifSub, ifProto;    // first interface (class 0 devices put it here)
  uint8_t  speed;                    // 0 low, 1 full
  char     mfr[24], prod[28];
};

// One new key press (held keys and releases are not repeated).
struct UsbKey {
  uint8_t usage;                     // HID keyboard usage (0x04 'a' .. 0x52 Up)
  uint8_t mods;                      // HID modifier bits (0x02|0x20 = shift)
  char    ascii;                     // US-layout character, 0 if none
};

enum class UsbhSt : uint8_t { Idle, Starting, Scanning, Stopping, Done, Failed };

// windowMs: host mode ends after this long without a keyboard (kept open while
// one is attached); 0 = stays host until usbhStop().
void        usbhStart(uint32_t windowMs);
void        usbhStop();                       // blocks until serial is restored (<= 4 s)
bool        usbhRunning();
UsbhSt      usbhState();
const char *usbhError();
uint32_t    usbhGen();                        // bumps on any device/key/state change
bool        usbhKbdUp();
uint32_t    usbhKeyCount();
int         usbhSecsLeft();                   // -1 when not counting down
int         usbhCount();
int         usbhSnapshot(UsbDev *out, int max);
int         usbhText(char *out, int cap);     // typed text (US layout) since start
bool        usbhGetKey(UsbKey &k);            // non-blocking
const char *usbhClassName(uint8_t cls, uint8_t sub, uint8_t proto);
const char *usbhDevKind(const UsbDev &d);
