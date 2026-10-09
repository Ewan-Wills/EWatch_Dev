// ewsim_host stub for ewmouse's src/core/ble_stack.cpp: bluedroid never starts.
#include "ble_stack.h"
bool bleStackAcquire(const char *) { return false; }
void bleStackRelease(const char *) {}
int bleStackUsers() { return 0; }
