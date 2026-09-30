#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "pet.h"

// ===========================================================================
// Tunables — all the "game feel" lives here. Rates are per real hour so the
// pet needs attention a few times a day, not constantly. Bump these down to
// make the creature needier, or up to make it more independent.
// ===========================================================================
namespace {

// --- drift rates while AWAKE (points per hour) ---
constexpr float HUNGER_RISE_AWAKE = 8.0f;   // full -> starving in ~12 h
constexpr float HAPPY_FALL_AWAKE  = 5.0f;   // content -> sad in ~14 h
constexpr float ENERGY_DRAIN_AWAKE= 6.0f;   // rested -> tired across a long day
constexpr float MESS_RISE_AWAKE   = 4.0f;   // grubby in ~a day if never cleaned

// --- drift rates while ASLEEP (night-time or a nap) ---
constexpr float HUNGER_RISE_SLEEP = 3.0f;   // metabolism slows in sleep
constexpr float HAPPY_FALL_SLEEP  = 1.0f;   // sleeping is peaceful
constexpr float ENERGY_REFILL_SLEEP=14.0f;  // a full night restores energy
constexpr float MESS_RISE_SLEEP   = 1.0f;

// Extra happiness bleed when a need is neglected.
constexpr float HUNGRY_SADNESS    = 3.0f;   // /h while hunger > 70
constexpr float MESSY_SADNESS     = 3.0f;   // /h while mess   > 60

// Health is a slow composite that chases a target set by the other stats.
constexpr float HEALTH_RATE       = 4.0f;   // points/hour toward the target

// Night window (local hour). The pet naturally sleeps in this range.
constexpr int   NIGHT_START = 22;           // 22:00
constexpr int   NIGHT_END   = 7;            // 07:00

// Action effects.
constexpr float FEED_HUNGER   = 35.0f;      // hunger removed per feed
constexpr float FEED_HAPPY    = 6.0f;
constexpr float FEED_MESS     = 4.0f;       // eating is a little messy
constexpr float FEED_REFUSE_BELOW = 8.0f;   // too full to eat below this hunger

constexpr float PLAY_HAPPY    = 22.0f;
constexpr float PLAY_ENERGY   = 12.0f;      // play is tiring
constexpr float PLAY_HUNGER   = 6.0f;       // and builds an appetite
constexpr float PLAY_NEEDS_ENERGY = 12.0f;  // too tired to play below this

constexpr float CLEAN_HEALTH  = 3.0f;
constexpr float CLEAN_HAPPY   = 6.0f;

constexpr float SHAKE_HAPPY   = 3.0f;       // each shake is a mini-play
constexpr float SHAKE_ENERGY  = 1.0f;
constexpr float PET_HAPPY     = 4.0f;       // a tap-cuddle

// Daily activity treat: this many shakes in a day earns a one-time bonus.
constexpr uint16_t ACTIVITY_TREAT = 25;
constexpr float TREAT_HAPPY   = 15.0f;
constexpr float TREAT_HEALTH  = 5.0f;

// Evolution: age thresholds (days) gated by health so only *good* care grows
// the pet. Lower these (or roll the RTC date forward in Settings) to test.
constexpr float STAGE_CHILD_DAYS = 2.0f;
constexpr float STAGE_ADULT_DAYS = 5.0f;
constexpr float EVOLVE_MIN_HEALTH= 45.0f;   // must be this healthy to evolve
constexpr float EGG_HATCH_DAYS   = 0.25f;   // an untended egg still hatches (~6 h)

// "It needs you" nag cadence while a need is critical.
constexpr uint32_t NAG_INTERVAL_MS = 45000;

// Don't replay more than this much drift after a long power-off — keeps the
// float math sane. Stats clamp at 0/100 anyway, so the pet still ends up
// suitably ravenous after a long absence.
constexpr float MAX_DRIFT_HOURS = 30.0f * 24.0f;

} // namespace

// ===========================================================================
// State
// ===========================================================================
struct PetState {
  float hunger, happiness, energy, health, mess;
  uint32_t bornEpoch;     // 0 until anchored to a valid RTC reading
  uint32_t lastEpoch;     // the stats above are true as-of this epoch (0 = unanchored)
  uint8_t  stage;         // PetStage
  bool     manualNap;     // user-initiated sleep (in addition to night-time)
  uint32_t activityDay;   // epoch/86400 the activity counter belongs to
  uint16_t activityToday;
  bool     treatToday;
};

static PetState g{};
static Preferences petPrefs;
static const char *PET_NS = "pet";

// One-shot / transient flags (RAM only — per session is fine).
static bool     gEvolved   = false;
static bool     gTreat     = false;
static uint32_t gLastNagMs = 0;
static bool     gFirstNag  = true;

// Cached from the most recent petTick() so petGet() can report time-of-day
// state (night sleep, age) without re-reading the RTC itself.
static bool     gCachedAsleep = false;
static uint32_t gCachedEpoch  = 0;

// ---- helpers ----
static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static uint8_t u8(float v) {
  v = clampf(v, 0.0f, 100.0f);
  return (uint8_t)(v + 0.5f);
}
static bool isNight(uint8_t hour) {
  // Night wraps past midnight, e.g. 22:00..07:00.
  if (NIGHT_START <= NIGHT_END) return hour >= NIGHT_START && hour < NIGHT_END;
  return hour >= NIGHT_START || hour < NIGHT_END;
}
static bool asleepNow(uint8_t hour) { return g.manualNap || isNight(hour); }

// ===========================================================================
// Persistence
// ===========================================================================
void petSave() {
  petPrefs.putUChar ("hun",  u8(g.hunger));
  petPrefs.putUChar ("hap",  u8(g.happiness));
  petPrefs.putUChar ("ene",  u8(g.energy));
  petPrefs.putUChar ("hea",  u8(g.health));
  petPrefs.putUChar ("mes",  u8(g.mess));
  petPrefs.putUInt  ("born", g.bornEpoch);
  petPrefs.putUInt  ("last", g.lastEpoch);
  petPrefs.putUChar ("stg",  g.stage);
  petPrefs.putBool  ("nap",  g.manualNap);
  petPrefs.putUInt  ("aday", g.activityDay);
  petPrefs.putUShort("acnt", g.activityToday);
  petPrefs.putBool  ("trt",  g.treatToday);
  petPrefs.putBool  ("init", true);
}

void petInit(uint32_t nowEpoch, bool rtcOk) {
  petPrefs.begin(PET_NS, /*readOnly=*/false);

  if (petPrefs.getBool("init", false)) {
    g.hunger        = petPrefs.getUChar ("hun",  20);
    g.happiness     = petPrefs.getUChar ("hap",  70);
    g.energy        = petPrefs.getUChar ("ene",  80);
    g.health        = petPrefs.getUChar ("hea",  90);
    g.mess          = petPrefs.getUChar ("mes",  0);
    g.bornEpoch     = petPrefs.getUInt  ("born", 0);
    g.lastEpoch     = petPrefs.getUInt  ("last", 0);
    g.stage         = petPrefs.getUChar ("stg",  (uint8_t)PetStage::Egg);
    g.manualNap     = petPrefs.getBool  ("nap",  false);
    g.activityDay   = petPrefs.getUInt  ("aday", 0);
    g.activityToday = petPrefs.getUShort("acnt", 0);
    g.treatToday    = petPrefs.getBool  ("trt",  false);
    return;
  }

  // Fresh pet: a healthy, content egg. Birth/clock anchoring happens on the
  // first valid tick (here if the RTC is already up, otherwise lazily).
  g.hunger = 20; g.happiness = 70; g.energy = 80; g.health = 90; g.mess = 0;
  g.stage = (uint8_t)PetStage::Egg;
  g.manualNap = false;
  g.activityToday = 0; g.treatToday = false;
  if (rtcOk) {
    g.bornEpoch = g.lastEpoch = nowEpoch;
    g.activityDay = nowEpoch / 86400u;
  } else {
    g.bornEpoch = g.lastEpoch = 0;
    g.activityDay = 0;
  }
  petSave();
}

// ===========================================================================
// Drift + evolution
// ===========================================================================
static void rollDayIfNeeded(uint32_t nowEpoch) {
  uint32_t day = nowEpoch / 86400u;
  if (day != g.activityDay) {
    g.activityDay   = day;
    g.activityToday = 0;
    g.treatToday    = false;
  }
}

// Health chases a target degraded by the other neglected needs.
static void driftHealth(float dtHours) {
  float target = 100.0f;
  if (g.hunger    > 60.0f) target -= (g.hunger - 60.0f) * 0.8f;
  if (g.happiness < 40.0f) target -= (40.0f - g.happiness) * 0.8f;
  if (g.mess      > 60.0f) target -= (g.mess - 60.0f) * 0.5f;
  target = clampf(target, 0.0f, 100.0f);
  float maxStep = HEALTH_RATE * dtHours;
  float diff = target - g.health;
  if (diff >  maxStep) diff =  maxStep;
  if (diff < -maxStep) diff = -maxStep;
  g.health = clampf(g.health + diff, 0.0f, 100.0f);
}

static void maybeEvolve(uint32_t nowEpoch) {
  if (g.bornEpoch == 0) return;
  float ageDays = (float)(nowEpoch - g.bornEpoch) / 86400.0f;
  uint8_t want = g.stage;
  if (g.stage == (uint8_t)PetStage::Egg && ageDays >= EGG_HATCH_DAYS)
    want = (uint8_t)PetStage::Baby;          // an ignored egg hatches eventually
  if (g.stage <= (uint8_t)PetStage::Baby && ageDays >= STAGE_CHILD_DAYS &&
      g.health >= EVOLVE_MIN_HEALTH)
    want = (uint8_t)PetStage::Child;
  if (g.stage <= (uint8_t)PetStage::Child && ageDays >= STAGE_ADULT_DAYS &&
      g.health >= EVOLVE_MIN_HEALTH)
    want = (uint8_t)PetStage::Adult;
  if (want > g.stage) {
    g.stage = want;
    gEvolved = true;
    petSave();
  }
}

void petTick(uint32_t nowEpoch, uint8_t hour, bool rtcOk) {
  if (!rtcOk) return;

  gCachedEpoch  = nowEpoch;
  gCachedAsleep = asleepNow(hour);

  // First valid clock for an unanchored pet: anchor without drifting.
  if (g.lastEpoch == 0) {
    g.lastEpoch = nowEpoch;
    if (g.bornEpoch == 0) g.bornEpoch = nowEpoch;
    g.activityDay = nowEpoch / 86400u;
    petSave();
    return;
  }
  // Clock went backwards (RTC re-set earlier): just re-anchor, no drift.
  if (nowEpoch <= g.lastEpoch) { g.lastEpoch = nowEpoch; return; }

  rollDayIfNeeded(nowEpoch);

  float dtHours = (float)(nowEpoch - g.lastEpoch) / 3600.0f;
  if (dtHours > MAX_DRIFT_HOURS) dtHours = MAX_DRIFT_HOURS;
  g.lastEpoch = nowEpoch;

  bool asleep = asleepNow(hour);
  if (asleep) {
    g.hunger    += HUNGER_RISE_SLEEP  * dtHours;
    g.happiness -= HAPPY_FALL_SLEEP   * dtHours;
    g.energy    += ENERGY_REFILL_SLEEP* dtHours;
    g.mess      += MESS_RISE_SLEEP    * dtHours;
  } else {
    g.hunger    += HUNGER_RISE_AWAKE  * dtHours;
    g.happiness -= HAPPY_FALL_AWAKE   * dtHours;
    g.energy    -= ENERGY_DRAIN_AWAKE * dtHours;
    g.mess      += MESS_RISE_AWAKE    * dtHours;
  }
  if (g.hunger > 70.0f) g.happiness -= HUNGRY_SADNESS * dtHours;
  if (g.mess   > 60.0f) g.happiness -= MESSY_SADNESS  * dtHours;

  g.hunger    = clampf(g.hunger,    0.0f, 100.0f);
  g.happiness = clampf(g.happiness, 0.0f, 100.0f);
  g.energy    = clampf(g.energy,    0.0f, 100.0f);
  g.mess      = clampf(g.mess,      0.0f, 100.0f);

  driftHealth(dtHours);
  maybeEvolve(nowEpoch);
}

// ===========================================================================
// Snapshot + mood
// ===========================================================================
static PetMood computeMood(bool asleep) {
  if (asleep)               return PetMood::Sleeping;
  if (g.health    < 25.0f)  return PetMood::Sick;
  if (g.happiness < 30.0f || g.hunger > 80.0f) return PetMood::Sad;
  if (g.happiness > 70.0f && g.hunger < 40.0f && g.energy > 40.0f)
                            return PetMood::Happy;
  return PetMood::Neutral;
}

static bool isCritical() {
  return g.hunger > 78.0f || g.happiness < 22.0f || g.energy < 12.0f ||
         g.health < 25.0f  || g.mess > 80.0f;
}

PetSnapshot petGet() {
  // night/age come from the cache petTick() refreshes each frame.
  PetSnapshot s;
  s.hunger        = u8(g.hunger);
  s.happiness     = u8(g.happiness);
  s.energy        = u8(g.energy);
  s.health        = u8(g.health);
  s.mess          = u8(g.mess);
  s.stage         = (PetStage)g.stage;
  s.asleep        = gCachedAsleep;
  s.mood          = computeMood(s.asleep);
  s.ageDays       = g.bornEpoch ? (uint16_t)((gCachedEpoch - g.bornEpoch) / 86400u) : 0;
  s.activityToday = g.activityToday;
  s.treatToday    = g.treatToday;
  s.critical      = isCritical();
  return s;
}

// ===========================================================================
// Interactions
// ===========================================================================
static void hatchEgg() {
  if (g.stage == (uint8_t)PetStage::Egg) {
    g.stage  = (uint8_t)PetStage::Baby;
    gEvolved = true;
  }
}

PetReact petDo(PetAction a, uint32_t nowEpoch, uint8_t hour) {
  gCachedEpoch  = nowEpoch;
  gCachedAsleep = asleepNow(hour);
  bool asleep   = gCachedAsleep;
  PetReact r    = PetReact::Ok;

  switch (a) {
    case PetAction::Feed:
      if (g.hunger < FEED_REFUSE_BELOW) {            // too full to eat
        g.happiness = clampf(g.happiness - 2.0f, 0, 100);
        r = PetReact::Refused;
        break;
      }
      g.hunger    = clampf(g.hunger    - FEED_HUNGER, 0, 100);
      g.happiness = clampf(g.happiness + FEED_HAPPY,  0, 100);
      g.mess      = clampf(g.mess      + FEED_MESS,   0, 100);
      hatchEgg();
      break;

    case PetAction::Play:
      if (asleep || g.energy < PLAY_NEEDS_ENERGY) {  // asleep or too tired
        r = PetReact::Refused;
        break;
      }
      g.happiness = clampf(g.happiness + PLAY_HAPPY,  0, 100);
      g.energy    = clampf(g.energy    - PLAY_ENERGY, 0, 100);
      g.hunger    = clampf(g.hunger    + PLAY_HUNGER, 0, 100);
      hatchEgg();
      break;

    case PetAction::Clean: {
      bool wasMessy = g.mess > 30.0f;
      g.mess      = 0.0f;
      g.happiness = clampf(g.happiness + (wasMessy ? CLEAN_HAPPY : 2.0f), 0, 100);
      g.health    = clampf(g.health    + CLEAN_HEALTH, 0, 100);
      hatchEgg();
      break;
    }

    case PetAction::Sleep:
      g.manualNap = !g.manualNap;
      r = g.manualNap ? PetReact::Slept : PetReact::Woke;
      gCachedAsleep = asleepNow(hour);
      break;
  }

  g.lastEpoch = nowEpoch;     // stats are valid as-of this interaction
  petSave();
  return r;
}

void petShake(uint32_t nowEpoch) {
  if (gCachedAsleep) return;                 // never disturb a sleeping pet
  rollDayIfNeeded(nowEpoch);
  if (g.activityToday < 60000) g.activityToday++;
  g.happiness = clampf(g.happiness + SHAKE_HAPPY,  0, 100);
  g.energy    = clampf(g.energy    - SHAKE_ENERGY, 0, 100);
  if (!g.treatToday && g.activityToday >= ACTIVITY_TREAT) {
    g.treatToday = true;
    gTreat       = true;
    g.happiness  = clampf(g.happiness + TREAT_HAPPY,  0, 100);
    g.health     = clampf(g.health    + TREAT_HEALTH, 0, 100);
    petSave();                               // persist the rare bonus
  }
}

void petPet(uint32_t nowEpoch) {
  (void)nowEpoch;
  if (gCachedAsleep) return;
  g.happiness = clampf(g.happiness + PET_HAPPY, 0, 100);
}

bool petNagDue() {
  if (!isCritical()) { gFirstNag = true; return false; }
  uint32_t now = millis();
  if (gFirstNag || (now - gLastNagMs) >= NAG_INTERVAL_MS) {
    gFirstNag  = false;
    gLastNagMs = now;
    return true;
  }
  return false;
}

bool petTakeEvolved() { bool e = gEvolved; gEvolved = false; return e; }
bool petTakeTreat()   { bool t = gTreat;   gTreat   = false; return t; }

// ===========================================================================
// Display helpers
// ===========================================================================
const char *petName() {
  // A stable, friendly name picked from its birth time so each pet keeps the
  // same name across reboots. An unanchored egg defaults to the first name.
  static const char *kNames[] = {
    "Pip", "Momo", "Bibo", "Tama", "Zuzu", "Coco", "Nori", "Bean",
  };
  uint32_t seed = g.bornEpoch ? g.bornEpoch : 0;
  return kNames[seed % (sizeof(kNames) / sizeof(kNames[0]))];
}

const char *petStageName(PetStage s) {
  switch (s) {
    case PetStage::Egg:   return "Egg";
    case PetStage::Baby:  return "Baby";
    case PetStage::Child: return "Child";
    case PetStage::Adult: return "Adult";
  }
  return "?";
}

const char *petActionName(PetAction a) {
  switch (a) {
    case PetAction::Feed:  return "Feed";
    case PetAction::Play:  return "Play";
    case PetAction::Clean: return "Clean";
    case PetAction::Sleep: return "Sleep";
  }
  return "?";
}

const char *petStatusLine() {
  if (gCachedAsleep)       return "zzZ...";
  if (g.health    < 25.0f) return "feels poorly";
  if (g.hunger    > 80.0f) return "starving!";
  if (g.happiness < 25.0f) return "lonely";
  if (g.energy    < 15.0f) return "so sleepy";
  if (g.mess      > 70.0f) return "needs a wash";
  if (g.hunger    > 60.0f) return "peckish";
  if (g.happiness > 70.0f && g.hunger < 40.0f) return "happy :)";
  return "doing ok";
}
