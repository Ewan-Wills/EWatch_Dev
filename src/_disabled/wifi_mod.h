// Wi-Fi smoke test helpers. ESP32-S3 has a single 2.4 GHz radio shared with BT.
// Naming: wifi_mod.* (not wifi.*) to avoid clashing with the Arduino WiFi.h header.
#pragma once
#include <stdint.h>
#include <Arduino.h>

void wifiBegin();                          // station mode, radio on, disconnected
int  wifiScan();                           // blocking scan, returns AP count (-1 on error)
String wifiTopSSID(int8_t &rssiOut);       // strongest AP from last scan ("" if none)

// Optional: connect to a known network. Returns true on success within timeoutMs.
bool wifiConnect(const char *ssid, const char *pass, uint32_t timeoutMs = 10000);
void wifiDisconnect();
bool wifiIsConnected();
String wifiLocalIP();

// ---- Access Point + tiny web UI ---------------------------------------
// Hosts an AP and a web server on http://192.168.4.1/ with two forms:
//   - "message" : text shown on screen via apTakeMessage()
//   - "time"    : sets RV-3028 hours/minutes/seconds (BCD)
// pass may be nullptr/"" for an open network (else >= 8 chars for WPA2).
void apBegin(const char *ssid, const char *pass = nullptr);
void apHandle();                                 // call from loop()
String apIP();
bool apTakeMessage(String &out);
bool apIsUp();                                   // true if SoftAP interface is up
uint8_t apClientCount();                         // associated stations                 // returns true once per new message
