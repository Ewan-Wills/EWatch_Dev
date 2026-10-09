// Spotify service — the only place Spotify network I/O happens. See spotify.h.
//
// Gated on EWATCH_ENABLE_WIFI: without the radio there is nothing to talk to,
// and we don't want to drag WiFiClientSecure/HTTPClient into a no-WiFi build.
#include "spotify.h"

#if defined(EWATCH_ENABLE_WIFI) && EWATCH_ENABLE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>
#include "model.h"

// ---------- config ----------
static const char *REDIRECT_URI = "http://127.0.0.1:8888/callback";
static const char *SCOPES =
    "user-read-playback-state user-modify-playback-state "
    "user-read-currently-playing";
static const uint32_t POLL_PLAYING_MS = 2000;   // poll cadence while playing
static const uint32_t POLL_IDLE_MS    = 4000;   // slower when paused / idle
static const size_t   ART_MAX         = 100 * 1024;

// Queued transport command (view -> service task).
struct SpotifyCmdMsg { SpotifyCmd cmd; int16_t arg; };

// ---------- protected state ----------
static SemaphoreHandle_t spMutex  = nullptr;   // guards snapshot + config strings
static SemaphoreHandle_t artMutex = nullptr;   // guards the album-art buffer
static QueueHandle_t     cmdQueue = nullptr;
static Preferences       spPrefs;

static SpotifySnapshot   snap;                 // under spMutex
static char  gClientId[128]   = "";            // under spMutex (set by web)
static char  gPendingCode[600]= "";            // under spMutex (set by web)
static bool  gHavePending     = false;         // under spMutex
static bool  gClientIdDirty   = false;         // under spMutex (persist requested)
static bool  gLogout          = false;         // under spMutex
static char  gAuthorizeUrl[600] = "";          // under spMutex (for the QR view)

// task-private (only the service task touches these) ----------------
static char     refreshToken[300] = "";
static char     accessToken[400]  = "";
static uint32_t accessExpiryMs    = 0;
static char     pkceVerifier[80]  = "";
static bool     authUrlBuilt      = false;
static char     artTrackId[48]    = "";        // track id the current art is for
static uint32_t nextPollMs        = 0;

// album art buffer (PSRAM) -------------------------------------------
static uint8_t  *artBuf  = nullptr;            // under artMutex
static size_t    artLen  = 0;                  // under artMutex
static uint32_t  artGen  = 0;                  // under artMutex

// ===================================================================
// small helpers
// ===================================================================
struct SpLock {
  SpLock()  { if (spMutex) xSemaphoreTake(spMutex, portMAX_DELAY); }
  ~SpLock() { if (spMutex) xSemaphoreGive(spMutex); }
};

static String b64url(const uint8_t *data, size_t len) {
  size_t cap = 4 * ((len + 2) / 3) + 1;
  unsigned char out[128];
  if (cap > sizeof(out)) return String();
  size_t olen = 0;
  if (mbedtls_base64_encode(out, sizeof(out), &olen, data, len) != 0) return String();
  String s((const char *)out, olen);
  s.replace("+", "-");
  s.replace("/", "_");
  s.replace("=", "");
  return s;
}

static String urlEncode(const String &in) {
  String out; out.reserve(in.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

static void setAuth(SpotifyAuth a) { SpLock lk; snap.auth = a; }

// Extract the OAuth code from whatever the user pasted (full redirect URL or
// the bare code). Tolerant of trailing '&...' params and whitespace.
static String extractCode(const String &pasted) {
  String s = pasted; s.trim();
  int i = s.indexOf("code=");
  if (i >= 0) {
    s = s.substring(i + 5);
    int amp = s.indexOf('&');
    if (amp >= 0) s = s.substring(0, amp);
  }
  s.trim();
  return s;
}

// ===================================================================
// PKCE + authorize URL
// ===================================================================
static void buildAuthorizeUrl() {
  char id[128];
  { SpLock lk; strncpy(id, gClientId, sizeof(id)); id[sizeof(id) - 1] = '\0'; }
  if (!id[0]) return;

  uint8_t rnd[32];
  esp_fill_random(rnd, sizeof(rnd));
  String verifier = b64url(rnd, sizeof(rnd));          // 43 chars, unreserved
  strncpy(pkceVerifier, verifier.c_str(), sizeof(pkceVerifier));
  pkceVerifier[sizeof(pkceVerifier) - 1] = '\0';

  uint8_t hash[32];
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts_ret(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update_ret(&shaCtx, (const unsigned char *)verifier.c_str(),
                            verifier.length());
  mbedtls_sha256_finish_ret(&shaCtx, hash);
  mbedtls_sha256_free(&shaCtx);
  String challenge = b64url(hash, sizeof(hash));

  String url = "https://accounts.spotify.com/authorize?response_type=code";
  url += "&client_id="; url += id;
  url += "&scope=";     url += urlEncode(SCOPES);
  url += "&code_challenge_method=S256&code_challenge="; url += challenge;
  url += "&redirect_uri="; url += urlEncode(REDIRECT_URI);

  { SpLock lk;
    strncpy(gAuthorizeUrl, url.c_str(), sizeof(gAuthorizeUrl));
    gAuthorizeUrl[sizeof(gAuthorizeUrl) - 1] = '\0'; }
  authUrlBuilt = true;
  Serial.printf("SPOT: authorize URL ready (%u chars)\n", url.length());
}

// ===================================================================
// HTTP plumbing (shared, timeout-bounded, retry-on-transient)
// ===================================================================
// Bound every network op so a stalled TLS handshake on a weak link can't hang
// the task past the 20 s task-watchdog. Handshake timeout is in SECONDS.
static const uint32_t TLS_HANDSHAKE_S  = 12;     // < 20 s WDT
static const int      HTTP_CONNECT_MS  = 8000;
static const int      HTTP_READ_MS     = 8000;
static const int      HTTP_MAX_TRIES   = 2;      // attempts for transient failures

static void prepTls(WiFiClientSecure &c) {
  c.setInsecure();                       // TODO: pin Spotify's root CA for prod
  c.setHandshakeTimeout(TLS_HANDSHAKE_S);
  c.setTimeout(HTTP_READ_MS / 1000);     // socket read timeout (seconds)
}
static void prepHttp(HTTPClient &h) {
  h.setConnectTimeout(HTTP_CONNECT_MS);
  h.setTimeout(HTTP_READ_MS);
  h.setReuse(false);
}
// A connection/TLS error (negative) or a 5xx is worth retrying; auth/4xx and
// 429 are definitive for the caller to handle (no blind retry).
static bool httpTransient(int code) { return code <= 0 || code >= 500; }

struct HttpResp { int status; String body; int retryAfter; };

// One request, retried up to `maxTries` on transient failure with linear
// backoff. Captures the (small) response body for token/error parsing — NOT for
// the large player poll, which streams + filters separately. `bearer` may be
// null. Pass maxTries=1 for non-idempotent calls (skip next/previous) so a
// dropped-after-success connection can't double-act.
static HttpResp httpDo(const char *method, const String &url, const String &body,
                       const char *contentType, const char *bearer, int maxTries) {
  HttpResp r; r.status = 0; r.retryAfter = 0;
  for (int attempt = 1; attempt <= maxTries; attempt++) {
    esp_task_wdt_reset();
    WiFiClientSecure client; prepTls(client);
    HTTPClient https; prepHttp(https);
    if (!https.begin(client, url)) { r.status = -1; vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    if (bearer && bearer[0]) https.addHeader("Authorization", String("Bearer ") + bearer);
    if (body.length()) https.addHeader("Content-Type", contentType ? contentType : "application/json");
    // Spotify's player endpoints 411 without a Content-Length; HTTPClient only
    // adds one for a non-empty body, so set it explicitly for empty PUT/POSTs.
    else               https.addHeader("Content-Length", "0");
    const char *hdrs[] = { "Retry-After" };
    https.collectHeaders(hdrs, 1);
    int code = https.sendRequest(method, body);
    r.status = code;
    if (code == 429) r.retryAfter = https.header("Retry-After").toInt();
    if (code > 0)    r.body = https.getString();
    https.end();
    if (!httpTransient(code)) return r;
    Serial.printf("SPOT: %s transient %d (try %d/%d)\n", method, code, attempt, maxTries);
    if (attempt < maxTries) vTaskDelay(pdMS_TO_TICKS(300 * attempt));
  }
  return r;
}

// ===================================================================
// token endpoint
// ===================================================================
// POST application/x-www-form-urlencoded to accounts.spotify.com/api/token.
// Returns HTTP status; on 200 parses access/refresh/expiry into the statics
// and persists a (possibly rotated) refresh token.
static int tokenPost(const String &form) {
  HttpResp r = httpDo("POST", "https://accounts.spotify.com/api/token", form,
                      "application/x-www-form-urlencoded", nullptr, HTTP_MAX_TRIES);
  if (r.status == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, r.body)) return -2;
    const char *at = doc["access_token"]  | "";
    const char *rt = doc["refresh_token"] | "";
    uint32_t    ex = doc["expires_in"]    | 3600;
    if (at[0]) {
      strncpy(accessToken, at, sizeof(accessToken));
      accessToken[sizeof(accessToken) - 1] = '\0';
      // Refresh 60 s early to avoid racing the expiry on a slow link.
      accessExpiryMs = millis() + (ex > 90 ? (ex - 60) : ex) * 1000UL;
    }
    if (rt[0]) {
      strncpy(refreshToken, rt, sizeof(refreshToken));
      refreshToken[sizeof(refreshToken) - 1] = '\0';
      spPrefs.putString("refresh", refreshToken);
    }
  }
  return r.status;
}

static bool exchangeCode(const String &code) {
  char id[128];
  { SpLock lk; strncpy(id, gClientId, sizeof(id)); id[sizeof(id) - 1] = '\0'; }
  String form = "grant_type=authorization_code";
  form += "&code=";          form += urlEncode(code);
  form += "&redirect_uri=";  form += urlEncode(REDIRECT_URI);
  form += "&client_id=";     form += id;
  form += "&code_verifier="; form += pkceVerifier;
  int st = tokenPost(form);
  Serial.printf("SPOT: code exchange -> HTTP %d\n", st);
  return st == 200 && accessToken[0] && refreshToken[0];
}

static bool refreshAccess() {
  if (!refreshToken[0]) return false;
  char id[128];
  { SpLock lk; strncpy(id, gClientId, sizeof(id)); id[sizeof(id) - 1] = '\0'; }
  String form = "grant_type=refresh_token";
  form += "&refresh_token="; form += urlEncode(refreshToken);
  form += "&client_id=";     form += id;
  int st = tokenPost(form);
  Serial.printf("SPOT: token refresh -> HTTP %d\n", st);
  if (st == 400 || st == 401) {
    // Refresh token revoked/invalid — force a fresh sign-in.
    refreshToken[0] = '\0';
    spPrefs.remove("refresh");
    accessToken[0] = '\0';
    authUrlBuilt = false;
  }
  return st == 200 && accessToken[0];
}

// Ensure we hold a non-expired access token. Refreshes if needed.
static bool ensureToken() {
  if (accessToken[0] && (int32_t)(accessExpiryMs - millis()) > 0) return true;
  return refreshAccess();
}

// ===================================================================
// album art
// ===================================================================
// One download attempt into a fresh PSRAM buffer. On a *complete* image, swaps
// it in under artMutex and bumps the generation. Returns true only then.
static bool fetchArtOnce(const String &url) {
  esp_task_wdt_reset();
  WiFiClientSecure client; prepTls(client);
  HTTPClient https; prepHttp(https);
  if (!https.begin(client, url)) return false;
  int code = https.GET();
  if (code != 200) { https.end(); Serial.printf("SPOT: art HTTP %d\n", code); return false; }

  int total = https.getSize();
  size_t cap = (total > 0 && (size_t)total < ART_MAX) ? (size_t)total : ART_MAX;
  uint8_t *buf = (uint8_t *)ps_malloc(cap);
  if (!buf) { https.end(); Serial.println("SPOT: art alloc failed"); return false; }

  WiFiClient *stream = https.getStreamPtr();
  size_t got = 0;
  uint32_t lastData = millis();
  while (https.connected() && got < cap &&
         (total < 0 || got < (size_t)total)) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = cap - got; if (want > avail) want = avail;
      int r = stream->readBytes(buf + got, want);
      if (r <= 0) break;
      got += r;
      lastData = millis();
    } else {
      if (millis() - lastData > HTTP_READ_MS) break;   // stall guard
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  https.end();

  // Reject a truncated download — a partial JPEG would just fail to decode.
  if (got < 64 || (total > 0 && got < (size_t)total)) {
    free(buf);
    Serial.printf("SPOT: art incomplete (%u/%d)\n", (unsigned)got, total);
    return false;
  }

  if (xSemaphoreTake(artMutex, portMAX_DELAY) == pdTRUE) {
    if (artBuf) free(artBuf);
    artBuf = buf; artLen = got; artGen++;
    xSemaphoreGive(artMutex);
    Serial.printf("SPOT: art %u bytes (gen %lu)\n", (unsigned)got, (unsigned long)artGen);
    return true;
  }
  free(buf);
  return false;
}

// Download album art, retrying once on a transient failure. Runs on the
// service task only. Returns true only if a complete image was installed.
static bool fetchArt(const String &url) {
  for (int attempt = 1; attempt <= HTTP_MAX_TRIES; attempt++) {
    if (fetchArtOnce(url)) return true;
    vTaskDelay(pdMS_TO_TICKS(300 * attempt));
  }
  return false;
}

// ===================================================================
// now-playing poll
// ===================================================================
// Build the ArduinoJson filter once: only the fields we render survive parsing.
static void buildPlayerFilter(JsonDocument &filter) {
  filter["is_playing"]  = true;
  filter["progress_ms"] = true;
  filter["device"]["volume_percent"] = true;
  JsonObject item = filter["item"].to<JsonObject>();
  item["name"]        = true;
  item["id"]          = true;
  item["duration_ms"] = true;
  item["artists"][0]["name"] = true;
  JsonObject alb = item["album"].to<JsonObject>();
  alb["name"] = true;
  alb["images"][0]["url"]   = true;
  alb["images"][0]["width"] = true;
}

// Choose the album image best suited to the ~240 px art box: the smallest one
// at least 240 wide, else the widest available.
static String pickArtUrl(JsonArrayConst images) {
  String best; int bestW = 0; String wide; int wideW = -1;
  for (JsonObjectConst im : images) {
    int w = im["width"] | 0;
    const char *u = im["url"] | "";
    if (!u[0]) continue;
    if (w > wideW) { wideW = w; wide = u; }
    if (w >= 240 && (bestW == 0 || w < bestW)) { bestW = w; best = u; }
  }
  return best.length() ? best : wide;
}

// Returns the HTTP status. Updates the snapshot; on a track change kicks off an
// album-art download. `retryAfter` receives the 429 backoff seconds if any.
static int pollPlayer(int &retryAfter) {
  if (!ensureToken()) return 401;
  retryAfter = 0;
  esp_task_wdt_reset();

  WiFiClientSecure client; prepTls(client);
  HTTPClient https; prepHttp(https);
  if (!https.begin(client, "https://api.spotify.com/v1/me/player")) return -1;
  https.addHeader("Authorization", String("Bearer ") + accessToken);
  const char *hdrs[] = { "Retry-After" };
  https.collectHeaders(hdrs, 1);
  int code = https.GET();

  if (code == 429) retryAfter = https.header("Retry-After").toInt();

  if (code != 200) {
    https.end();
    if (code == 204) {                       // nothing playing / no active device
      SpLock lk; snap.playback = SpotifyPlayback::Idle;
      snap.lastHttpStatus = 204; snap.rateLimited = false;
      snap.track[0] = snap.artist[0] = snap.album[0] = snap.trackId[0] = '\0';
      snap.progressMs = snap.durationMs = 0; snap.isPlaying = false;
    } else if (code == 429) {
      SpLock lk; snap.rateLimited = true; snap.lastHttpStatus = 429;
    } else if (code == 401) {
      accessToken[0] = '\0';                 // force a refresh before the next poll
      SpLock lk; snap.lastHttpStatus = 401;
    } else {
      SpLock lk; snap.lastHttpStatus = code;
      if (code < 0) snap.playback = SpotifyPlayback::Offline;
    }
    return code;
  }

  JsonDocument filter;
  buildPlayerFilter(filter);
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { SpLock lk; snap.lastHttpStatus = -10; return -10; }

  JsonObjectConst item = doc["item"];
  if (item.isNull()) {                       // device active but between tracks / ad
    SpLock lk; snap.playback = SpotifyPlayback::Idle; snap.lastHttpStatus = 200;
    snap.rateLimited = false;
    return 200;
  }

  bool playing = doc["is_playing"] | false;
  uint32_t prog = doc["progress_ms"] | 0;
  uint32_t dur  = item["duration_ms"] | 0;
  const char *name = item["name"] | "";
  const char *id   = item["id"]   | "";
  const char *alb  = item["album"]["name"] | "";

  // Join artist names with ", ".
  String artists;
  for (JsonObjectConst a : item["artists"].as<JsonArrayConst>()) {
    const char *an = a["name"] | "";
    if (!an[0]) continue;
    if (artists.length()) artists += ", ";
    artists += an;
  }

  String artUrl = pickArtUrl(item["album"]["images"].as<JsonArrayConst>());
  bool trackChanged = strncmp(id, artTrackId, sizeof(artTrackId)) != 0;

  { SpLock lk;
    snap.playback = playing ? SpotifyPlayback::Playing : SpotifyPlayback::Paused;
    snap.isPlaying = playing;
    snap.progressMs = prog;
    snap.durationMs = dur;
    snap.progressAnchorMs = millis();
    strncpy(snap.track,  name,            sizeof(snap.track));  snap.track[sizeof(snap.track) - 1]   = '\0';
    strncpy(snap.artist, artists.c_str(), sizeof(snap.artist)); snap.artist[sizeof(snap.artist) - 1] = '\0';
    strncpy(snap.album,  alb,             sizeof(snap.album));  snap.album[sizeof(snap.album) - 1]   = '\0';
    strncpy(snap.trackId, id,             sizeof(snap.trackId));snap.trackId[sizeof(snap.trackId) - 1]= '\0';
    if (!doc["device"]["volume_percent"].isNull()) {
      snap.volumePct = doc["device"]["volume_percent"] | snap.volumePct;
      snap.hasVolume = true;
    }
    snap.rateLimited = false;
    snap.lastHttpStatus = 200;
  }

  if (trackChanged && artUrl.length()) {
    esp_task_wdt_reset();
    if (fetchArt(artUrl)) {     // only claim the art on a complete download
      strncpy(artTrackId, id, sizeof(artTrackId)); artTrackId[sizeof(artTrackId) - 1] = '\0';
    }
  }
  // Mirror which track the cached art belongs to so the view can tell whether
  // its decoded image still matches what's playing (e.g. after an idle gap).
  { SpLock lk; strncpy(snap.artTrackId, artTrackId, sizeof(snap.artTrackId));
    snap.artTrackId[sizeof(snap.artTrackId) - 1] = '\0'; }
  return 200;
}

// ===================================================================
// transport commands
// ===================================================================
// Issue a control request (httpDo already retries transient failures + sets
// Content-Length). Refreshes + retries once on 401; flags loss of Premium on a
// PREMIUM_REQUIRED 403. Returns the final HTTP status.
static int control(const char *method, const String &path, const String &body) {
  String url = String("https://api.spotify.com") + path;
  // next/previous are non-idempotent — never auto-retry (a retry after a
  // dropped-but-successful request would skip an extra track). play/pause/volume
  // are idempotent, so a transient retry is safe.
  int tries = (strcmp(method, "POST") == 0) ? 1 : HTTP_MAX_TRIES;
  for (int pass = 0; pass < 2; pass++) {
    if (!ensureToken()) return 401;
    HttpResp r = httpDo(method, url, body, "application/json", accessToken, tries);
    Serial.printf("SPOT: %s %s -> %d\n", method, path.c_str(), r.status);
    if (r.status == 401 && pass == 0) { accessToken[0] = '\0'; continue; }
    if (r.status == 403 && r.body.indexOf("PREMIUM_REQUIRED") >= 0) {
      SpLock lk; snap.premium = false;
    }
    return r.status;
  }
  return 401;
}

static inline bool ok2xx(int s) { return s >= 200 && s < 300; }

static void runCommand(SpotifyCmd cmd, int16_t arg) {
  bool     playing; uint8_t vol; bool hasVol;
  { SpLock lk; playing = snap.isPlaying; vol = snap.volumePct; hasVol = snap.hasVolume; }

  switch (cmd) {
    case SpotifyCmd::PlayPause: {
      int st = control("PUT", playing ? "/v1/me/player/pause" : "/v1/me/player/play", "");
      // Only commit the optimistic flip if the server accepted it; otherwise the
      // next poll reconciles back to reality.
      if (ok2xx(st)) { SpLock lk; snap.isPlaying = !playing;
        snap.playback = snap.isPlaying ? SpotifyPlayback::Playing : SpotifyPlayback::Paused; }
      break;
    }
    case SpotifyCmd::Next:
      control("POST", "/v1/me/player/next", "");
      break;
    case SpotifyCmd::Prev:
      control("POST", "/v1/me/player/previous", "");
      break;
    case SpotifyCmd::VolumeDelta:
    case SpotifyCmd::VolumeSet: {
      int target = (cmd == SpotifyCmd::VolumeSet) ? arg : (int)vol + arg;
      if (!hasVol && cmd == SpotifyCmd::VolumeDelta) target = arg > 0 ? 50 : 0;
      if (target < 0) target = 0; if (target > 100) target = 100;
      int st = control("PUT", String("/v1/me/player/volume?volume_percent=") + target, "");
      { SpLock lk;
        // 403/404 => the active device refuses remote volume (very common on
        // phones). Surface it; don't pretend the change took.
        if (st == 403 || st == 404) snap.volumeSupported = false;
        else if (ok2xx(st))         { snap.volumeSupported = true; snap.volumePct = target; snap.hasVolume = true; } }
      break;
    }
    case SpotifyCmd::Resync:
      break;
  }
  nextPollMs = millis();    // reconcile UI against reality promptly
}

// ===================================================================
// service task
// ===================================================================
static void evaluateAuth() {
  bool wifiOk;
  { ModelLock lk; wifiOk = model.wifiConnected; }
  bool haveRefresh = refreshToken[0] != '\0';

  bool haveId; bool pending; char code[600];
  { SpLock lk;
    haveId  = gClientId[0] != '\0';
    pending = gHavePending;
    if (pending) { strncpy(code, gPendingCode, sizeof(code)); code[sizeof(code) - 1] = '\0'; } }

  // WiFi down: if already signed in, keep the now-playing UI but mark Offline
  // (a transient drop shouldn't bounce the user to the setup screen). Only show
  // the WiFi prompt if the watch was never set up.
  if (!wifiOk) {
    if (haveRefresh) { setAuth(SpotifyAuth::Ready); SpLock lk; snap.playback = SpotifyPlayback::Offline; }
    else             setAuth(SpotifyAuth::NeedWifi);
    return;
  }

  if (!haveId) { setAuth(SpotifyAuth::NeedClientId); return; }

  if (pending) {
    setAuth(SpotifyAuth::Authorizing);
    bool ok = exchangeCode(extractCode(code));
    { SpLock lk; gHavePending = false; gPendingCode[0] = '\0'; }
    if (!ok) { setAuth(SpotifyAuth::AuthError); return; }
    // success falls through to the refresh-token path below next tick
  }

  if (!refreshToken[0]) {
    if (!authUrlBuilt) buildAuthorizeUrl();
    setAuth(SpotifyAuth::NeedAuth);
    return;
  }

  // Have a refresh token but couldn't mint an access token. If refreshAccess
  // wiped it (definitive 400/401 -> revoked), re-sign-in; otherwise it was a
  // transient network failure — stay on the now-playing screen, Offline, and
  // keep retrying rather than throwing a scary auth error.
  if (!ensureToken()) {
    if (!refreshToken[0]) { setAuth(SpotifyAuth::NeedAuth); }
    else { setAuth(SpotifyAuth::Ready); SpLock lk; snap.playback = SpotifyPlayback::Offline; }
    return;
  }
  setAuth(SpotifyAuth::Ready);
}

static void serviceTask(void *) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();

    // ---- apply config changes from the web handlers ----
    bool persistId = false; char idToPersist[128]; bool doLogout = false;
    { SpLock lk;
      if (gClientIdDirty) { gClientIdDirty = false; persistId = true;
        strncpy(idToPersist, gClientId, sizeof(idToPersist)); idToPersist[sizeof(idToPersist) - 1] = '\0'; }
      if (gLogout) { gLogout = false; doLogout = true; } }
    if (persistId) {
      spPrefs.putString("clientId", idToPersist);
      authUrlBuilt = false;                      // rebuild the QR for the new app
      Serial.println("SPOT: client id saved");
    }
    if (doLogout) {
      refreshToken[0] = accessToken[0] = artTrackId[0] = '\0';
      spPrefs.remove("refresh");
      authUrlBuilt = false;
      Serial.println("SPOT: logged out");
    }

    evaluateAuth();

    SpotifyAuth a; { SpLock lk; a = snap.auth; }
    static int pollErrs = 0;
    if (a == SpotifyAuth::Ready) {
      bool wifiOk; { ModelLock lk; wifiOk = model.wifiConnected; }
      if (!wifiOk) {
        // Offline: don't hammer the radio, and drop any queued commands so they
        // don't fire late (and wrong) when the link returns.
        { SpLock lk; snap.playback = SpotifyPlayback::Offline; }
        SpotifyCmdMsg drop; while (cmdQueue && xQueueReceive(cmdQueue, &drop, 0) == pdPASS) {}
        nextPollMs = millis() + 1000;
      } else {
        // Drain queued transport commands (bounded so a burst can't monopolise
        // the task on a slow link; leftovers run next cycle).
        SpotifyCmdMsg msg;
        for (int n = 0; n < 4 && cmdQueue &&
                        xQueueReceive(cmdQueue, &msg, 0) == pdPASS; n++) {
          esp_task_wdt_reset();
          runCommand(msg.cmd, msg.arg);
        }
        // Poll now-playing on cadence, with backoff + Offline on repeated error.
        if ((int32_t)(millis() - nextPollMs) >= 0) {
          int retryAfter = 0;
          esp_task_wdt_reset();
          int st = pollPlayer(retryAfter);
          uint32_t gap;
          if (st == 200 || st == 204) {
            pollErrs = 0;
            bool playing; { SpLock lk; playing = snap.isPlaying; }
            gap = playing ? POLL_PLAYING_MS : POLL_IDLE_MS;
          } else if (st == 429 && retryAfter > 0) {
            gap = (uint32_t)retryAfter * 1000UL;
          } else if (st == 401) {
            gap = 600;                       // token refreshed; retry soon
          } else {
            // transient/offline: exponential backoff, capped; flag Offline after
            // a couple of consecutive failures so the UI can say so.
            if (++pollErrs >= 2) { SpLock lk; snap.playback = SpotifyPlayback::Offline; }
            int shift = pollErrs > 3 ? 3 : pollErrs;
            gap = POLL_PLAYING_MS << shift;  // 4s, 8s, 16s
            if (gap > 16000) gap = 16000;
          }
          nextPollMs = millis() + gap;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(120));
  }
}

// ===================================================================
// public API
// ===================================================================
void spotifyInit() {
  if (!spMutex)  spMutex  = xSemaphoreCreateMutex();
  if (!artMutex) artMutex = xSemaphoreCreateMutex();
  if (!cmdQueue) cmdQueue = xQueueCreate(8, sizeof(SpotifyCmdMsg));

  memset(&snap, 0, sizeof(snap));
  snap.auth = SpotifyAuth::NeedWifi;
  snap.playback = SpotifyPlayback::Unknown;
  snap.premium = true;
  snap.volumeSupported = true;
  snap.volumePct = 50;

  spPrefs.begin("spotify", /*readOnly=*/false);
  String id = spPrefs.getString("clientId", "");
  String rt = spPrefs.getString("refresh", "");
  strncpy(gClientId, id.c_str(), sizeof(gClientId)); gClientId[sizeof(gClientId) - 1] = '\0';
  strncpy(refreshToken, rt.c_str(), sizeof(refreshToken)); refreshToken[sizeof(refreshToken) - 1] = '\0';
  Serial.printf("SPOT: init (clientId %s, refresh %s)\n",
                gClientId[0] ? "set" : "unset", refreshToken[0] ? "set" : "unset");
}

void spotifySvcStartTask() {
  // Large stack: TLS handshake + HTTPClient + ArduinoJson recurse deeply.
  xTaskCreatePinnedToCore(serviceTask, "spotify", 16384, nullptr, 4, nullptr, 0);
}

void spotifyGetSnapshot(SpotifySnapshot *out) {
  SpLock lk; *out = snap;
}

uint32_t spotifyArtGen() {
  uint32_t g; if (xSemaphoreTake(artMutex, portMAX_DELAY) == pdTRUE) { g = artGen; xSemaphoreGive(artMutex); return g; }
  return 0;
}
const uint8_t *spotifyArtLock(size_t *lenOut) {
  if (xSemaphoreTake(artMutex, portMAX_DELAY) != pdTRUE) return nullptr;
  if (!artBuf) { xSemaphoreGive(artMutex); return nullptr; }
  if (lenOut) *lenOut = artLen;
  return artBuf;                                  // caller MUST call spotifyArtUnlock()
}
void spotifyArtUnlock() { xSemaphoreGive(artMutex); }

void spotifyPostCmd(SpotifyCmd cmd, int16_t arg) {
  if (!cmdQueue) return;
  SpotifyCmdMsg m{ cmd, arg };
  xQueueSend(cmdQueue, &m, 0);
}

void spotifySetClientId(const char *clientId) {
  if (!clientId) return;
  SpLock lk;
  strncpy(gClientId, clientId, sizeof(gClientId)); gClientId[sizeof(gClientId) - 1] = '\0';
  gClientIdDirty = true;
}

bool spotifySubmitAuthCode(const char *codeOrUrl) {
  if (!codeOrUrl || !codeOrUrl[0]) return false;
  SpLock lk;
  strncpy(gPendingCode, codeOrUrl, sizeof(gPendingCode)); gPendingCode[sizeof(gPendingCode) - 1] = '\0';
  gHavePending = true;
  return true;
}

void spotifyLogout() { SpLock lk; gLogout = true; }

SpotifyAuth spotifyAuthState() { SpLock lk; return snap.auth; }
bool spotifyHasClientId()      { SpLock lk; return gClientId[0] != '\0'; }

size_t spotifyAuthorizeUrl(char *buf, size_t buflen) {
  if (!buf || !buflen) return 0;
  SpLock lk;
  strncpy(buf, gAuthorizeUrl, buflen); buf[buflen - 1] = '\0';
  return strlen(buf);
}

#else  // EWATCH_ENABLE_WIFI == 0 — stubs so the view + main still link.

#include <string.h>
void spotifyInit() {}
void spotifySvcStartTask() {}
void spotifyGetSnapshot(SpotifySnapshot *out) { if (out) { memset(out, 0, sizeof(*out)); out->auth = SpotifyAuth::NeedWifi; } }
uint32_t spotifyArtGen() { return 0; }
const uint8_t *spotifyArtLock(size_t *lenOut) { if (lenOut) *lenOut = 0; return nullptr; }
void spotifyArtUnlock() {}
void spotifyPostCmd(SpotifyCmd, int16_t) {}
void spotifySetClientId(const char *) {}
bool spotifySubmitAuthCode(const char *) { return false; }
void spotifyLogout() {}
SpotifyAuth spotifyAuthState() { return SpotifyAuth::NeedWifi; }
bool spotifyHasClientId() { return false; }
size_t spotifyAuthorizeUrl(char *buf, size_t buflen) { if (buf && buflen) buf[0] = '\0'; return 0; }

#endif  // EWATCH_ENABLE_WIFI
