// Haptic roles: what HAPPENED, not how it feels.
//
// Apps call haptic(HapticRole::Back) instead of picking an effect or a raw
// hapticBuzz(intensity, ms). One table (haptic_roles.cpp) maps every role to
// an effect from the library in drivers/haptic.h, so the whole watch can be
// retuned in one place and the same event always feels the same in every app.
// The guide with when-to-use rules and per-app recommendations is HAPTICS.md
// in the project root. `hap roles` on the console prints the live table.
#pragma once
#include <stdint.h>
#include "haptic.h"

enum class HapticRole : uint8_t {
  // ---- navigation & controls (quiet; the user caused it and is looking) ----
  Tap,           // generic button / tile press
  Open,          // open an app, item, detail view
  Back,          // back / close / dismiss
  Scroll,        // list detent, page flip, carousel step
  Adjust,        // +/- value step, slider notch, held ramp repeat
  ModeChange,    // switch mode / tab / view style
  ToggleOn,      // something switched on / started
  ToggleOff,     // something switched off / stopped
  LongPress,     // a hold was recognised
  Destructive,   // delete, clear all, reset (after confirming)
  // ---- results of an action ----
  Confirm,       // saved / sent / applied
  Refused,       // can't do that (invalid input, not enough coins, nothing to do)
  Failed,        // tried and it failed (not connected, I/O error)
  // ---- the watch telling you something (you might not be looking) ----
  MessageIn,     // a message / notification arrived
  Reminder,      // something needs attention soon (nag, low battery)
  Warning,       // something is wrong and needs action
  Alarm,         // timer done, alarm: must be noticed
  Connected,     // link up (BLE paired, Wi-Fi joined, PC bridge)
  Disconnected,  // link lost
  // ---- system ----
  Boot, Wake, Sleep, PowerOff,
  // ---- games ----
  Shoot,         // fire a weapon / throw
  HitLight,      // small impact, scrape, enemy hit
  HitHeavy,      // big impact, kill, boss hit
  Damage,        // the player takes damage
  Explosion,     // blast, death, crash
  Collect,       // coin, gem, crumb
  PowerUp,       // power-up, charge complete, buy
  LevelUp,       // level clear, evolve, new high score
  GameOver,      // lose / game over
  Speed,         // one short engine / road pulse: call it repeatedly for a texture
  // ---- companion / pet ----
  Happy,         // affection, play
  Eat,           // feeding
  Sad,           // "no", refuse, unhappy
  Count
};

void        haptic(HapticRole r);
Haptic      hapticRoleEffect(HapticRole r);
const char *hapticRoleName(HapticRole r);
const char *hapticRoleUse(HapticRole r);   // one-line "when to use"
