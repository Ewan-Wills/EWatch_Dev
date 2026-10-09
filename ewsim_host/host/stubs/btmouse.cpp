// ewsim_host stub for ewmouse's src/apps/btmouse.cpp (BLE mouse client).
#include "btmouse.h"
#include "host_unsupported_view.h"
void mouseTouch(bool &, uint16_t &, uint16_t &) {}
Gesture mouseTakeGesture(uint16_t &, uint16_t &) { return Gesture::None; }
bool mouseButton() { return false; }
int mouseTakeGoto() { return -1; }
bool mouseTakeWake() { return false; }
bool mouseTakeActivity() { return false; }
bool mouseConnected() { return false; }
bool mouseCursorActive() { return false; }
void mouseCursorPaint() {}
void mouseBegin() {}
bool mouseHandleLine(char *) { return false; }
void MouseView::onEnter() { ewsimHostUnsupported("BT Mouse app (BLE)"); firstDraw = true; }
void MouseView::render() { if (firstDraw) { ewsimDrawUnsupported("BT Mouse", "needs the BLE radio + a mouse"); firstDraw = false; } }
void MouseView::onEvent(const Event &) {}
