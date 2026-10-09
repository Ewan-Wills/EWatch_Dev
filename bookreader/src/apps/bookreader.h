// bookreader — read plain-text books stored in LittleFS on the watch.
//
// Two screens:
//   * LibraryView  (Screen::BookLibrary) — scrollable list of stored .txt
//     books; tap to open, long-press to delete. Also starts the background
//     serial-upload listener so a .txt sent from the PC lands in flash.
//   * ReaderView   (Screen::BookReader)  — word-wrapped text with smooth
//     pixel scrolling (frameCanvas), swipe/auto-scroll, font-size setting,
//     and per-book last-position memory (persisted to NVS by byte offset).
//
// The paging engine (book.*-style logic) lives entirely in bookreader.cpp:
// a book is indexed once on open into an array of per-line byte offsets held
// in PSRAM, and each visible line's bytes are read from flash on demand — the
// whole book is never resident in RAM.
#pragma once
#include <math.h>
#include "view.h"
#include "system/launcher.h"

// Starts the background serial upload listener task (idempotent). Called from
// LibraryView::onEnter. The listener only reads serial while the Library is on
// screen (it pauses the ewlog console then); uploads need the Library open.
void bookUploadStart();

// The Library is the launcher list (AuraListView: same cards, physics,
// transitions) fed with the books on flash. Tap opens, long-press deletes.
class LibraryView : public AuraListView {
public:
  LibraryView() : AuraListView(nullptr, 0, "Books", Screen::AppList) {}
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
protected:
  void activate(int row) override;
  void longPress(int row) override;
  void drawOverlay(Arduino_Canvas *cv) override;
  bool interceptEvent(const Event &e) override;
private:
  void rescan();
  int   delRow      = -1;     // row awaiting delete confirmation, -1 = none
  uint32_t lastSeenUploads = 0;
  uint32_t lastInteract = 0;
};

class ReaderView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
  // OS2.1 frame pacing: animate (~30 fps) while scrolling or auto-playing,
  // otherwise event-driven. Replaces BaseOS's constant model.revision bumps.
  uint16_t desiredFrameMs() const override {
    return (playing || pendingTapMs || fabsf(scrollGoal - scrollPx) > 0.3f ||
            fabsf(menuTarget - menuPos) > 0.002f) ? 33 : 0;
  }
private:
  void drawFrame();
  void drawControls();
  void saveProgress();
  uint32_t topLine() const;           // line at the top of the screen (target, not mid-scroll)
  void jumpToLine(int32_t line);      // instant jump, clamped
  void handleTap(int16_t x, int16_t y);
  void openControls();                // pauses auto-scroll
  void closeControls();               // resumes it if it was running
  // Tap detection: a tap is a short touch that didn't move and wasn't
  // followed by a swipe gesture; it's acted on ~160 ms after release so a
  // late-arriving swipe gesture can still cancel it.
  uint16_t downX = 0, downY = 0;
  uint32_t downMs = 0;
  bool     moved = false;
  uint32_t pendingTapMs = 0;
  int16_t  tapX = 0, tapY = 0;
  uint32_t lastGestureMs = 0;
  bool     resumeAfter = false;       // auto-scroll was on when the menu opened
  float    menuPos = 0.f, menuTarget = 0.f;   // menu slide: 0 hidden, 1 open
  uint32_t lastSavedOff = 0xFFFFFFFFu;
  bool  controls    = false;  // controls overlay visible
  bool  playing     = false;  // auto-scroll active
  float scrollPx    = 0.f;    // current rendered scroll position (px)
  float scrollGoal  = 0.f;    // target scroll position (px)
  uint32_t lastFrameMs = 0;   // for auto-scroll dt
  uint32_t lastInteract = 0;  // idle → return to library
  uint32_t lastSaveMs = 0;    // throttle progress persistence
};
