// Home Assistant light remote — public API.
//
// Architecture (mirrors the Spotify app, trimmed to one light): a single
// background FreeRTOS task owns ALL network I/O. It polls the light's state
// (GET /api/states/<entity>) and runs toggle commands (POST
// /api/services/light/turn_on|turn_off), talking plain HTTP to the HA REST API
// with a Bearer token. Everything it produces lives behind one mutex; the view
// only reads a snapshot and posts commands — it never touches the network.
//
// Config (base URL + long-lived token + entity id) is compiled in from
// secrets/ha.local.h (copy of secrets/ha.example.h). No secrets are hardcoded
// in this file.
#pragma once
#include <stdint.h>

// ---- lifecycle (called from main.cpp) ----
void haInit();            // after Storage::load(); init mutex/queue + snapshot
void haSvcStartTask();    // spawn the background network task

// ---- connection / light state ----
enum class HAConn : uint8_t {
  NeedWifi,     // WiFi off, not in Client mode, or not connected
  NeedConfig,   // secrets/ha.local.h still holds the placeholder token
  Connecting,   // first poll in flight / retrying
  Online,       // last poll succeeded
  Error         // repeated poll/command failure (HTTP or network)
};

enum class HALight : uint8_t {
  Unknown,      // not polled yet
  On,
  Off,
  Unavailable   // entity reported "unavailable"/"unknown" by HA
};

// Immutable snapshot the view renders from. Copied out under the mutex so the
// view never holds a lock while drawing.
struct HASnapshot {
  HAConn  conn;
  HALight light;
  bool    busy;             // a toggle command is in flight
  int     lastHttpStatus;   // last REST status (for the debug status line)
};

// Fill `out` with the current state. Cheap; takes the mutex briefly.
void haGetSnapshot(HASnapshot *out);

// ---- commands (posted by the view, run by the service) ----
enum class HACmd : uint8_t {
  Toggle,   // flip based on last known state (defaults to turn_on if unknown)
  Refresh   // force an immediate state poll
};
void haPostCmd(HACmd cmd);

// Friendly label for the light (from HA_LIGHT_NAME, or the entity id).
const char *haLightName();

// ---- the launcher view ----
#include "view.h"
class HomeAssistantView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override {}

private:
  void drawButton(const HASnapshot &s, bool full);
  void drawStatus(const HASnapshot &s, bool full);

  bool    firstDraw = true;
  HAConn  shownConn  = (HAConn)0xFF;
  HALight shownLight = (HALight)0xFF;
  bool    shownBusy  = false;
  int     shownStatus = 0x7FFFFFFF;

  // optimistic echo so the dot/label flips the instant you tap
  bool    optimistic      = false;
  HALight optimisticLight = HALight::Unknown;

  // button hit-rect (set in drawButton)
  int16_t btnX = 0, btnY = 0, btnW = 0, btnH = 0;
};
