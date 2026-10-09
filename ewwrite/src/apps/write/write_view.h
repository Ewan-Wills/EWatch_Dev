// Write app: type by drawing letters with a finger. See ewwrite/NOTES.md.
#pragma once
#include "view.h"
#include "write_recog.h"

class WriteView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  // ~30 fps while a finger is down or a glyph is waiting to be recognised.
  uint16_t desiredFrameMs() const override;

private:
  static constexpr int kMaxPts = 768;
  static constexpr int kMaxStrokes = 8;

  wrec::RawPt *pts = nullptr;   // kMaxPts, PSRAM (allocated on first enter)
  int      nPts = 0;
  int      nStrokes = 0;
  bool     inStroke = false;     // finger down drawing ink
  bool     uiTouch = false;      // finger went down on the top bar
  uint32_t lastUpMs = 0;
  bool     dirty = true;

  // Last recognition (chips + correction + learning).
  wrec::Cand cands[3];
  int      nCands = 0;
  int      candPos = -1;         // index in the text of the char they refer to
  int      candSel = 0;          // which chip is currently applied
  wrec::Pt lastCloud[wrec::kN];
  char     status[40] = "";

  bool     upper = true;
  bool     loaded = false;
  uint16_t gapMs = 600;

  void beginStroke(int16_t x, int16_t y);
  void addPoint(int16_t x, int16_t y);
  void endStroke();
  void recognise();
  void tapTopBar(uint16_t x, uint16_t y);
  void applyCandidate(int i);
  void saveUserTemplates();
  void loadUserTemplates();
};
