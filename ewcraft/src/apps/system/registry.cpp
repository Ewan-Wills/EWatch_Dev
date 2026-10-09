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
#include "craft.h"
#include "xbox_pad.h"
#include "particles.h"
#include "boids.h"
#include "tunnel.h"
#include "starfox_shooter.h"
#include "petview.h"
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
  #include "island.h"
#endif
#include "doom.h"
#include "gallery.h"
#if defined(EWATCH_ENABLE_SPOTIFY) && EWATCH_ENABLE_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  #include "spotify.h"
#endif

View *racerViewPtr();   // games/racer_view.cpp
View *doomViewPtr();    // games/doom_app.cpp

// ---------------------------------------------------------------------------
// Launcher tables. Sections render as dim headers above their first row.
// ---------------------------------------------------------------------------
static const AuraEntry kTopApps[] = {
#if defined(EWATCH_ENABLE_SPOTIFY) && EWATCH_ENABLE_SPOTIFY && \
    defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI
  { "Spotify",   Icon::Music,     aura::kGreen,  Screen::Spotify,     "APPS"  },
  { "Stopwatch", Icon::Stopwatch, aura::kOrange, Screen::Stopwatch,   nullptr },
#else
  { "Stopwatch", Icon::Stopwatch, aura::kOrange, Screen::Stopwatch,   "APPS"  },
#endif
  { "Timer",     Icon::Timer,     aura::kTeal,   Screen::Timer,       nullptr },
  { "Craft",     Icon::Cube,      aura::kGreen,  Screen::Craft,       nullptr },
  { "Xbox Pad",  Icon::Bluetooth, aura::kGreen,  Screen::Pad,         nullptr },
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
  { "Doom",      Icon::Skull,     aura::kRed,    Screen::Doom,        "GAMES" },
  { "Tunnel",    Icon::Tunnel,    aura::kTeal,   Screen::TunnelRacer, nullptr },
  { "Starfox",   Icon::Ship,      aura::kBlue,   Screen::Starfox,     nullptr },
  { "Racer",     Icon::Car,       aura::kOrange, Screen::Racer,       nullptr },
  { "Pet",       Icon::Paw,       aura::kPink,   Screen::Tamagotchi,  nullptr },
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
static CraftView vCraft;
static PadView vPad;
static ParticlesView vParticles;
static BoidsView     vBoids;
static TunnelRacerView    vTunnel;
static StarfoxShooterView vStarfox;
static PetView            vPet;
static GalleryView        vGallery;
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
static IslandView         vIsland;
#endif
#if defined(EWATCH_ENABLE_SPOTIFY) && EWATCH_ENABLE_SPOTIFY && \
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
    case Screen::Craft:           return &vCraft;
    case Screen::Pad:             return &vPad;
    case Screen::Particles:       return &vParticles;
    case Screen::Boids:           return &vBoids;
    case Screen::TunnelRacer:     return &vTunnel;
    case Screen::Starfox:         return &vStarfox;
    case Screen::Tamagotchi:      return &vPet;
    case Screen::Gallery:         return &vGallery;
#if defined(EWATCH_ENABLE_ISLAND) && EWATCH_ENABLE_ISLAND
    case Screen::Island:          return &vIsland;
#endif
    case Screen::Racer:           return racerViewPtr();
    case Screen::Doom:            return doomViewPtr();
#if defined(EWATCH_ENABLE_SPOTIFY) && EWATCH_ENABLE_SPOTIFY && \
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
