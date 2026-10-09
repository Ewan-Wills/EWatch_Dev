// AuraListView — the EWatchOS launcher / menu list.
//
// A vertically scrolling list of icon cards with Apple-feel physics: momentum
// flings, rubber-band edges, press-highlight on touch, cascade entrance, and
// slide transitions into whatever a row opens. One class drives the app
// launcher, the System page, and the Settings menu — each is just a table.
#pragma once
#include <Arduino_GFX_Library.h>
#include "view.h"
#include "ui_icons.h"
#include "ui_motion.h"

struct AuraEntry {
  const char *label;
  Icon        icon;
  uint16_t    tileColor;     // squircle fill behind the glyph
  Screen      target;
  const char *section;       // non-null starts a new section header above row
  const char *sub;           // optional second line (size 1); omit = nullptr
};

class AuraListView : public View {
public:
  static const int kMaxRows = 40;      // rowY[] capacity — tables must fit
  AuraListView(const AuraEntry *entries, int n, const char *title,
               Screen back, Trans backTrans = Trans::PopRight)
    : entries(entries), N(n > kMaxRows ? kMaxRows : n), title(title),
      backScreen(back), backTrans(backTrans) {}

  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  bool paintTo(Arduino_Canvas *cv) override;
  uint16_t desiredFrameMs() const override;

protected:
  // Hooks for data-driven lists (e.g. the Books library). Defaults give the
  // plain launcher behaviour.
  virtual void activate(int row);                  // tap: open the row
  virtual void longPress(int row) { (void)row; }   // held ~0.65 s without moving
  virtual void drawOverlay(Arduino_Canvas *cv) { (void)cv; }  // over the list
  virtual bool interceptEvent(const Event &e) { (void)e; return false; }
  void setEntries(const AuraEntry *e, int n);      // swap rows at runtime
  int  rowCount() const { return N; }

private:
  const AuraEntry *entries;
  int              N;
  const char      *title;
  const Screen     backScreen;
  const Trans      backTrans;

  Arduino_Canvas *canvas = nullptr;
  ScrollPhys      phys;
  Tween           entrance;          // rows cascade up on entry
  int             rowY[kMaxRows];    // content-space Y of each row (precomputed)
  int             contentH = 0;

  int      pressed   = -1;           // row index currently under a finger
  uint32_t pressedMs = 0;
  uint32_t lastFingerMs = 0;   // last Touch/TouchHold seen (drag watchdog)
  bool     dragMode  = false;
  bool     longFired = false;        // long-press already handled this touch
  uint16_t downX = 0, downY = 0;
  uint32_t launchAtMs = 0;           // deferred launch (post press-flash)
  int      launchRow  = -1;

  uint16_t *tileCache = nullptr;     // N pre-rendered 44x44 icon tiles (PSRAM)

  void  buildTileCache();
  void  blitTile(int i, int16_t x, int16_t y);
  void  layout();
  void  drawFrame();
  int   hitRow(int16_t x, int16_t y) const;
  void  handleEvent(const Event &e);
  bool  animating() const;
  void  goBack();
};
