// Spotify now-playing viewer + remote — public API.
//
// Architecture (mirrors wifi_svc): a single background FreeRTOS task owns ALL
// network I/O. It runs the auth state machine (PKCE), refreshes access tokens,
// polls GET /v1/me/player, downloads album-art JPEG bytes, and executes
// transport commands. Everything it produces lives behind one mutex; the view
// only reads snapshots and posts commands — it never touches the network.
//
// The watch is a REMOTE, not a playback device: commands act on whatever device
// is active on the user's Spotify Connect. Controlling playback needs Spotify
// Premium; reading now-playing works on Free (controls degrade gracefully).
//
// Secrets (Client ID + refresh token) persist in their own NVS namespace
// ("spotify"), owned exclusively by the service task. The global `model` is
// untouched except for Screen::Spotify.
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---- lifecycle (called from main.cpp) ----
void spotifyInit();          // after Storage::load(); loads Client ID + refresh token
void spotifySvcStartTask();  // spawn the background network task

// ---- where the auth flow stands / what the player is doing ----
enum class SpotifyAuth : uint8_t {
  NeedWifi,      // WiFi off, not in Client mode, or not connected to a network
  NeedClientId,  // no Spotify Client ID configured yet (set via web config)
  NeedAuth,      // have Client ID but no refresh token -> show the QR sign-in
  Authorizing,   // exchanging the pasted code / refreshing the very first token
  Ready,         // authenticated; polling now-playing
  AuthError      // token exchange/refresh failed (e.g. revoked) -> re-auth
};

enum class SpotifyPlayback : uint8_t {
  Unknown,   // not polled yet
  Playing,
  Paused,
  Idle,      // 204 / no active device
  Offline    // network/API unreachable
};

// Immutable snapshot the view renders from. Copied out under the service mutex
// so the view never holds a lock while drawing.
struct SpotifySnapshot {
  SpotifyAuth     auth;
  SpotifyPlayback playback;

  char     track[128];
  char     artist[160];
  char     album[128];
  char     trackId[48];     // Spotify track id — used to detect track changes
  char     artTrackId[48];  // track id the downloaded album art belongs to

  uint32_t progressMs;      // playback position at progressAnchorMs
  uint32_t durationMs;
  uint32_t progressAnchorMs;// millis() when progress/isPlaying were sampled
  bool     isPlaying;

  bool     hasVolume;
  uint8_t  volumePct;       // active device volume (0..100)

  bool     premium;         // false once a control returns PREMIUM_REQUIRED
  bool     volumeSupported; // false once the active device refuses volume (403/404)
  bool     rateLimited;     // last poll/command hit 429
  int      lastHttpStatus;  // for the on-screen/debug status line
};

// Fill `out` with the current state. Cheap; takes the mutex briefly.
void spotifyGetSnapshot(SpotifySnapshot *out);

// ---- album art (produced by the service, drawn by the render task) ----
// The service downloads the JPEG bytes into a PSRAM buffer and bumps a
// generation counter. The view compares spotifyArtGen() against what it last
// drew; when it differs it locks the buffer, decodes straight to the panel,
// then records the new generation. Locking blocks the service from overwriting
// the buffer mid-decode (it simply waits).
uint32_t       spotifyArtGen();
const uint8_t *spotifyArtLock(size_t *lenOut);  // mutex; nullptr if no art
void           spotifyArtUnlock();

// ---- transport commands (posted by the view, run by the service) ----
enum class SpotifyCmd : uint8_t {
  PlayPause,
  Next,
  Prev,
  VolumeDelta,   // arg = signed percent step (e.g. +10 / -10)
  VolumeSet,     // arg = absolute percent 0..100
  Resync         // force an immediate now-playing poll
};
void spotifyPostCmd(SpotifyCmd cmd, int16_t arg = 0);

// ---- one-time setup, driven from the web config page (wifi_svc.cpp) ----
// All of these just stash input under the service mutex and flag the task; the
// task performs NVS writes and network calls on its next cycle.
void spotifySetClientId(const char *clientId);       // persist + re-evaluate auth
bool spotifySubmitAuthCode(const char *codeOrUrl);   // accept pasted code/redirect URL
void spotifyLogout();                                // clear refresh token -> re-auth

// Snapshots for the web status block.
SpotifyAuth spotifyAuthState();
bool        spotifyHasClientId();
// Copy the current PKCE authorize URL (valid while auth == NeedAuth). Returns
// the length written (0 if none). Used by the on-watch QR screen.
size_t      spotifyAuthorizeUrl(char *buf, size_t buflen);

// ---- the launcher view ----
#include "view.h"
class SpotifyView : public View {
public:
  // Delta-drawn UI; 5 Hz keeps the progress bar and art fresh.
  uint16_t desiredFrameMs() const override { return 200; }
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  void onExit()  override;
private:
  void drawChrome();
  void drawArtPlaceholder();
  void drawSetup(const SpotifySnapshot &s);
  void drawNowPlaying(const SpotifySnapshot &s);
  void drawProgress(const SpotifySnapshot &s, bool full);
  void drawControls(const SpotifySnapshot &s);
  void adjustVolume(const SpotifySnapshot &s, int delta);

  bool     firstDraw   = true;
  uint32_t lastArtGen  = 0;
  bool     artDrawn    = false;          // (setup screens) "QR + caption painted"
  char     shownTrackId[48]       = "";  // track whose title/artist is painted
  char     shownArtTrackId[48]    = "";  // track whose real art is painted
  char     placeholderTrackId[48] = "";  // track the art placeholder is painted for
  SpotifyAuth shownAuth = (SpotifyAuth)0xFF;
  // progress-bar delta cache
  int      lastBarPx   = -1;
  uint32_t lastElapsed = 0xFFFFFFFF;
  // optimistic play/pause echo so the icon flips instantly on tap
  bool     optimisticPlaying = false;
  bool     haveOptimistic    = false;
  // tap-vs-swipe discrimination so a swipe's initial finger-down doesn't also
  // register as a play/pause tap (the cause of phantom toggling)
  uint16_t downX = 0, downY = 0;
  uint32_t downMs = 0;
  bool     touchDown        = false;
  bool     swipedSinceDown  = false;
  uint32_t lastActionMs     = 0;
  uint16_t holdCount        = 0;   // TouchHolds seen since finger-down (~30ms each)
  uint32_t lastUpMs         = 0;
  // transient volume readout shown in the control band after a volume swipe
  int      volOverlayVal   = -1;
  uint32_t volOverlayUntil = 0;
  // control-band delta cache (avoid per-frame repaint / flicker)
  int      lastCtrlMode    = -1;   // -1 none, 0 transport, 1 volume, 2 offline
  bool     lastCtrlPlaying = false;
  int      lastCtrlVol     = -1;
  bool     lastCtrlPremium = true;
  bool     lastCtrlVolSup  = true;
};
