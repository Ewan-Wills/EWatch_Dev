// USB keyboard host service — see usb_kbd_host.h.
//
// A small USB host stack written directly on ESP-IDF 4.4.7's HCD layer
// (idf447_hcd.h), because the IDF 4.4 USB Host Library can't see devices
// behind a hub. One task owns everything: PHY -> OTG host, root port, device
// enumeration (root device + one level of external hub), HID boot keyboard,
// then tear-down and PHY back to USB-Serial/JTAG. Views read snapshots and
// drain the key queue.
//
// Pipe budget: the S3's DWC2 has 8 host channels and every HCD pipe holds one,
// so only the hub's and the keyboard's pipes stay allocated; other devices are
// enumerated, listed and their EP0 pipe freed.
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include "usb_kbd_host.h"
#include "ewlog.h"

#include "idf447_hcd.h"
#include "usb/usb_helpers.h"
#include "esp_private/usb_phy.h"
#include "soc/rtc_cntl_struct.h"
#include "soc/usb_wrap_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "esp_intr_alloc.h"

static const int     MAX_DEV = USBH_MAX_DEV;
static const int     MAX_HUB_PORTS = 7;

typedef UsbhSt St;

static volatile St   gSt = St::Idle;
static volatile bool gRun = false;
static volatile uint32_t gDeadline = 0;
static volatile uint32_t gWindowMs = 30000;   // 0 = until usbhStop()
static QueueHandle_t gKeyQ = nullptr;
static volatile uint32_t gGen = 0;
static TaskHandle_t  gTask = nullptr;
static SemaphoreHandle_t gMux = nullptr;
static UsbDev gDev[MAX_DEV];
static int    gCount = 0;
static char   gErr[40];

const char *usbhClassName(uint8_t cls, uint8_t sub, uint8_t proto) {
  switch (cls) {
    case 0x03:
      if (sub == 1 && proto == 1) return "Keyboard";
      if (sub == 1 && proto == 2) return "Mouse";
      return "HID";
    case 0x09: return "Hub";
    case 0x08: return "Mass storage";
    case 0x02: case 0x0A: return "CDC serial";
    case 0x01: return "Audio";
    case 0x07: return "Printer";
    case 0x0E: return "Video";
    case 0xE0: return "Wireless";
    case 0xEF: return "Misc";
    case 0xFF: return "Vendor";
    default:   return "Device";
  }
}

const char *usbhDevKind(const UsbDev &d) {
  return d.cls ? usbhClassName(d.cls, d.sub, d.proto) : usbhClassName(d.ifCls, d.ifSub, d.ifProto);
}

// ---------------------------------------------------------------------------
// HCD plumbing. Callbacks may run in the USB ISR: they only set flags / give
// a semaphore; everything else happens on the host task.
// ---------------------------------------------------------------------------
static hcd_port_handle_t gPort = nullptr;
static volatile bool gPortEv = false;
static SemaphoreHandle_t gCtlSem = nullptr;
static urb_t *gCtlUrb = nullptr;               // shared by all control transfers
static const int CTL_DATA_MAX = 320;

static bool portCb(hcd_port_handle_t, hcd_port_event_t, void *, bool) { gPortEv = true; return false; }

static bool ctlCb(hcd_pipe_handle_t, hcd_pipe_event_t, void *, bool in_isr) {
  BaseType_t woke = pdFALSE;
  if (in_isr) xSemaphoreGiveFromISR(gCtlSem, &woke); else xSemaphoreGive(gCtlSem);
  return woke == pdTRUE;
}

static void pipeKill(hcd_pipe_handle_t p) {
  if (!p) return;
  hcd_pipe_command(p, HCD_PIPE_CMD_HALT);
  hcd_pipe_command(p, HCD_PIPE_CMD_FLUSH);
  while (hcd_urb_dequeue(p)) {}
  hcd_pipe_free(p);
}

// One control transfer on a default pipe. Returns data-stage bytes (>= 0) or <0.
static int ctrl(hcd_pipe_handle_t pipe, int mps, uint8_t bmRT, uint8_t bReq,
                uint16_t wVal, uint16_t wIdx, uint16_t wLen, uint8_t *in) {
  usb_transfer_t *t = &gCtlUrb->transfer;
  usb_setup_packet_t *sp = (usb_setup_packet_t *)t->data_buffer;
  sp->bmRequestType = bmRT; sp->bRequest = bReq;
  sp->wValue = wVal; sp->wIndex = wIdx; sp->wLength = wLen;
  bool isIn = bmRT & 0x80;
  int data = (isIn && wLen) ? usb_round_up_to_mps(wLen, mps) : wLen;
  if (data > CTL_DATA_MAX) return -1;
  t->num_bytes = sizeof(usb_setup_packet_t) + data;
  t->actual_num_bytes = 0;
  xSemaphoreTake(gCtlSem, 0);                          // drop a stale give
  if (hcd_urb_enqueue(pipe, gCtlUrb) != ESP_OK) return -2;
  if (xSemaphoreTake(gCtlSem, pdMS_TO_TICKS(1000)) != pdTRUE) {
    hcd_pipe_command(pipe, HCD_PIPE_CMD_HALT);
    hcd_pipe_command(pipe, HCD_PIPE_CMD_FLUSH);
    hcd_urb_dequeue(pipe);
    hcd_pipe_command(pipe, HCD_PIPE_CMD_CLEAR);
    return -3;
  }
  urb_t *u = hcd_urb_dequeue(pipe);
  if (!u) return -4;
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
    if (hcd_pipe_get_state(pipe) == HCD_PIPE_STATE_HALTED) hcd_pipe_command(pipe, HCD_PIPE_CMD_CLEAR);
    return -10 - (int)t->status;
  }
  int n = t->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
  if (n < 0) n = 0;
  if (n > wLen) n = wLen;
  if (in && n) memcpy(in, t->data_buffer + sizeof(usb_setup_packet_t), n);
  return n;
}

static void strDesc(const uint8_t *d, int n, char *out, size_t cap) {
  out[0] = 0;
  if (n < 2 || d[1] != 3) return;
  int len = d[0] < n ? d[0] : n;
  size_t j = 0;
  for (int i = 2; i + 1 < len && j + 1 < cap; i += 2) {
    uint16_t c = d[i] | (d[i + 1] << 8);
    out[j++] = (c >= 32 && c < 127) ? (char)c : '?';
  }
  out[j] = 0;
}

// ---------------------------------------------------------------------------
// HID boot keyboard (root or behind the hub). Boot protocol -> every keyboard
// sends the same 8-byte report: [mods, 0, k1..k6].
// ---------------------------------------------------------------------------
static hcd_pipe_handle_t gKbdCtl = nullptr, gKbdIn = nullptr;
static urb_t   *gKbdUrb = nullptr;
static usb_ep_desc_t gKbdEpDesc;
static int     gKbdDevIdx = -1;
static volatile bool gKbdUp = false;
static volatile bool gKbdDone = false;
static uint8_t gPrevKeys[6];
static char    gText[120];
static int     gTextLen = 0;
static volatile uint32_t gKeyCount = 0;

static bool kbdCb(hcd_pipe_handle_t, hcd_pipe_event_t, void *, bool) { gKbdDone = true; return false; }

static char hidToAscii(uint8_t k, bool shift) {
  if (k >= 0x04 && k <= 0x1D) return (shift ? 'A' : 'a') + (k - 0x04);
  if (k >= 0x1E && k <= 0x27) return shift ? "!@#$%^&*()"[k - 0x1E] : "1234567890"[k - 0x1E];
  switch (k) {
    case 0x28: return '\n';
    case 0x2B: return ' ';                                   // tab -> space
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default:   return 0;
  }
}

static void kbdReport(const uint8_t *r, int n) {
  if (n < 3) return;
  bool shift = r[0] & 0x22;
  int nk = n - 2 > 6 ? 6 : n - 2;
  for (int i = 0; i < nk; i++) {
    uint8_t k = r[2 + i];
    if (k < 0x04) continue;                                  // none / rollover error
    bool held = false;
    for (int j = 0; j < 6; j++) if (gPrevKeys[j] == k) held = true;
    if (held) continue;                                      // only new presses
    char c = hidToAscii(k, shift);
    xSemaphoreTake(gMux, portMAX_DELAY);
    if (k == 0x2A) { if (gTextLen) gText[--gTextLen] = 0; }  // backspace
    else if (c) {
      if (gTextLen >= (int)sizeof(gText) - 1) { memmove(gText, gText + 20, gTextLen - 20); gTextLen -= 20; }
      gText[gTextLen++] = c; gText[gTextLen] = 0;
    }
    gKeyCount++; gGen++;
    xSemaphoreGive(gMux);
    if (gKeyQ) { UsbKey uk = { k, r[0], c }; xQueueSend(gKeyQ, &uk, 0); }
    EWLOGI("USBH", "key 0x%02x '%c' mods=%02x", k, (c >= 32 && c < 127) ? c : '.', r[0]);
  }
  memset(gPrevKeys, 0, sizeof(gPrevKeys));
  memcpy(gPrevKeys, r + 2, nk);
}

static void kbdSubmit() {
  usb_transfer_t *t = &gKbdUrb->transfer;
  int mps = gKbdEpDesc.wMaxPacketSize & 0x7FF;
  t->num_bytes = mps > 64 ? 64 : mps;
  t->actual_num_bytes = 0;
  if (hcd_urb_enqueue(gKbdIn, gKbdUrb) != ESP_OK) EWLOGW("USBH", "kbd enqueue failed");
}

static void kbdStart(int devIdx, hcd_pipe_handle_t ctlPipe, int mps0, uint8_t ifNum, usb_speed_t speed) {
  const UsbDev &d = gDev[devIdx];
  int r = ctrl(ctlPipe, mps0, 0x21, 0x0B, 0, ifNum, 0, nullptr);   // SET_PROTOCOL(boot)
  EWLOGI("USBH", "kbd set_protocol(boot) r=%d", r);
  ctrl(ctlPipe, mps0, 0x21, 0x0A, 0, ifNum, 0, nullptr);           // SET_IDLE(0): reports on change only
  hcd_pipe_config_t pc = {};
  pc.callback = kbdCb;
  pc.ep_desc = &gKbdEpDesc;
  pc.dev_speed = speed;
  pc.dev_addr = d.addr;
  if (hcd_pipe_alloc(gPort, &pc, &gKbdIn) != ESP_OK) { EWLOGE("USBH", "kbd pipe alloc failed"); return; }
  if (!gKbdUrb) gKbdUrb = urb_alloc(64, 0, 0);
  gKbdCtl = ctlPipe; gKbdDevIdx = devIdx;
  memset(gPrevKeys, 0, sizeof(gPrevKeys));
  gKbdUp = true;
  kbdSubmit();
  EWLOGI("USBH", "keyboard up addr=%u port=%u ep=0x%02x mps=%u interval=%u %s",
         d.addr, d.port, gKbdEpDesc.bEndpointAddress, gKbdEpDesc.wMaxPacketSize, gKbdEpDesc.bInterval,
         speed == USB_SPEED_LOW ? "LS" : "FS");
}

static void kbdService() {
  if (!gKbdDone) return;
  gKbdDone = false;
  urb_t *u;
  while ((u = hcd_urb_dequeue(gKbdIn)) != nullptr) {
    usb_transfer_t *t = &u->transfer;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) kbdReport(t->data_buffer, t->actual_num_bytes);
    else {
      EWLOGW("USBH", "kbd in status=%d", (int)t->status);
      if (hcd_pipe_get_state(gKbdIn) == HCD_PIPE_STATE_HALTED) hcd_pipe_command(gKbdIn, HCD_PIPE_CMD_CLEAR);
      if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) { gKbdUp = false; return; }
    }
    if (gKbdUp) kbdSubmit();
  }
}

static void kbdStop() {
  gKbdUp = false;
  pipeKill(gKbdIn);  gKbdIn = nullptr;
  if (gKbdCtl) { pipeKill(gKbdCtl); gKbdCtl = nullptr; }
  gKbdDevIdx = -1;
}

// ---------------------------------------------------------------------------
// Enumeration. Device answers at address 0 on a fresh EP0 pipe; we read the
// short device descriptor for bMaxPacketSize0, SET_ADDRESS, then the full
// device + config descriptors and strings, SET_CONFIGURATION, and decide what
// it is. Returns the gDev index or -1. *pipeOut gets the EP0 pipe when the
// caller needs to keep it (hub, keyboard); otherwise it's freed here.
// ---------------------------------------------------------------------------
struct EnumOut { hcd_pipe_handle_t pipe; int mps0; bool isHub; int kbdIf; usb_speed_t speed; };

static int enumerate(usb_speed_t speed, uint8_t addr, uint8_t port, EnumOut &out) {
  out = {};
  out.kbdIf = -1; out.speed = speed;
  hcd_pipe_config_t pc = {};
  pc.callback = ctlCb;
  pc.ep_desc = nullptr;                      // default pipe
  pc.dev_speed = speed;
  pc.dev_addr = 0;
  hcd_pipe_handle_t p;
  if (hcd_pipe_alloc(gPort, &pc, &p) != ESP_OK) { EWLOGE("USBH", "ep0 pipe alloc failed (port %u)", port); return -1; }
  static uint8_t buf[CTL_DATA_MAX];
  int mps = speed == USB_SPEED_LOW ? 8 : 64;
  int n = ctrl(p, mps, 0x80, 0x06, 0x0100, 0, 8, buf);           // short device descriptor
  if (n < 8) { EWLOGE("USBH", "port %u: get dev desc(8) r=%d", port, n); pipeKill(p); return -1; }
  mps = buf[7] ? buf[7] : 8;
  hcd_pipe_update_mps(p, mps);
  n = ctrl(p, mps, 0x00, 0x05, addr, 0, 0, nullptr);             // SET_ADDRESS
  if (n < 0) { EWLOGE("USBH", "port %u: set_address r=%d", port, n); pipeKill(p); return -1; }
  vTaskDelay(pdMS_TO_TICKS(10));                                 // set-address recovery
  hcd_pipe_update_dev_addr(p, addr);

  UsbDev d = {};
  d.addr = addr; d.port = port; d.speed = speed == USB_SPEED_LOW ? 0 : 1;
  n = ctrl(p, mps, 0x80, 0x06, 0x0100, 0, 18, buf);
  if (n < 18) { EWLOGE("USBH", "addr %u: get dev desc r=%d", addr, n); pipeKill(p); return -1; }
  d.bcdUSB = buf[2] | (buf[3] << 8);
  d.cls = buf[4]; d.sub = buf[5]; d.proto = buf[6];
  d.vid = buf[8] | (buf[9] << 8); d.pid = buf[10] | (buf[11] << 8);
  uint8_t iMfr = buf[14], iProd = buf[15];
  if (iMfr && (n = ctrl(p, mps, 0x80, 0x06, 0x0300 | iMfr, 0x0409, 64, buf)) > 0) strDesc(buf, n, d.mfr, sizeof(d.mfr));
  if (iProd && (n = ctrl(p, mps, 0x80, 0x06, 0x0300 | iProd, 0x0409, 64, buf)) > 0) strDesc(buf, n, d.prod, sizeof(d.prod));

  n = ctrl(p, mps, 0x80, 0x06, 0x0200, 0, 9, buf);              // config descriptor header
  if (n < 9) { EWLOGE("USBH", "addr %u: get cfg desc r=%d", addr, n); pipeKill(p); return -1; }
  int total = buf[2] | (buf[3] << 8);
  uint8_t cfgVal = buf[5];
  if (total > (int)sizeof(buf) - 64) total = sizeof(buf) - 64;   // room for MPS round-up
  n = ctrl(p, mps, 0x80, 0x06, 0x0200, 0, total, buf);
  if (n < 9) { EWLOGE("USBH", "addr %u: get full cfg r=%d", addr, n); pipeKill(p); return -1; }

  EWLOGI("USBH", "dev addr=%u port=%u vid=%04x pid=%04x usb=%x.%02x cls=%02x/%02x/%02x speed=%s mps0=%d mfr=\"%s\" prod=\"%s\"",
         addr, port, d.vid, d.pid, d.bcdUSB >> 8, d.bcdUSB & 0xFF, d.cls, d.sub, d.proto,
         d.speed ? "full" : "low", mps, d.mfr, d.prod);
  bool firstIf = true, curKbd = false; int curIf = -1;
  for (int i = 0; i + 2 <= n && buf[i] >= 2; i += buf[i]) {
    const uint8_t *q = buf + i;
    if (q[1] == 0x04 && i + 9 <= n) {
      curIf = q[2];
      curKbd = (q[5] == 3 && q[6] == 1 && q[7] == 1 && q[3] == 0);
      if (firstIf) { d.ifCls = q[5]; d.ifSub = q[6]; d.ifProto = q[7]; firstIf = false; }
      EWLOGI("USBH", "  if=%u alt=%u cls=%02x/%02x/%02x eps=%u (%s)", q[2], q[3], q[5], q[6], q[7], q[4],
             usbhClassName(q[5], q[6], q[7]));
    } else if (q[1] == 0x05 && i + 7 <= n && curKbd && out.kbdIf < 0 && (q[2] & 0x80) && (q[3] & 3) == 3) {
      out.kbdIf = curIf;
      memcpy(&gKbdEpDesc, q, 7);
    }
  }
  n = ctrl(p, mps, 0x00, 0x09, cfgVal, 0, 0, nullptr);           // SET_CONFIGURATION
  if (n < 0) EWLOGW("USBH", "addr %u: set_config r=%d", addr, n);

  out.isHub = (d.cls ? d.cls : d.ifCls) == 0x09;
  out.mps0 = mps;
  int idx = -1;
  xSemaphoreTake(gMux, portMAX_DELAY);
  if (gCount < MAX_DEV) { idx = gCount; gDev[gCount++] = d; }
  gGen++;
  xSemaphoreGive(gMux);
  if (out.isHub || out.kbdIf >= 0) out.pipe = p;                 // caller keeps it
  else pipeKill(p);                                              // pipe budget
  return idx;
}

static void markGone(int idx) {
  if (idx < 0) return;
  xSemaphoreTake(gMux, portMAX_DELAY);
  gDev[idx].gone = true; gGen++;
  xSemaphoreGive(gMux);
  EWLOGI("USBH", "gone addr=%u port=%u", gDev[idx].addr, gDev[idx].port);
}

// After enumeration: start the keyboard if it is one; otherwise drop the pipe.
static void adopt(int idx, EnumOut &eo) {
  if (idx >= 0 && eo.kbdIf >= 0 && !gKbdUp) { kbdStart(idx, eo.pipe, eo.mps0, (uint8_t)eo.kbdIf, eo.speed); return; }
  if (!eo.isHub && eo.pipe) { pipeKill(eo.pipe); eo.pipe = nullptr; }
}

// ---------------------------------------------------------------------------
// External hub (one level). Ports are polled with GET_PORT_STATUS every
// 250 ms instead of using the hub's status-change endpoint: one pipe fewer.
// ---------------------------------------------------------------------------
static hcd_pipe_handle_t gHubPipe = nullptr;
static int     gHubMps = 8, gHubPorts = 0, gHubIdx = -1;
static int     gPortDev[MAX_HUB_PORTS + 1];
static uint8_t gPortTries[MAX_HUB_PORTS + 1];
static uint8_t gNextAddr = 2;

static bool hubPortStatus(int port, uint16_t &st, uint16_t &ch) {
  uint8_t b[4];
  if (ctrl(gHubPipe, gHubMps, 0xA3, 0x00, 0, port, 4, b) < 4) return false;
  st = b[0] | (b[1] << 8); ch = b[2] | (b[3] << 8);
  return true;
}

static void hubStart(int idx, EnumOut &eo) {
  gHubPipe = eo.pipe; gHubMps = eo.mps0; gHubIdx = idx;
  uint8_t b[16];
  int n = ctrl(gHubPipe, gHubMps, 0xA0, 0x06, 0x2900, 0, 9, b);  // hub descriptor
  if (n < 7) { EWLOGE("USBH", "hub desc r=%d", n); return; }
  gHubPorts = b[2] > MAX_HUB_PORTS ? MAX_HUB_PORTS : b[2];
  int pgood = b[5] * 2;
  for (int p = 0; p <= MAX_HUB_PORTS; p++) { gPortDev[p] = -1; gPortTries[p] = 0; }
  for (int p = 1; p <= gHubPorts; p++) ctrl(gHubPipe, gHubMps, 0x23, 0x03, 8, p, 0, nullptr);  // PORT_POWER
  vTaskDelay(pdMS_TO_TICKS(pgood > 100 ? pgood : 100));
  EWLOGI("USBH", "hub up: %d ports (of %u), power-good %d ms", gHubPorts, b[2], pgood);
}

static void hubPoll() {
  for (int p = 1; p <= gHubPorts && gRun; p++) {
    uint16_t st, ch;
    if (!hubPortStatus(p, st, ch)) continue;
    if (ch & 0x0001) ctrl(gHubPipe, gHubMps, 0x23, 0x01, 16, p, 0, nullptr);   // clear C_PORT_CONNECTION
    bool conn = st & 0x0001;
    if (!conn) {
      if (gPortDev[p] >= 0) {
        if (gPortDev[p] == gKbdDevIdx) kbdStop();
        markGone(gPortDev[p]);
        gPortDev[p] = -1;
      }
      gPortTries[p] = 0;
      continue;
    }
    if (gPortDev[p] >= 0 || gPortTries[p] >= 3) continue;
    gPortTries[p]++;
    vTaskDelay(pdMS_TO_TICKS(100));                                // attach debounce
    ctrl(gHubPipe, gHubMps, 0x23, 0x03, 4, p, 0, nullptr);         // PORT_RESET
    bool done = false;
    for (int i = 0; i < 25 && !done; i++) {
      vTaskDelay(pdMS_TO_TICKS(20));
      if (hubPortStatus(p, st, ch) && (ch & 0x0010)) done = true;
    }
    ctrl(gHubPipe, gHubMps, 0x23, 0x01, 20, p, 0, nullptr);        // clear C_PORT_RESET
    if (!done || !(st & 0x0002)) { EWLOGW("USBH", "hub port %d: reset failed st=%04x", p, st); continue; }
    vTaskDelay(pdMS_TO_TICKS(20));                                 // reset recovery
    usb_speed_t sp = (st & 0x0200) ? USB_SPEED_LOW : USB_SPEED_FULL;
    EWLOGI("USBH", "hub port %d: device attached, %s", p, sp == USB_SPEED_LOW ? "low speed" : "full/high speed");
    EnumOut eo;
    int idx = enumerate(sp, gNextAddr++, p, eo);
    if (idx < 0) continue;
    gPortDev[p] = idx;
    if (eo.isHub) { EWLOGW("USBH", "nested hub on port %d not supported", p); pipeKill(eo.pipe); continue; }
    adopt(idx, eo);
  }
}

// ---------------------------------------------------------------------------
// Root port + task
// ---------------------------------------------------------------------------
static int gRootIdx = -1;

static void freeAllDevicePipes() {
  kbdStop();
  if (gHubPipe) { pipeKill(gHubPipe); gHubPipe = nullptr; }
  gHubPorts = 0; gHubIdx = -1;
}

static void rootConnected() {
  if (hcd_port_command(gPort, HCD_PORT_CMD_RESET) != ESP_OK) { EWLOGE("USBH", "root reset failed"); return; }
  usb_speed_t sp = USB_SPEED_FULL;
  hcd_port_get_speed(gPort, &sp);
  EWLOGI("USBH", "root: device attached, %s", sp == USB_SPEED_LOW ? "low speed" : "full speed");
  gNextAddr = 2;
  EnumOut eo;
  gRootIdx = enumerate(sp, 1, 0, eo);
  if (gRootIdx < 0) return;
  if (eo.isHub) hubStart(gRootIdx, eo);
  else adopt(gRootIdx, eo);
}

static void rootGone() {
  freeAllDevicePipes();
  xSemaphoreTake(gMux, portMAX_DELAY);
  for (int i = 0; i < gCount; i++) gDev[i].gone = true;
  gGen++;
  xSemaphoreGive(gMux);
  gRootIdx = -1;
  EWLOGI("USBH", "root: disconnected");
  hcd_port_recover(gPort);
  hcd_port_command(gPort, HCD_PORT_CMD_POWER_ON);
}

static void restoreSerialJtag() {
  // Undo the host pad config (D+/D- pulldowns forced by the OTG PHY setup) and
  // hand the internal PHY back to USB-Serial/JTAG with its D+ pullup, so the Mac
  // re-enumerates the console/flash port.
  USB_WRAP.otg_conf.pad_pull_override = 0;
  USB_WRAP.otg_conf.dp_pulldown = 0;
  USB_WRAP.otg_conf.dm_pulldown = 0;
  USB_WRAP.otg_conf.pad_enable = 0;
  // = usb_phy_ll_int_jtag_enable() (that header is C-only: volatile struct copy)
  USB_SERIAL_JTAG.conf0.phy_sel = 0;
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
  USB_SERIAL_JTAG.conf0.dp_pullup = 1;
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
  RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
  RTCCNTL.usb_conf.sw_usb_phy_sel = 0;
}

static void hostTask(void *) {
  usb_phy_handle_t phy = nullptr;
  bool hcdUp = false;
  esp_err_t e;
  uint32_t lastPoll = 0;

  usb_phy_config_t pc = {};
  pc.controller = USB_PHY_CTRL_OTG;
  pc.target = USB_PHY_TARGET_INT;
  pc.otg_mode = USB_OTG_MODE_HOST;
  pc.otg_speed = USB_PHY_SPEED_UNDEFINED;
  e = usb_new_phy(&pc, &phy);
  if (e != ESP_OK) { snprintf(gErr, sizeof(gErr), "phy: %s", esp_err_to_name(e)); goto fail; }
  {
    hcd_config_t hc = {};
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
    e = hcd_install(&hc);
    if (e != ESP_OK) { snprintf(gErr, sizeof(gErr), "hcd: %s", esp_err_to_name(e)); goto fail; }
    hcdUp = true;
    hcd_port_config_t pcfg = {};
    pcfg.fifo_bias = HCD_PORT_FIFO_BIAS_BALANCED;
    pcfg.callback = portCb;
    e = hcd_port_init(1, &pcfg, &gPort);
    if (e != ESP_OK) { snprintf(gErr, sizeof(gErr), "port: %s", esp_err_to_name(e)); goto fail; }
    if (!gCtlUrb) gCtlUrb = urb_alloc(sizeof(usb_setup_packet_t) + CTL_DATA_MAX, 0, 0);
    hcd_port_command(gPort, HCD_PORT_CMD_POWER_ON);
  }

  gSt = St::Scanning;
  gGen++;
  EWLOGI("USBH", "host up (HCD), window %lu s (0 = until stopped)", (unsigned long)(gWindowMs / 1000));
  while (gRun && (gWindowMs == 0 || (int32_t)(gDeadline - millis()) > 0)) {
    if (gPortEv) {
      gPortEv = false;
      hcd_port_event_t ev = hcd_port_handle_event(gPort);
      if (ev == HCD_PORT_EVENT_CONNECTION) rootConnected();
      else if (ev == HCD_PORT_EVENT_DISCONNECTION || ev == HCD_PORT_EVENT_ERROR ||
               ev == HCD_PORT_EVENT_OVERCURRENT) rootGone();
    }
    if (gHubPipe && millis() - lastPoll > 250) { lastPoll = millis(); hubPoll(); }
    if (gKbdUp) { kbdService(); gDeadline = millis() + gWindowMs; }   // keyboard in use: stay host
    vTaskDelay(pdMS_TO_TICKS(4));
  }

  gSt = St::Stopping;
  gGen++;
  freeAllDevicePipes();
  hcd_port_command(gPort, HCD_PORT_CMD_POWER_OFF);
  for (int i = 0; i < 20 && hcd_port_get_state(gPort) != HCD_PORT_STATE_NOT_POWERED; i++) {
    if (gPortEv) { gPortEv = false; hcd_port_handle_event(gPort); }
    if (hcd_port_get_state(gPort) == HCD_PORT_STATE_RECOVERY) { hcd_port_recover(gPort); break; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  hcd_port_deinit(gPort); gPort = nullptr;
  hcd_uninstall();
  usb_del_phy(phy);
  restoreSerialJtag();
  EWLOGI("USBH", "host down, serial restored, devices=%d keys=%lu", gCount, (unsigned long)gKeyCount);
  gSt = St::Done;
  gGen++;
  gTask = nullptr;
  vTaskDelete(nullptr);
  return;

fail:
  EWLOGE("USBH", "start failed: %s", gErr);
  if (gPort) { hcd_port_deinit(gPort); gPort = nullptr; }
  if (hcdUp) hcd_uninstall();
  if (phy) usb_del_phy(phy);
  restoreSerialJtag();
  gSt = St::Failed;
  gGen++;
  gTask = nullptr;
  vTaskDelete(nullptr);
}

void usbhStart(uint32_t windowMs) {
  if (gTask) return;
  if (!gKeyQ) gKeyQ = xQueueCreate(16, sizeof(UsbKey));
  xQueueReset(gKeyQ);
  gWindowMs = windowMs;
  if (!gMux) gMux = xSemaphoreCreateMutex();
  if (!gCtlSem) gCtlSem = xSemaphoreCreateBinary();
  xSemaphoreTake(gMux, portMAX_DELAY);
  gCount = 0; gErr[0] = 0;
  gTextLen = 0; gText[0] = 0; gKeyCount = 0;
  xSemaphoreGive(gMux);
  gPortEv = false; gKbdDone = false; gKbdUp = false; gRootIdx = -1;
  gDeadline = millis() + windowMs;
  gRun = true;
  gSt = St::Starting;
  gGen++;
  EWLOGI("USBH", "host start: USB serial goes away (window %lu s, 0 = until stopped)", (unsigned long)(windowMs / 1000));
  delay(150);                         // let that line reach the Mac before the port drops
  xTaskCreatePinnedToCore(hostTask, "usbhost", 8192, nullptr, 5, &gTask, 1);
}

// Ends the window early and waits for the PHY to be back on Serial/JTAG.
void usbhStop() {
  if (!gTask) return;
  gRun = false;
  for (int i = 0; i < 80 && gTask; i++) delay(50);
}

// ---------------------------------------------------------------------------
// Accessors for views
// ---------------------------------------------------------------------------
static void ensureMux() { if (!gMux) gMux = xSemaphoreCreateMutex(); }

bool        usbhRunning()  { return gTask != nullptr; }
UsbhSt      usbhState()    { return gSt; }
const char *usbhError()    { return gErr; }
uint32_t    usbhGen()      { return gGen; }
bool        usbhKbdUp()    { return gKbdUp; }
int         usbhCount()    { return gCount; }
uint32_t    usbhKeyCount() { return gKeyCount; }
int usbhSecsLeft() {
  if (gSt != UsbhSt::Scanning || gWindowMs == 0) return -1;
  int32_t ms = (int32_t)(gDeadline - millis());
  return ms > 0 ? ms / 1000 : 0;
}
int usbhSnapshot(UsbDev *out, int max) {
  ensureMux();
  xSemaphoreTake(gMux, portMAX_DELAY);
  int n = gCount < max ? gCount : max;
  memcpy(out, gDev, sizeof(UsbDev) * n);
  xSemaphoreGive(gMux);
  return n;
}
int usbhText(char *out, int cap) {
  ensureMux();
  xSemaphoreTake(gMux, portMAX_DELAY);
  int n = gTextLen < cap - 1 ? gTextLen : cap - 1;
  memcpy(out, gText, n); out[n] = 0;
  xSemaphoreGive(gMux);
  return n;
}
bool usbhGetKey(UsbKey &k) { return gKeyQ && xQueueReceive(gKeyQ, &k, 0) == pdTRUE; }
