// ewsim_host stub for src/apps/xbox_pad.cpp (NimBLE Xbox controller host).
// No controller is ever connected; Craft falls back to touch + attract mode.
// The Pad app itself is NOT simulated (fails the run unless --allow-unsupported).
#include "xbox_pad.h"
#include "host_unsupported_view.h"
void padBegin() {}
void padSetEnabled(bool) {}
bool padEnabled() { return false; }
PadState padGet() { PadState p; memset(&p, 0, sizeof p); return p; }
const char *padStatus() { return "not simulated"; }
uint32_t padGen() { return 0; }
void padRumble(uint8_t, uint16_t) {}
void PadView::onEnter() { ewsimHostUnsupported("Xbox Pad app (BLE controller)"); firstDraw = true; }
void PadView::render() { if (firstDraw) { ewsimDrawUnsupported("Xbox Pad", "needs the BLE radio + controller"); firstDraw = false; } }
void PadView::onEvent(const Event &) {}
