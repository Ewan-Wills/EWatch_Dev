// View registry / dispatch. App tables for the carousels, the Screen -> View
// mapping, and switchTo(). Add a new app by writing its view file, giving it
// a Screen enum entry (model.h), an accessor, and one row in a table here.
#include "display.h"
#include "ui_kit.h"
#include "carousel.h"
#include "system_views.h"
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
  #include "viewer3d.h"
#endif
#if defined(EWATCH_ENABLE_MEDIA) && EWATCH_ENABLE_MEDIA
  #include "media.h"
#endif
#if defined(EWATCH_ENABLE_QR) && EWATCH_ENABLE_QR
  #include "qr.h"
#endif
#include "stopwatch.h"
#include "timer_app.h"
#include "particles.h"
#include "boids.h"

// Top level: user-facing apps plus an entry into the System sub-page
// (settings, diagnostics, power off). Tiles paint in the theme accent.
static const AppEntry kTopApps[] = {
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
  { "3D Viewer", 0, Screen::Viewer3D    },
#endif
  { "Particles", 0, Screen::Particles   },
  { "Flock",     0, Screen::Boids       },
#if defined(EWATCH_ENABLE_MEDIA) && EWATCH_ENABLE_MEDIA
  { "Media",     0, Screen::Media       },
#endif
#if defined(EWATCH_ENABLE_QR) && EWATCH_ENABLE_QR
  { "QR Share",  0, Screen::QRCode      },
#endif
  { "Stopwatch", 0, Screen::Stopwatch   },
  { "Timer",     0, Screen::Timer       },
  { "System",    0, Screen::SystemApps  },
};
static const AppEntry kSystemApps[] = {
  { "Settings",       0, Screen::Settings      },
  { "Sensor Test",    0, Screen::SensorTest    },
  { "Touch Gestures", 0, Screen::TouchGestures },
  { "IMU Gestures",   0, Screen::ImuGestures   },
  { "Power Off",      0, Screen::PowerOff      },
};
static const AppEntry kSettingsEntries[] = {
  { "Time",    0, Screen::SettingsTime    },
  { "Date",    0, Screen::SettingsDate    },
  { "Sleep",   0, Screen::SettingsSleep   },
  { "Wake",    0, Screen::SettingsWake    },
  { "Display", 0, Screen::SettingsDisplay },
  { "Font",    0, Screen::SettingsFont    },
  { "Haptics", 0, Screen::SettingsHaptics },
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  { "WiFi",    0, Screen::SettingsWifi    },
#endif
  { "Memory",  0, Screen::SettingsMemory  },
};

static CarouselView vAppList(kTopApps,
                             sizeof(kTopApps) / sizeof(kTopApps[0]),
                             "Apps", Screen::Watch, /*wrap=*/true);
static CarouselView vSystemApps(kSystemApps,
                                sizeof(kSystemApps) / sizeof(kSystemApps[0]),
                                "System", Screen::AppList, /*wrap=*/true);
static CarouselView vSettings(kSettingsEntries,
                              sizeof(kSettingsEntries) / sizeof(kSettingsEntries[0]),
                              "Settings", Screen::SystemApps, /*wrap=*/true);

#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
static Viewer3DView vViewer3D;
#endif
#if defined(EWATCH_ENABLE_MEDIA) && EWATCH_ENABLE_MEDIA
static MediaView vMedia;
#endif
#if defined(EWATCH_ENABLE_QR) && EWATCH_ENABLE_QR
static QRCodeView vQRCode;
#endif
static StopwatchView vStopwatch;
static TimerView     vTimer;
static ParticlesView vParticles;
static BoidsView     vBoids;

View *currentView = nullptr;

View *viewFor(Screen s) {
  switch (s) {
    case Screen::Watch:           return watchFaceViewPtr();
    case Screen::ScreenSaver:     return screenSaverViewPtr();
    case Screen::AppList:         return &vAppList;
    case Screen::SystemApps:      return &vSystemApps;
    case Screen::Settings:        return &vSettings;
    case Screen::SettingsTime:    return settingsTimeViewPtr();
    case Screen::SettingsDate:    return settingsDateViewPtr();
    case Screen::SettingsSleep:   return settingsSleepViewPtr();
    case Screen::SettingsWake:    return settingsWakeViewPtr();
    case Screen::SettingsDisplay: return settingsDisplayViewPtr();
    case Screen::SettingsFont:    return settingsFontViewPtr();
    case Screen::SettingsHaptics: return settingsHapticsViewPtr();
    case Screen::SettingsMemory:  return settingsMemoryViewPtr();
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
    case Screen::SettingsWifi:      return settingsWifiViewPtr();
    case Screen::SettingsKnownNets: return settingsKnownNetsViewPtr();
#endif
    case Screen::SensorTest:      return sensorTestViewPtr();
    case Screen::TouchGestures:   return touchGesturesViewPtr();
    case Screen::ImuGestures:     return imuGesturesViewPtr();
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
    case Screen::Viewer3D:        return &vViewer3D;
#endif
#if defined(EWATCH_ENABLE_MEDIA) && EWATCH_ENABLE_MEDIA
    case Screen::Media:           return &vMedia;
#endif
#if defined(EWATCH_ENABLE_QR) && EWATCH_ENABLE_QR
    case Screen::QRCode:          return &vQRCode;
#endif
    case Screen::Stopwatch:       return &vStopwatch;
    case Screen::Timer:           return &vTimer;
    case Screen::Particles:       return &vParticles;
    case Screen::Boids:           return &vBoids;
    case Screen::PowerOff:        return powerOffViewPtr();
    default:                      break;
  }
  return watchFaceViewPtr();
}

void switchTo(Screen s) {
  View *next = viewFor(s);
  if (currentView == next) return;
  if (currentView) currentView->onExit();
  { ModelLock lk; model.screen = s; model.revision++; }
  currentView = next;
  currentView->onEnter();
}

void viewsInit() {
  currentView = watchFaceViewPtr();
  if (gfx) uiClearAll();
  currentView->onEnter();
}
