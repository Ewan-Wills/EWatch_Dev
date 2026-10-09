// ewsim_host stub for src/core/wifi_svc.cpp: WiFi is not simulated. The radio
// is always off, there is no web UI, no NTP and no sync window. Views that show
// WiFi status render their "off" state.
#include "wifi_svc.h"
#include "host_sim.h"
void wifiSvcInit() {}
void wifiSvcStartTask() {}
void wifiSvcRequestScan() { ewsimHostNote("WiFi scan requested (not simulated: no networks)"); }
bool wifiSvcRadioActive() { return false; }
bool wifiSvcWindowActive() { return false; }
uint32_t wifiSvcNextWindowInMs() { return 0xFFFFFFFFu; }
void wifiSvcKickWindow() {}
void wifiSvcHold(bool) {}
bool wifiSvcQueueJob(void (*)()) { return false; }
void wifiSvcForceOff(uint32_t) {}
void wifiSvcSuspend(bool, uint32_t) {}
