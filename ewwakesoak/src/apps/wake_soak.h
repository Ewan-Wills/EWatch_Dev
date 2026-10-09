// Wake soak test — benchmark what really wakes the watch.
//
// While enabled (Wake Soak app, saved in NVS), every wake from screen-off or
// from the screensaver is recorded. Touch and motion wakes ask the wearer
// "was that on purpose?": hold YES for 1 s, tap NO, or let it time out (5 s,
// counted as not intentional). NO and timeout put the screen straight back off.
// Button wakes are logged as intentional without a prompt. Sleep touches the
// hold filter rejected are logged too ("rejected"), so near-misses show up.
//
// Each record is one CSV line in LittleFS /wakesoak.csv and an EWLOGI("SOAK")
// line. Console: `soak stats` | `soak dump` | `soak clear` | `soak on|off`
// | `soak get FROM COUNT` (hashed lines for tools/soak_pull.py).
// CSV: n,uptime_s,rtc,src,off_ms,gesture,pts,x,y,held_ms,ax,ay,az,answer,resp_ms,trace_id
// For motion wakes gesture = raise-check verdict (0 ok 1 not_facing
// 2 not_steady 3 no_turn 4 no_imu) and held_ms = check time.
// LABEL MODE (`soak label on`, default off): every motion trigger wakes and
// asks, whatever the raise check said, so every trigger gets a ground-truth
// label; the verdict is still recorded. Ignoring the prompt = not intended.
// The accelerometer trace for each row is in wake_gest (trace_id).
#pragma once
#include "view.h"

struct SoakWake {
  const char *src;       // "sleep_touch" "sleep_motion" "sleep_button"
                         // "saver_touch" "saver_button" "sleep_touch_rejected"
  uint32_t offMs;        // time dark / in the screensaver before the wake
  uint8_t  gesture, pts; // touch chip register at wake
  uint16_t x, y;
  uint16_t heldMs;       // how long the finger was held (touch wakes)
  int16_t  ax, ay, az;   // raw accel at wake (motion wakes), 0 otherwise
  uint32_t traceId;      // wake_gest trace (0 = none)
};

void soakBegin();                                // boot: load the enabled flag
bool soakEnabled();
bool soakLabelMode();                            // label every motion trigger
void soakOnWake(const SoakWake &w, bool askUser);   // render task
void soakLogRejected(const SoakWake &w);            // render task (screen-off loop)
bool soakHandleLine(char *line);                    // ewlog console
bool soakPromptActive();                            // keep the screen awake
bool soakTakeGoto();                                // render task: `soak test` prompt pending

class WakeSoakView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;
private:
  bool     firstDraw = true;
  uint32_t enterMs = 0;
  bool     holding = false;
  uint32_t holdStartMs = 0;
  int      shownProg = -1, shownSecs = -1;
  void drawPrompt();
  void drawStats();
  void finish(const char *answer);
};
