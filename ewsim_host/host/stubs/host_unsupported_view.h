// ewsim_host: shared look for views whose hardware the simulator can't run.
#pragma once
#include <Arduino.h>
#include "display.h"
#include "host_sim.h"
static inline void ewsimDrawUnsupported(const char *app, const char *why) {
  if (!gfx) return;
  gfx->fillScreen(0xF81F);                       // magenta: impossible to mistake for real UI
  gfx->setTextColor(0xFFFF, 0xF81F);
  gfx->setTextSize(2);
  gfx->setCursor(12, 96);  gfx->print("NOT SIMULATED");
  gfx->setTextSize(1);
  gfx->setCursor(12, 130); gfx->print(app);
  gfx->setCursor(12, 146); gfx->print(why);
  gfx->setCursor(12, 178); gfx->print("ewsim_host: record this app");
  gfx->setCursor(12, 192); gfx->print("on the real watch (ewt record)");
}
