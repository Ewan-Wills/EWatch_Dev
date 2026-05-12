#include <Preferences.h>
#include "model.h"
#include "storage.h"

static Preferences prefs;
static const char *NS = "ewatch";

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
}

void Storage::save() {
  bool t, b, i;
  uint16_t to, off;
  uint8_t  th, br;
  uint16_t bg, fg;
  { ModelLock lk;
    t   = model.wakeOnTouch;
    b   = model.wakeOnButton;
    i   = model.wakeOnImu;
    to  = model.sleepTimeoutSec;
    off = model.sleepToOffSec;
    th  = model.imuWakeThreshold;
    br  = model.brightness;
    bg  = model.bgColor;
    fg  = model.fgColor; }
  prefs.putBool  ("wkTouch",   t);
  prefs.putBool  ("wkButton",  b);
  prefs.putBool  ("wkImu",     i);
  prefs.putUShort("sleepSec",  to);
  prefs.putUShort("sleepOff",  off);
  prefs.putUChar ("imuThresh", th);
  prefs.putUChar ("bright",    br);
  prefs.putUShort("bgColor",   bg);
  prefs.putUShort("fgColor",   fg);
}
