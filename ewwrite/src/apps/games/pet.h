// Virtual-pet state model — the "soul" of the Tamagotchi app.
//
// This module owns the creature's persistent state and all of its rules. It is
// deliberately free of any drawing or hardware code so it can be reasoned about
// (and unit-tested) on its own; the view (petview.cpp) reads a PetSnapshot and
// paints it, and feeds user/sensor input back in through the action calls.
//
// TIME BASE & PERSISTENCE
//   Every stat drifts over *real* time, anchored to the RV-3028 RTC epoch
//   (seconds since 2000, via rtcEpochSec()). State is persisted to NVS together
//   with `lastEpoch` — the timestamp at which the saved stat values were true.
//   On every wake/reboot/power-off we reload (stats, lastEpoch) and replay the
//   elapsed drift up to "now", so the pet keeps living while the watch is off.
//   That makes saves cheap and rare: we only persist on a state change (an
//   action, an evolution) and on app exit, never per-frame.
#pragma once
#include <stdint.h>

// Persistent growth stages. The egg hatches on first care; later stages are
// reached after N days of *good* care (see pet.cpp evolution rules).
enum class PetStage : uint8_t { Egg = 0, Baby = 1, Child = 2, Adult = 3 };

// The resting expression the view should draw. Transient animations (eating,
// playing, refusing) are owned by the view; this is the underlying mood.
enum class PetMood : uint8_t { Sleeping, Sick, Sad, Neutral, Happy };

// The four tap actions, in the order they appear on the action bar.
enum class PetAction : uint8_t { Feed = 0, Play = 1, Clean = 2, Sleep = 3 };

// How the pet reacted to an action — drives the view's haptic + status text.
enum class PetReact : uint8_t {
  Ok,       // the action did something good
  Refused,  // pet declined (not hungry / too tired / asleep)
  Slept,    // manual nap started
  Woke,     // manual nap ended
};

// Immutable view of the pet for one frame. All bars are 0..100.
struct PetSnapshot {
  uint8_t  hunger;        // 0 = full belly .. 100 = starving
  uint8_t  happiness;     // 0 = miserable .. 100 = delighted
  uint8_t  energy;        // 0 = exhausted .. 100 = rested
  uint8_t  health;        // 0 = dying .. 100 = thriving
  uint8_t  mess;          // 0 = spotless .. 100 = filthy
  PetStage stage;
  PetMood  mood;
  bool     asleep;        // sleeping right now (night-time or a manual nap)
  uint16_t ageDays;       // whole days since birth
  uint16_t activityToday; // shakes/steps counted today
  bool     treatToday;    // daily activity treat already earned
  bool     critical;      // at least one need is in the red
};

// Load persisted state (or create a fresh egg). Safe to call once at app entry;
// `nowEpoch`/`rtcOk` anchor a brand-new pet's birth time. If the RTC isn't
// ready yet, anchoring is deferred to the first petTick() that has a valid clock.
void petInit(uint32_t nowEpoch, bool rtcOk);

// Advance drift up to `nowEpoch` and re-evaluate mood + evolution. Call every
// frame with the current RTC epoch and hour-of-day. A no-op when !rtcOk.
void petTick(uint32_t nowEpoch, uint8_t hour, bool rtcOk);

// Current snapshot for drawing.
PetSnapshot petGet();

// Perform a tap action. Returns how the pet reacted. `nowEpoch`/`hour` keep the
// drift anchor consistent with the moment of interaction. Persists on success.
PetReact petDo(PetAction a, uint32_t nowEpoch, uint8_t hour);

// A vigorous shake was detected (ImuMotion) while the app is foreground: counts
// as activity and a little burst of play. May earn the once-a-day treat.
void petShake(uint32_t nowEpoch);

// The creature itself was tapped (affection). A small happiness nudge.
void petPet(uint32_t nowEpoch);

// True at most once per NAG interval while a need is critical — the view buzzes
// the "I need attention" pattern when this fires. Self-rate-limited via millis().
bool petNagDue();

// One-shot flags the view consumes to play a celebration animation/haptic.
bool petTakeEvolved();   // true once, right after a stage advance
bool petTakeTreat();     // true once, right after the daily treat is earned

// Flush current state to NVS. Called on app exit; also called internally after
// each action so an auto-sleep that skips onExit() can't lose a fed meal.
void petSave();

// Display helpers.
const char *petName();          // stable per-pet name, seeded from its birth time
const char *petStageName(PetStage s);
const char *petActionName(PetAction a);
const char *petStatusLine();   // short flavour text for the current state
