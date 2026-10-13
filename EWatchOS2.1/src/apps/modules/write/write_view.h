// Write app: type by drawing letters with a finger. See NOTES.md.
#pragma once
#include "view.h"
#include "write_recog.h"
#include "write_lm.h"

class Arduino_GFX;

class WriteView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  // 60 fps while ink is going down, a button is held or ink fades, 30 while a
  // glyph waits, else 20 (Write holds 240 MHz while open, see the .cpp).
  uint16_t desiredFrameMs() const override;

private:
  static constexpr int kMaxPts = 768;
  static constexpr int kMaxStrokes = 8;
  static constexpr int kClasses = 36;

  enum class Btn : uint8_t { None, Shift, Del };
  enum class Case : uint8_t { Lower, Shift, Caps };

  wrec::RawPt *pts = nullptr;   // kMaxPts, PSRAM (allocated on first enter)
  int      nPts = 0;
  int      nStrokes = 0;
  int      inkShown = 0;         // points already drawn on screen
  bool     inStroke = false;
  uint32_t lastUpMs = 0;
  bool     full = true;          // full redraw due (text/buttons/ink cleared)
  bool     dirtyText = false;    // only the text line and buttons changed

  // CNN result for the glyph as it stands, computed at finger-up so a commit
  // is instant (and an early commit can be decided).
  bool     cacheOk = false;
  float    cacheProb[kClasses];
  bool     wantJob = false;      // glyph changed since the last CNN job
  uint32_t myJob = 0;            // worker job holding this glyph (0 = none)

  // The last glyph's ink fades out instead of vanishing.
  wrec::RawPt *fadePts = nullptr;
  int      nFade = 0;
  int      fadeStep = 0;         // 0 = none; 1..3 = dimmer each step
  uint32_t fadeMs = 0;
  int16_t  fadeBox[4] = {0, 0, 0, 0};

  // A touch that starts on a button is a press unless it moves (then ink).
  Btn      press = Btn::None;
  int16_t  pressX = 0, pressY = 0;
  uint32_t pressMs = 0, repeatMs = 0;
  bool     repeating = false;
  uint32_t lastShiftTapMs = 0;

  // Current word (since the last space): per-letter CNN log-probabilities,
  // for word correction when it ends.
  float    wordLp[wlm::kMaxWord][wlm::kClasses];
  int      wordLen = 0;
  bool     wordOk = true;        // false once the word's letters aren't all ours
  bool     wordChecked = true;   // correction already run on the word as it stands
  uint32_t lastTypedMs = 0;

  // Last correction: highlighted for a moment; one delete right after undoes it.
  int      corrPos = -1, corrLen = 0;
  size_t   corrTextLen = 0;      // text length right after it (undo only if unchanged)
  char     corrOrig[wlm::kMaxWord + 1] = "";
  uint32_t corrHiUntil = 0;

  bool     autoSpace = false;    // the space after . or , was added for the writer
  float    letterSize = 110.f;   // running average of letter ink size (px), for . and ,

  Case     caseMode = Case::Shift;
  bool     loaded = false;
  uint16_t gapMs = 350;

  void beginStroke(int16_t x, int16_t y);
  void addPoint(int16_t x, int16_t y);
  void endStroke();
  void analyse();
  void commit();
  void typeChar(char c, const float *lp);
  void punctuate(char c);
  void space();
  void correctWord();
  void backspace(bool word);
  bool undoCorrection();
  void sentenceCase();
  void startFade();
  void stepFade(uint32_t now);
  void inkIncremental();
  void drawFull();
  void drawTextOn(Arduino_GFX *d);
  void drawButtonsOn(Arduino_GFX *d);
  void drawTextAndButtons();
};
