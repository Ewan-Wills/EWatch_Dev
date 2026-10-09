// printstream — Creality K2 Plus live camera + basic print control.
//
// Architecture (mirrors Spotify_Controller / wifi_svc): background FreeRTOS
// tasks own ALL network I/O; the view only reads immutable snapshots and posts
// commands, never touching the network itself.
//
//   psNetTask  — polls Moonraker (/printer/objects/query) for print state,
//                temps and progress; executes confirmed control commands
//                (pause/resume/home). Plain HTTP on :7125, no auth.
//   psCamTask  — when the camera screen is active, pulls JPEG frames from the
//                printer's MJPEG proxy (:4408/webcam/?action=stream) — or, as a
//                fallback, periodic ?action=snapshot — into a PSRAM buffer and
//                bumps a generation counter. Decoding (JPEGDEC) happens on the
//                render task, exactly like Spotify album art.
//
// All printer connection details come from secrets/printer.local.h (git-
// ignored) or secrets/printer.example.h — never hardcoded here. Control
// actions (pause/resume/home) hit real hardware and are gated behind an
// on-screen CONFIRM step in the view before a command is ever posted.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "view.h"

// ---- lifecycle (called from main.cpp) ----
void printstreamInit();        // after Storage::load(); create mutexes/queues
void printstreamStartTasks();  // spawn the background net + camera tasks

// ---- link / print state --------------------------------------------------
enum class PsLink : uint8_t {
  NeedWifi,     // WiFi off / not in client mode / not connected
  Connecting,   // have WiFi, not yet had a good Moonraker reply
  Online,       // Moonraker answering
  Offline       // Moonraker was reachable but is now timing out / erroring
};

enum class PsState : uint8_t {
  Unknown, Standby, Printing, Paused, Complete, Cancelled, Error
};

enum class PsCamState : uint8_t {
  Off,          // camera screen not active -> task idle
  Connecting,   // opening the stream
  Streaming,    // live MJPEG frames arriving
  SnapshotOk,   // MJPEG unavailable; periodic snapshots are working
  Unavailable   // no frames at all (daemon down / idle printer / wrong URL)
};

// Immutable snapshot the view renders from. Copied out under the net mutex so
// the view never holds a lock while drawing.
struct PsSnapshot {
  PsLink   link;
  PsState  state;
  PsCamState cam;

  char     filename[128];   // current/last job gcode name (basename)
  uint8_t  progressPct;     // 0..100

  float    nozzleTemp, nozzleTarget;
  float    bedTemp,    bedTarget;

  uint32_t printDurationS;  // seconds elapsed on the active print
  int      lastHttpStatus;  // for the status line / debugging
};

// Fill `out` with the current state. Cheap; takes the net mutex briefly.
void printstreamGetSnapshot(PsSnapshot *out);

// ---- camera frame buffer (produced by psCamTask, decoded by the view) ----
// psCamTask writes the newest complete JPEG into a PSRAM buffer and bumps a
// generation counter. The view compares psCamGen() against what it last drew;
// when it differs it locks the buffer, decodes to the panel, records the gen.
// Locking blocks the camera task from overwriting the buffer mid-decode.
uint32_t       psCamGen();
const uint8_t *psCamLock(size_t *lenOut);   // mutex; nullptr if no frame yet
void           psCamUnlock();

// The view calls this on enter/exit so the camera task only streams while the
// camera screen is actually visible (saves radio + CPU otherwise).
void           psSetCameraWanted(bool wanted);

// ---- control commands (posted by the view AFTER confirm, run by psNetTask) -
enum class PsCmd : uint8_t { Pause, Resume, Home, Resync };
void printstreamPostCmd(PsCmd cmd);

// ---- the launcher view ----------------------------------------------------
// Two sub-screens inside the one app: a camera view and a stats/controls view;
// swipe up/down toggles between them. Controls live on the stats screen, each
// behind a tap-to-arm CONFIRM overlay.
class PrintStreamView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override;

private:
  enum class Page : uint8_t { Camera, Stats };
  enum class Confirm : uint8_t { None, Pause, Resume, Home };

  void drawCamera(const PsSnapshot &s, bool full);
  void drawStats(const PsSnapshot &s, bool full);
  void drawConfirmOverlay();
  void clearConfirm();
  bool inRect(int x, int y, int rx, int ry, int rw, int rh) const {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
  }

  Page     page       = Page::Stats;
  bool     firstDraw   = true;
  Confirm  confirm     = Confirm::None;

  // camera delta cache
  uint32_t lastCamGen  = 0xFFFFFFFF;
  PsCamState lastCamState = (PsCamState)0xFF;
  int      shownState2_ = -1;   // status-strip cache (camera page)
  int      shownPct2_   = -1;

  // stats delta cache (avoid per-frame flicker)
  PsLink   shownLink   = (PsLink)0xFF;
  PsState  shownState  = (PsState)0xFF;
  int      shownPct    = -1;
  int      shownNozzle = -1000, shownBed = -1000;
  char     shownFile[128] = "";
};
