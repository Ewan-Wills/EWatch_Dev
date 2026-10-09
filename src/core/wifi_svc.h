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

void wifiSvcInit();          // call once after Storage::load(), before tasks
void wifiSvcStartTask();     // spawn the background task

// Capacity / age of the last STA scan results that the web form exposes.
// Populated either on a periodic client-mode scan or via wifiSvcRequestScan().
// Used by the AP web form to offer a "pick from scan" SSID dropdown.
void wifiSvcRequestScan();
