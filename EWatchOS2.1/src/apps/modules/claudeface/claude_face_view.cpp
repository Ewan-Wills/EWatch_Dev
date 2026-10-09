#include "claude_face_view.h"
#include "claude_face.h"
#include "cf_board.h"
#include "cf_link.h"
#include "cf_orient.h"
#include "cf_policy.h"
#include "cf_settings.h"
#include "model.h"

static bool sBegun = false;

void claudeFaceModuleBegin() {
  settingsLoad();
  cfLinkInit();
}

void ClaudeFaceView::onEnter() {
  if (!sBegun) { claudeFaceBegin(frameCanvas()); sBegun = true; }
  claudeFaceWake();
}

void ClaudeFaceView::render() {
  cfLinkService();                 // HELLO on attach + queued EVENTs
  CFEnv env;
  { ModelLock lk;
    env.rtcOk = model.rtcOk;
    env.year = model.year; env.month = model.month; env.day = model.day;
    env.hour = model.hour; env.minute = model.minute; env.second = model.second;
    env.batOk = model.batPct > 0; env.batPct = model.batPct; }
  claudeFaceTick(env);
}

void ClaudeFaceView::onEvent(const Event &e) {
  switch (e.type) {
    case EventType::Touch:      claudeFaceTouchDown(e.x, e.y); break;
    case EventType::TouchHold:  claudeFaceTouchHold(e.x, e.y); break;
    case EventType::TouchUp:    claudeFaceTouchUp(e.x, e.y); break;
    case EventType::ButtonDown: claudeFaceWake(); break;
    case EventType::ButtonShort: claudeFaceButtonShort(); break;   // detail page on/off
    case EventType::ImuMotion:  claudeFaceWake(); break;
    case EventType::Gesture: {
      uint8_t g = (uint8_t)e.gesture;      // Gesture ids are the CST816S chip ids
      if (cfPanelDirToView(cfChipGestureToPanelDir(g)) == CF_DIR_DOWN) {
        switchTo(Screen::AppList);          // the face has no use for swipe down
        return;
      }
      claudeFaceGesture(g);
      break;
    }
    default: break;
  }
}
