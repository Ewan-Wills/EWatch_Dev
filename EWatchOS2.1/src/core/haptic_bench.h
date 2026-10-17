// Haptic bench: `hap ...` console commands and the accelerometer vibration
// meter (haptic_bench.cpp).
#pragma once

bool hapticConsole(const char *line);   // true if the line was a `hap` command
void hapticMeterPoll();                 // taskIO only: runs a queued meter
