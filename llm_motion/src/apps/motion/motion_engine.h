// Motion engine — runs the motion language model on its own FreeRTOS task.
//
//   I/O task (50 Hz) --motionFeedSample--> queue --> engine task (core 0, prio 2)
//   MotionView touch gestures --motionFeedTouch--^      |  tokenizer -> segmenter
//                                                        |  -> LM (incremental)
//   render task <--motionServiceRender-- pending action -+  -> label + p
//
// The engine runs while the Motion app is open, or everywhere when "global"
// (background) mode is on. Actions are executed on the render task (where
// switchTo/powerOnActivity are legal). Dry run (default ON) only logs/shows.
#pragma once
#include <Arduino.h>
#include "event.h"

struct MotionSnap {
  bool     ready, running, dry, global;
  int      capLabel;            // -1 = capture off
  uint32_t version;             // bumps on any visible change
  uint8_t  toks[8];             // last tokens, oldest first
  uint8_t  ntoks;
  int16_t  predNext;            // LM's most likely next motion token (-1 none)
  bool     segActive;
  uint8_t  segLen;
  int      lastLabel;           // -1 = nothing recognised yet
  int      lastBest;            // best label before the threshold
  float    lastP;
  bool     lastTouch;
  uint32_t lastAtMs;
  char     lastAction[28];
  float    lastFwdMs;           // last single forward pass
  float    avgFwdMs;            // running mean
  float    lastClassifyMs;      // SEP forward + label softmax at segment end
  float    tokPerSec;           // forward passes per second (recent)
  uint32_t forwards, segments, recognised;
  uint16_t injectLeft;          // samples of an injected trace still to play
  float    thr;
};

void        motionEngineStart();          // MotionView::onEnter (lazy init)
void        motionEngineStop();           // MotionView::onExit (no-op if global)
bool        motionEngineReady();
void        motionGetSnap(MotionSnap *out);
void        motionSetDry(bool dry);
void        motionSetGlobal(bool on);
void        motionSetCapture(int label);  // -1 off
void        motionFeedTouch(Gesture g);   // MotionView only

// controller.cpp hooks
void        motionFeedSample(bool ok, int16_t ax, int16_t ay, int16_t az);   // I/O task
void        motionServiceRender();        // render task, once per loop

// ewlog console: "motion ..." lines. Returns true if handled.
bool        motionConsole(const char *line);

// Action text for a label (for the mapping page).
const char *motionActionText(int label);
