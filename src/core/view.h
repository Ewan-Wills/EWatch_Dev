// View interface. Views render from the model and consume events the
// controller posts. The render task drives `currentView`; switching is done by
// calling switchTo(Screen). The racer runs a single RacerView (see
// src/apps/game/racer_view.cpp).
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
};

extern View *currentView;
void  viewsInit();        // construct views, enter the initial screen
void  switchTo(Screen s); // swap the active view (onExit old / onEnter new)
View *viewFor(Screen s);
