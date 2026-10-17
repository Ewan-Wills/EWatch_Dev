// The role -> effect table. Change a feel for the whole watch here.
// Rules behind the choices (details in HAPTICS.md):
//   * the more often it happens, the shorter and quieter it is (Tick/Click)
//   * results get a two-part shape: rising = good, hard triple = bad
//   * things you might not be looking at get rhythm, so they can't be
//     mistaken for a touch reply
//   * games may be strong, but never hold full drive (driver caps it)
#include <stddef.h>
#include "haptic_roles.h"

namespace {
struct RoleRow { HapticRole role; const char *name; Haptic fx; const char *use; };

constexpr RoleRow kRoles[] = {
  { HapticRole::Tap,          "Tap",          Haptic::Click,       "button or tile press" },
  { HapticRole::Open,         "Open",         Haptic::Click,       "open an app, item or detail view" },
  { HapticRole::Back,         "Back",         Haptic::Tick,        "back, close, dismiss" },
  { HapticRole::Scroll,       "Scroll",       Haptic::Tick,        "list detent, page flip, carousel step" },
  { HapticRole::Adjust,       "Adjust",       Haptic::Tick,        "+/- step, slider notch, held ramp" },
  { HapticRole::ModeChange,   "ModeChange",   Haptic::DoubleClick, "switch mode, tab or view style" },
  { HapticRole::ToggleOn,     "ToggleOn",     Haptic::ToggleOn,    "switched on, started, armed" },
  { HapticRole::ToggleOff,    "ToggleOff",    Haptic::ToggleOff,   "switched off, stopped, cancelled" },
  { HapticRole::LongPress,    "LongPress",    Haptic::LongPress,   "a hold was recognised" },
  { HapticRole::Destructive,  "Destructive",  Haptic::Thud,        "delete, clear, reset (after confirm)" },
  { HapticRole::Confirm,      "Confirm",      Haptic::Success,     "saved, sent, applied" },
  { HapticRole::Refused,      "Refused",      Haptic::Knock,       "can't do that: invalid, nothing to do" },
  { HapticRole::Failed,       "Failed",       Haptic::Error,       "tried and failed: not connected, I/O" },
  { HapticRole::MessageIn,    "MessageIn",    Haptic::Notify,      "message or notification arrived" },
  { HapticRole::Reminder,     "Reminder",     Haptic::Heartbeat,   "needs attention soon (nag, low battery)" },
  { HapticRole::Warning,      "Warning",      Haptic::Warning,     "something is wrong, act now" },
  { HapticRole::Alarm,        "Alarm",        Haptic::Alarm,       "timer done, alarm" },
  { HapticRole::Connected,    "Connected",    Haptic::Success,     "link up: BLE, Wi-Fi, PC bridge" },
  { HapticRole::Disconnected, "Disconnected", Haptic::ToggleOff,   "link lost" },
  { HapticRole::Boot,         "Boot",         Haptic::Success,     "power on" },
  { HapticRole::Wake,         "Wake",         Haptic::Click,       "wake from sleep" },
  { HapticRole::Sleep,        "Sleep",        Haptic::Bump,        "going to sleep" },
  { HapticRole::PowerOff,     "PowerOff",     Haptic::Fade,        "power off" },
  { HapticRole::Shoot,        "Shoot",        Haptic::Snap,        "fire, throw" },
  { HapticRole::HitLight,     "HitLight",     Haptic::Click,       "small impact, scrape, enemy hit" },
  { HapticRole::HitHeavy,     "HitHeavy",     Haptic::Thud,        "big impact, kill, boss hit" },
  { HapticRole::Damage,       "Damage",       Haptic::Error,       "the player takes damage" },
  { HapticRole::Explosion,    "Explosion",    Haptic::Rumble,      "blast, death, crash" },
  { HapticRole::Collect,      "Collect",      Haptic::Tick,        "coin, gem, crumb" },
  { HapticRole::PowerUp,      "PowerUp",      Haptic::Swell,       "power-up, charge done, purchase" },
  { HapticRole::LevelUp,      "LevelUp",      Haptic::Gallop,      "level clear, evolve, new best" },
  { HapticRole::GameOver,     "GameOver",     Haptic::Fade,        "lose, game over" },
  { HapticRole::Speed,        "Speed",        Haptic::Tick,        "one engine/speed pulse; call repeatedly" },
  { HapticRole::Happy,        "Happy",        Haptic::Heartbeat,   "affection, play" },
  { HapticRole::Eat,          "Eat",          Haptic::Purr,        "feeding" },
  { HapticRole::Sad,          "Sad",          Haptic::Fade,        "no, refuse, unhappy" },
};
// C++11 constexpr: recursion, not a loop.
constexpr bool rowsInOrder(size_t i = 0) {
  return i >= sizeof(kRoles) / sizeof(kRoles[0]) ||
         ((size_t)kRoles[i].role == i && rowsInOrder(i + 1));
}
static_assert(sizeof(kRoles) / sizeof(kRoles[0]) == (size_t)HapticRole::Count,
              "every HapticRole needs a row in kRoles");
static_assert(rowsInOrder(), "kRoles rows must be in HapticRole order");

const RoleRow &row(HapticRole r) {
  uint8_t i = (uint8_t)r < (uint8_t)HapticRole::Count ? (uint8_t)r : 0;
  return kRoles[i];
}
}  // namespace

void        haptic(HapticRole r)          { hapticPlay(row(r).fx); }
Haptic      hapticRoleEffect(HapticRole r) { return row(r).fx; }
const char *hapticRoleName(HapticRole r)  { return row(r).name; }
const char *hapticRoleUse(HapticRole r)   { return row(r).use; }
