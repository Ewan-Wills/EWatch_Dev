#include <Arduino.h>
#include <Preferences.h>
#include "model.h"
#include "storage.h"

static Preferences prefs;
// Own NVS namespace — deliberately NOT "ewatch" (FirstOS's), so BasicDigital
// never inherits FirstOS's saved settings (e.g. its 30 s auto-power-off, which
// made the watch power fully off in deep sleep and look dead). First boot finds
// this namespace empty, so the model defaults in model.h apply.
static const char *NS = "basicd";

void Storage::begin() {
  prefs.begin(NS, /*readOnly=*/false);
}

void Storage::load() {
  ModelLock lk;
  model.wakeOnTouch      = prefs.getBool  ("wkTouch",   model.wakeOnTouch);
  model.wakeOnButton     = prefs.getBool  ("wkButton",  model.wakeOnButton);
  model.wakeOnImu        = prefs.getBool  ("wkImu",     model.wakeOnImu);
  model.sleepTimeoutSec  = prefs.getUShort("sleepSec",  model.sleepTimeoutSec);
  model.sleepToOffSec    = prefs.getUShort("sleepOff",  model.sleepToOffSec);
  model.imuWakeThreshold = prefs.getUChar ("imuThresh", model.imuWakeThreshold);
  model.brightness       = prefs.getUChar ("bright",    model.brightness);
  model.bgColor          = prefs.getUShort("bgColor",   model.bgColor);
  model.fgColor          = prefs.getUShort("fgColor",   model.fgColor);
  model.accentColor      = prefs.getUShort("accColor",  model.accentColor);
  model.lineColor        = prefs.getUShort("lineColor", model.lineColor);
  model.hapticStrength   = prefs.getUChar ("haptStr",   model.hapticStrength);
}

void Storage::save() {
  bool t, b, i;
  uint16_t to, off, bg, fg, ac, ln;
  uint8_t  th, br, hap;
  { ModelLock lk;
    t   = model.wakeOnTouch;
    b   = model.wakeOnButton;
    i   = model.wakeOnImu;
    to  = model.sleepTimeoutSec;
    off = model.sleepToOffSec;
    th  = model.imuWakeThreshold;
    br  = model.brightness;
    bg  = model.bgColor;
    fg  = model.fgColor;
    ac  = model.accentColor;
    ln  = model.lineColor;
    hap = model.hapticStrength; }
  prefs.putBool  ("wkTouch",   t);
  prefs.putBool  ("wkButton",  b);
  prefs.putBool  ("wkImu",     i);
  prefs.putUShort("sleepSec",  to);
  prefs.putUShort("sleepOff",  off);
  prefs.putUChar ("imuThresh", th);
  prefs.putUChar ("bright",    br);
  prefs.putUShort("bgColor",   bg);
  prefs.putUShort("fgColor",   fg);
  prefs.putUShort("accColor",  ac);
  prefs.putUShort("lineColor", ln);
  prefs.putUChar ("haptStr",   hap);
}
