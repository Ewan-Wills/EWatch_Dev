// Background WiFi service. Owns the radio and an HTTP server (in AP mode).
//
// State machine is driven by model.wifiEnabled + model.wifiMode:
//   disabled         -> radio off
//   enabled + AP     -> SoftAP "EWATCH_SETUP" + http://192.168.4.1/ (settings)
//   enabled + Client -> periodic scan, connect to strongest known SSID
//
// Status is mirrored into model.wifiConnected / wifiRssi / wifiSsid /
// wifiApClients so the watch face icon and settings views can read it.
//
// The service runs in its own FreeRTOS task; views never block the radio.
#pragma once
#include <stdint.h>

void wifiSvcInit();          // call once after Storage::load(), before tasks
void wifiSvcStartTask();     // spawn the background task

// Capacity / age of the last STA scan results that the web form exposes.
// Populated either on a periodic client-mode scan or via wifiSvcRequestScan().
// Used by the AP web form to offer a "pick from scan" SSID dropdown.
void wifiSvcRequestScan();

// ---- RadioManager (client-mode discipline) --------------------------------
// In client mode the radio is OFF by default. Every T_SYNC seconds (power
// config, NVS) — or on explicit user request — a sync window opens: connect,
// run all pending work (NTP + queued jobs), disconnect, radio off. Each
// session is logged under the WIFI tag. AP mode is exempt: hosting the setup
// portal is an explicit user action and stays up while enabled.

// True while the radio is up for any reason (AP up, window connecting/active).
bool     wifiSvcRadioActive();

// True while a client-mode sync window is in progress (scanning, connecting,
// or associated doing work). The power manager keeps the screen awake for
// the duration so the watch doesn't sleep mid-connect. AP mode does NOT
// count — it's persistent and would block sleep forever.
bool     wifiSvcWindowActive();

// ms until the next scheduled sync window; 0 = due now, UINT32_MAX = none
// scheduled (radio disabled, AP mode, or a window already open). Used by the
// power manager to set its light-sleep timer horizon.
uint32_t wifiSvcNextWindowInMs();

// Open a sync window now (user action needing network).
void     wifiSvcKickWindow();

// Queue work for the next window instead of touching WiFi directly. The
// callback runs in the radio task while associated. Returns false if the
// queue is full (8 slots).
bool     wifiSvcQueueJob(void (*job)());

// Tear the radio down and keep it down (deep-sleep prep). Blocks up to
// waitMs for the radio task to finish the teardown.
void     wifiSvcForceOff(uint32_t waitMs);
