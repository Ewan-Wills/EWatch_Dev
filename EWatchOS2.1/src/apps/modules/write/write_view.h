// Write app: type by drawing letters with a finger. See NOTES.md.
#pragma once
#include "view.h"
#include "write_recog.h"
#include "write_lm.h"

class WriteView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  // 60 fps while ink is going down or a button is held, 30 while a glyph waits, else 20.
  uint16_t desiredFrameMs() const override;

private:
  static constexpr int kMaxPts = 768;
  static constexpr int kMaxStrokes = 8;

  enum class Btn : uint8_t { None, Shift, Del };
  enum class Case : uint8_t { Lower, Shift, Caps };

  wrec::RawPt *pts = nullptr;   // kMaxPts, PSRAM (allocated on first enter)
  int      nPts = 0;
  int      nStrokes = 0;
  int      inkShown = 0;         // points already drawn on screen
  bool     inStroke = false;
  uint32_t lastUpMs = 0;
  bool     full = true;          // full redraw due (text/buttons/ink cleared)

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

  Case     caseMode = Case::Lower;
  bool     loaded = false;
  uint16_t gapMs = 450;

  void beginStroke(int16_t x, int16_t y);
  void addPoint(int16_t x, int16_t y);
  void endStroke();
  void recognise();
  void typeChar(char c, const float *lp);
  void space();
  void correctWord();
  void backspace(bool word);
  void inkIncremental();
  void drawFull();
};
