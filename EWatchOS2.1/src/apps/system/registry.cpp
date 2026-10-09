// View registry / dispatch — EWatchOS edition. The launcher tables, the
// Screen -> View mapping, and switchTo(). Add a new app by writing its view
// file, giving it a Screen enum entry (model.h), and one row in a table here.
#include "display.h"
#include "ui_kit.h"
#include "ui_style.h"
#include "launcher.h"
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
#include "bluetooth.h"
#if EW_APP_WAKESOAK
#include "wake_soak.h"
#endif  // EW_APP_WAKESOAK
#include "particles.h"
#include "boids.h"
#include "tunnel.h"
#include "starfox_shooter.h"
#if EW_APP_PET
#include "petview.h"
#endif  // EW_APP_PET
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
  #include "island.h"
#endif
#if EW_APP_DOOM
#include "doom.h"
#endif  // EW_APP_DOOM
#include "gallery.h"
#if defined(EW_APP_SPOTIFY) && EW_APP_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  #include "spotify.h"
#endif

#if EW_APP_RACER
View *racerViewPtr();   // games/racer_view.cpp
#endif  // EW_APP_RACER
#if EW_APP_DOOM
View *doomViewPtr();    // games/doom_app.cpp
#endif  // EW_APP_DOOM

// ---------------------------------------------------------------------------
// Launcher tables. Sections render as dim headers above their first row.
// ---------------------------------------------------------------------------
static const AuraEntry kTopApps[] = {
#if defined(EW_APP_SPOTIFY) && EW_APP_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  { "Spotify",   Icon::Music,     aura::kGreen,  Screen::Spotify,     "APPS"  },
  { "Stopwatch", Icon::Stopwatch, aura::kOrange, Screen::Stopwatch,   nullptr },
#else
  { "Stopwatch", Icon::Stopwatch, aura::kOrange, Screen::Stopwatch,   "APPS"  },
#endif
  { "Timer",     Icon::Timer,     aura::kTeal,   Screen::Timer,       nullptr },
  { "Bluetooth", Icon::Bluetooth, aura::kBlue,   Screen::Bluetooth,   nullptr },
#if EW_APP_WAKESOAK
  { "Wake Soak", Icon::Info,      aura::kOrange, Screen::WakeSoak,    nullptr },
#endif  // EW_APP_WAKESOAK
#if defined(EWATCH_ENABLE_QR) && EWATCH_ENABLE_QR
  { "QR Share",  Icon::QR,        aura::kBlue,   Screen::QRCode,      nullptr },
#endif
  { "Photos",    Icon::Photo,     aura::kPink,   Screen::Gallery,     nullptr },
#if defined(EWATCH_ENABLE_MEDIA) && EWATCH_ENABLE_MEDIA
  { "Media",     Icon::Photo,     aura::kPurple, Screen::Media,       nullptr },
#endif
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
  { "3D Viewer", Icon::Cube,      aura::kPurple, Screen::Viewer3D,    nullptr },
#endif
#if EW_APP_DOOM
  { "Doom",      Icon::Skull,     aura::kRed,    Screen::Doom,        "GAMES" },
  { "Tunnel",    Icon::Tunnel,    aura::kTeal,   Screen::TunnelRacer, nullptr },
#else
  { "Tunnel",    Icon::Tunnel,    aura::kTeal,   Screen::TunnelRacer, "GAMES" },
#endif  // EW_APP_DOOM
  { "Starfox",   Icon::Ship,      aura::kBlue,   Screen::Starfox,     nullptr },
#if EW_APP_RACER
  { "Racer",     Icon::Car,       aura::kOrange, Screen::Racer,       nullptr },
#endif  // EW_APP_RACER
#if EW_APP_PET
  { "Pet",       Icon::Paw,       aura::kPink,   Screen::Tamagotchi,  nullptr },
#endif  // EW_APP_PET
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
  { "Island",    Icon::Island,    aura::kGreen,  Screen::Island,      nullptr },
#endif
  { "Particles", Icon::Sparkle,   aura::kYellow, Screen::Particles,   "TOYS"  },
  { "Flock",     Icon::Bird,      aura::kTeal,   Screen::Boids,       nullptr },
  { "Settings",  Icon::Gear,      aura::kLine,   Screen::Settings,    "SYSTEM" },
  { "System",    Icon::Wrench,    aura::kLine,   Screen::SystemApps,  nullptr },
};

static const AuraEntry kSystemApps[] = {
  { "Sensor Test",    Icon::Info,    aura::kTeal,   Screen::SensorTest,    nullptr },
  { "Touch Gestures", Icon::Sparkle, aura::kBlue,   Screen::TouchGestures, nullptr },
  { "IMU Gestures",   Icon::Cube,    aura::kPurple, Screen::ImuGestures,   nullptr },
  { "Power Off",      Icon::Power,   aura::kRed,    Screen::PowerOff,      nullptr },
};

static const AuraEntry kSettingsEntries[] = {
  { "Time",    Icon::Clock,   aura::kBlue,   Screen::SettingsTime,    nullptr },
  { "Date",    Icon::Info,    aura::kBlue,   Screen::SettingsDate,    nullptr },
  { "Sleep",   Icon::Moon,    aura::kPurple, Screen::SettingsSleep,   nullptr },
  { "Wake",    Icon::Sun,     aura::kOrange, Screen::SettingsWake,    nullptr },
  { "Display", Icon::Torch,   aura::kYellow, Screen::SettingsDisplay, nullptr },
  { "Faces",   Icon::Clock,   aura::kTeal,   Screen::SettingsFont,    nullptr },
  { "Haptics", Icon::Vibrate, aura::kPink,   Screen::SettingsHaptics, nullptr },
#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  { "WiFi",    Icon::Wifi,    aura::kGreen,  Screen::SettingsWifi,    nullptr },
#endif
  { "Memory",  Icon::Memory,  aura::kLine,   Screen::SettingsMemory,  nullptr },
};

static AuraListView vAppList(kTopApps,
                             sizeof(kTopApps) / sizeof(kTopApps[0]),
                             "Apps", Screen::Watch, Trans::CurtainDown);
static AuraListView vSystemApps(kSystemApps,
                                sizeof(kSystemApps) / sizeof(kSystemApps[0]),
                                "System", Screen::AppList);
static AuraListView vSettings(kSettingsEntries,
                              sizeof(kSettingsEntries) / sizeof(kSettingsEntries[0]),
                              "Settings", Screen::AppList);

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
static BluetoothView vBluetooth;
#if EW_APP_WAKESOAK
static WakeSoakView vWakeSoak;
#endif  // EW_APP_WAKESOAK
static ParticlesView vParticles;
static BoidsView     vBoids;
static TunnelRacerView    vTunnel;
static StarfoxShooterView vStarfox;
#if EW_APP_PET
static PetView            vPet;
#endif  // EW_APP_PET
static GalleryView        vGallery;
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
static IslandView         vIsland;
#endif
#if defined(EW_APP_SPOTIFY) && EW_APP_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
static SpotifyView        vSpotify;
#endif

View *currentView = nullptr;

View *viewFor(Screen s) {
  switch (s) {
    case Screen::Watch:           return watchFaceViewPtr();
    case Screen::ScreenSaver:     return screenSaverViewPtr();
    case Screen::AppList:         return &vAppList;
    case Screen::SystemApps:      return &vSystemApps;
    case Screen::Settings:        return &vSettings;
    case Screen::QuickSettings:   return quickSettingsViewPtr();
    case Screen::Torch:           return torchViewPtr();
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
    case Screen::Bluetooth:       return &vBluetooth;
#if EW_APP_WAKESOAK
    case Screen::WakeSoak:        return &vWakeSoak;
#endif  // EW_APP_WAKESOAK
    case Screen::Particles:       return &vParticles;
    case Screen::Boids:           return &vBoids;
    case Screen::TunnelRacer:     return &vTunnel;
    case Screen::Starfox:         return &vStarfox;
#if EW_APP_PET
    case Screen::Tamagotchi:      return &vPet;
#endif  // EW_APP_PET
    case Screen::Gallery:         return &vGallery;
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
    case Screen::Island:          return &vIsland;
#endif
#if EW_APP_RACER
    case Screen::Racer:           return racerViewPtr();
#endif  // EW_APP_RACER
#if EW_APP_DOOM
    case Screen::Doom:            return doomViewPtr();
#endif  // EW_APP_DOOM
#if defined(EW_APP_SPOTIFY) && EW_APP_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
    case Screen::Spotify:         return &vSpotify;
#endif
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
