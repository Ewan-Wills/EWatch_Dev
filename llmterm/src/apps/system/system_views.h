// Accessors for the system views. Each view class is private to its own .cpp
// (state stays file-local); the registry only needs a View* per screen.
#pragma once
#include "view.h"

View *watchFaceViewPtr();        // watchface.cpp
View *screenSaverViewPtr();      // screensaver.cpp

View *settingsTimeViewPtr();     // settings_clock.cpp
View *settingsDateViewPtr();     // settings_clock.cpp

View *settingsSleepViewPtr();    // settings_system.cpp
View *settingsWakeViewPtr();     // settings_system.cpp
View *settingsDisplayViewPtr();  // settings_system.cpp
View *settingsFontViewPtr();     // settings_system.cpp
View *settingsHapticsViewPtr();  // settings_system.cpp
View *settingsMemoryViewPtr();   // settings_system.cpp

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
View *settingsWifiViewPtr();     // settings_wifi.cpp
View *settingsKnownNetsViewPtr();// settings_wifi.cpp
#endif

View *sensorTestViewPtr();       // diagnostics.cpp
View *touchGesturesViewPtr();    // diagnostics.cpp
View *imuGesturesViewPtr();      // diagnostics.cpp

View *powerOffViewPtr();         // poweroff.cpp
View *quickSettingsViewPtr();    // quickset.cpp
View *torchViewPtr();            // quickset.cpp
