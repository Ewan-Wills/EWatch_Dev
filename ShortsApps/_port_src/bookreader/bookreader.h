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
#include "view.h"

// Starts the background serial upload listener task (idempotent). Called from
// LibraryView::onEnter so the listener is live whenever the app is in use.
void bookUploadStart();

class LibraryView : public View {
public:
  void onEnter() override;
  void onExit()  override {}
  void render()  override;
  void onEvent(const Event &e) override;
private:
  void rescan();
  void drawList();
  int   scrollTop   = 0;      // index of first visible book row
  int   sel         = -1;     // selected row (for delete confirm)
  bool  confirmDel  = false;  // delete-confirmation overlay active
  bool  firstDraw   = true;
  uint32_t lastSeenUploads = 0;
  uint32_t lastInteract = 0;
};

class ReaderView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;
private:
  void drawFrame();
  void drawControls();
  void saveProgress();
  bool  controls    = false;  // controls overlay visible
  bool  playing     = false;  // auto-scroll active
  float scrollPx    = 0.f;    // current rendered scroll position (px)
  float scrollGoal  = 0.f;    // target scroll position (px)
  uint32_t lastFrameMs = 0;   // for auto-scroll dt
  uint32_t lastInteract = 0;  // idle → return to library
  uint32_t lastSaveMs = 0;    // throttle progress persistence
};
