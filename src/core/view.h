// View interface + registry entry points. Views render from the model and
// consume events the controller posts. Switching is done via switchTo(Screen).
// Concrete views live in src/apps/ (one file per app) and register through
// apps/system/registry.cpp.
#pragma once
#include "model.h"
#include "event.h"

class View {
public:
  virtual ~View() = default;
  virtual void onEnter() {}
  virtual void onExit()  {}
  virtual void render()  = 0;
  virtual void onEvent(const Event &) {}

  // Frame pacing hint for the render task.
  //   0  -> event-driven: re-render only on events / model changes (plus a
  //         1 s heartbeat). This is the battery-friendly default.
  //   >0 -> animated: the render task calls render() roughly every N ms even
  //         with no events, so the view can run a continuous animation.
  // Views may change their answer at runtime (e.g. a watch face returns >0
  // only while a stopwatch is displayed ticking).
  virtual uint16_t desiredFrameMs() const { return 0; }
};

extern View *currentView;
void  viewsInit();
void  switchTo(Screen s);
View *viewFor(Screen s);

// Watch face style picker — defined alongside WatchFaceView in watchface.cpp.
// The web settings page and the on-watch Display settings both use these to
// render and validate the dropdown / row picker.
int          watchFaceStyleCount();
const char  *watchFaceStyleName(int idx);

// Theme helpers — defined in apps/system/ui_kit.cpp, used by every view so a
// Settings → Display change repaints the whole UI on the next render.
struct ThemeColors { uint16_t bg, fg, accent, line; };
ThemeColors  theme();
uint16_t     contrastFor(uint16_t bg);     // WHITE/BLACK best against bg
void         drawBackButton();              // top-left chevron (accent bg)
void         drawTitleBar(const char *title,
                          int16_t titleY = 14, int16_t lineY = 46,
                          int16_t lineX = 20, int16_t lineW = 200);
bool         tappedBack(uint16_t x, uint16_t y);
