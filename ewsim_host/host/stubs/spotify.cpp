// ewsim_host stub for src/apps/spotify/spotify.cpp: no network, so the
// Spotify view shows its real "needs WiFi" setup screen.
#include <Arduino.h>
#include "spotify.h"
#include "host_sim.h"
void spotifyInit() {}
void spotifySvcStartTask() {}
void spotifyGetSnapshot(SpotifySnapshot *out) {
  memset(out, 0, sizeof *out);
  out->auth = SpotifyAuth::NeedWifi;
  out->playback = SpotifyPlayback::Unknown;
  out->premium = true;
  out->volumeSupported = true;
  ewsimHostNote("Spotify: network not simulated (shows the needs-WiFi screen)");
}
uint32_t spotifyArtGen() { return 0; }
const uint8_t *spotifyArtLock(size_t *lenOut) { if (lenOut) *lenOut = 0; return nullptr; }
void spotifyArtUnlock() {}
void spotifyPostCmd(SpotifyCmd, int16_t) {}
void spotifySetClientId(const char *) {}
bool spotifySubmitAuthCode(const char *) { return false; }
void spotifyLogout() {}
SpotifyAuth spotifyAuthState() { return SpotifyAuth::NeedWifi; }
bool spotifyHasClientId() { return false; }
size_t spotifyAuthorizeUrl(char *buf, size_t n) { if (n) buf[0] = 0; return 0; }
