// Shared Bluedroid lifetime. Several features use BLE (Bluetooth remote app,
// BLE mouse); the stack comes up on the first acquire and goes down on the
// last release, so one feature closing can't pull the radio from the other.
// First acquire also suspends WiFi (its driver RAM goes to the BLE
// controller) and pins the CPU at 240 MHz; the last release undoes both.
#pragma once
#include <Arduino.h>

// devName: the GAP device name if this acquire brings the stack up. Default =
// the BT remote's name, which phones pair with, whoever starts the stack.
bool bleStackAcquire(const char *who, const char *devName = "EWatch Remote");   // false if bluedroid failed to start
void bleStackRelease(const char *who);
int  bleStackUsers();

// Link quality of a connected peer: ask (non-blocking), then read the latest
// answer if it is no older than maxAgeMs.
void bleRequestRssi(const uint8_t bda[6]);
bool bleLastRssi(int8_t &rssi, uint32_t maxAgeMs);
