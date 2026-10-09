# Spotify app — setup & usage

A standalone Spotify "now playing" viewer + remote that runs entirely on the
EWatch over WiFi. It shows what's playing on your Spotify account and controls
playback (play/pause, skip, volume) on whatever device is active on your Spotify
Connect — phone, desktop, or speaker. The watch is a **remote**, not a playback
device; it never streams audio.

- **Reading** now-playing works on any account (Free or Premium).
- **Controls** (play/pause, next/prev, volume) require **Spotify Premium** — the
  Spotify API rejects them otherwise. The watch detects this and dims the
  on-screen controls when it sees `PREMIUM_REQUIRED`.

---

## One-time setup

### 1. Create a Spotify Developer app (gives you a Client ID)

1. Go to <https://developer.spotify.com/dashboard> and log in.
2. **Create app**. Name/description anything.
3. Under **Redirect URIs**, add exactly:

   ```
   http://127.0.0.1:8888/callback
   ```

   (Loopback HTTP is the only non-HTTPS redirect Spotify still accepts after the
   Nov 2025 OAuth migration — that's why we use it.)
4. For **APIs used**, tick **Web API**. Save.
5. Copy the **Client ID** (Settings → Basic Information). You do **not** need the
   client secret — the watch uses PKCE, so no secret is ever stored on-device.

### 2. Get the watch on WiFi (Client mode)

On the watch: **Settings → WiFi** → enable, set **Client** mode, and add your
home network (SSID + password). Wait until it shows connected. The watch's
config page is then reachable on your LAN at **http://ewatch.local/** (or the IP
shown on the WiFi settings screen).

### 3. Enter the Client ID

In a browser on any device on the same network, open **http://ewatch.local/**,
scroll to the **Spotify** section, paste your **Client ID**, and **Save**.

### 4. Sign in (QR + paste, no PC needed)

1. On the watch, open the **Spotify** app (swipe left from the watch face → tap
   the **Spotify** tile). It shows a **QR code**.
2. **Scan the QR with your phone** and approve access in Spotify.
3. Spotify redirects your phone to `http://127.0.0.1:8888/callback?code=…`. Your
   phone shows a "can't reach this page" error — **that's expected**. The useful
   part is the URL in the address bar.
4. **Copy the whole URL** (or just the `code=…` value).
5. Back on **http://ewatch.local/** → **Spotify** section → paste it into
   **"Paste the redirect URL (or just the code)"** and submit.

The watch exchanges the code for tokens and stores a **refresh token in NVS**.
From now on it signs itself in silently on every boot — **no browser or PC ever
again**. (To start over, use **Sign out of Spotify** on the config page.)

---

## Using it

Open the **Spotify** tile. Once signed in and something is playing you'll see
album art, title, artist, an animated progress bar, and transport controls.

| Action | Gesture |
|---|---|
| Play / pause | **Tap** the centre / album art |
| Previous track | **Swipe left**, or tap the left of the control row |
| Next track | **Swipe right**, or tap the right of the control row |
| Volume up / down | **Swipe up / down** (shows a `VOL nn%` readout) |
| Back to app list | **Hardware button (SW2)**, or tap the top-left chevron |

Every control buzzes the haptic. Commands fire **optimistically** (the UI
updates immediately) and reconcile on the next poll (~2 s). The progress bar is
interpolated locally between polls so it animates smoothly.

**States you may see**

- *WiFi not connected* — finish step 2.
- *No Spotify Client ID yet* — finish step 3.
- *Scan the QR…* — finish step 4.
- *Nothing playing* — no active device; start playback somewhere first.
- Dimmed controls — your account isn't Premium (display still works).

---

## Implementation notes (for future edits)

- **`src/apps/spotify.cpp`** — the service: a background FreeRTOS task (core 0,
  16 KB stack) that owns *all* Spotify network I/O. PKCE auth, token refresh,
  `GET /v1/me/player` polling (ArduinoJson with a filter), transport commands,
  and album-art download. State lives behind its own mutex; the global `model`
  is untouched apart from `Screen::Spotify`.
- **`src/apps/spotify_view.cpp`** — `SpotifyView`: renders from snapshots and
  posts commands. Album-art JPEGs are decoded **on the render task** (JPEGDEC)
  so the single-painter rule holds; the service only hands over raw bytes.
- **Secrets** (Client ID + refresh token) persist in NVS namespace `spotify`,
  separate from the `ewatch` settings namespace.
- **TLS** uses `WiFiClientSecure::setInsecure()` (no cert validation). **TODO:**
  pin Spotify's root CA for production (`api.spotify.com`,
  `accounts.spotify.com`, `i.scdn.co`).
- **Tuning knobs** in `spotify.cpp`: `POLL_PLAYING_MS` / `POLL_IDLE_MS` (poll
  cadence), `ART_MAX` (max album-art bytes). `429` responses honour
  `Retry-After`.

### Things to verify on first hardware run

- **Album-art colour.** Decode is set to `RGB565_LITTLE_ENDIAN` +
  `draw16bitRGBBitmap`. If art comes out red/blue-swapped, switch to
  `RGB565_BIG_ENDIAN` and `draw16bitBeRGBBitmap` (both noted in
  `spotify_view.cpp`).
- **QR scannability.** The authorize URL is ~330 bytes → QR version is chosen to
  fit (~v12, 65×65) and drawn at 2 px/module (~130 px). If a phone struggles,
  increase brightness or hold closer. NB: the QRCode lib does not bounds-check
  data vs version (it overflows a stack VLA), so `drawQr()` picks a fitting
  version from a capacity table up front — never pass it an undersized version.
- **TLS memory / task stack.** If you see watchdog resets while signing in or
  polling, bump the `spotify` task stack in `spotifySvcStartTask()`.
