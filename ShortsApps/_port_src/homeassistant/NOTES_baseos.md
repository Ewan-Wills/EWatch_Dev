# homeassistant — EWatch app notes

Toggle a single Home Assistant light from the watch and show its state. LOW
priority MVP. WiFi stays **on** (`-DEWATCH_ENABLE_WIFI=1`); the watch is a WiFi
client calling the HA REST API directly (no PC companion).

Compile status (final): `ewatch SUCCESS` — Flash 31.3% (984,457 B), RAM 15.7%.
Build is clean; the one `hapticBuzz(400,220)` overflow warning is pre-existing
BaseOS code in `controller.cpp`, not this app.

---

## What Ewan must do before flashing (REQUIRED)

The app reads its config from a compiled-in header that is **git-ignored**. The
repo ships only the template. On the flashing machine:

```sh
cd ShortsApps/homeassistant
cp secrets/ha.example.h secrets/ha.local.h
# edit secrets/ha.local.h with your real values
```

Fill in `secrets/ha.local.h`:

| Macro | What | Where to get it |
|---|---|---|
| `HA_BASE_URL` | Base URL, **no trailing slash**, plain `http://` | The Pi's LAN IP + port, e.g. `http://192.168.0.50:8123` |
| `HA_TOKEN` | Long-lived access token | HA → your Profile → Security → "Long-lived access tokens" → Create |
| `HA_ENTITY_ID` | The light to toggle (must be `light.` domain) | HA → Developer Tools → States → filter `light.` (the lamp) |
| `HA_LIGHT_NAME` | Friendly label shown on the watch (optional) | anything, e.g. `Lamp` |

Then the normal flash from the primer:

```sh
export PLATFORMIO_CORE_DIR=/Users/ewills/Documents/EWatch/.pio-core-shortsapps
~/.platformio/penv/bin/pio run -e ewatch -t upload -t monitor
```

The watch must also be joined to the same WiFi as the Pi. WiFi join uses the
existing BaseOS flow — Settings → WiFi (Client mode, known network / the
`EWATCH_SETUP` SoftAP web form). This app does **not** add any WiFi-config UI;
it just reads `model.wifiConnected`.

> `http://` is deliberate. HA over `https://` with a self-signed cert would
> need a CA bundle we don't ship. If HA is only reachable over TLS, that's a
> follow-up (pin the CA, like the Spotify app's `WiFiClientSecure`). For a
> lamp on the home Pi, plain HTTP on the LAN is fine.

---

## REST calls used

Plain HTTP, `Authorization: Bearer <HA_TOKEN>` on every request.

- **State poll** — `GET {HA_BASE_URL}/api/states/{HA_ENTITY_ID}`
  Parsed with ArduinoJson using a filter that keeps only `state`; mapped
  `"on"→On`, `"off"→Off`, anything else (`unavailable`/`unknown`)→Unavailable.
  Polled every 5 s when reachable, backing off to 8 s after an error, and once
  immediately on app open and right after a toggle.
- **Toggle** — `POST {HA_BASE_URL}/api/services/light/turn_on` (or `turn_off`)
  with body `{"entity_id":"{HA_ENTITY_ID}"}`. Target is chosen from the last
  known state (On→turn_off, anything else→turn_on). On a 2xx the snapshot flips
  optimistically; the next poll confirms.

All network ops are bounded (6 s connect / 6 s read) to stay under the 20 s task
watchdog, and run on a dedicated FreeRTOS task (`ha`, core 0, 8 KB stack). The
view never touches the network — it reads a mutex-guarded `HASnapshot` and posts
`HACmd` values to a queue, mirroring the Spotify app's architecture.

---

## UI

- Title bar = the light's friendly name.
- Big rounded button, centered: amber when On, dim grey when Off, red when
  Unavailable, with the text `ON` / `OFF` / `N/A` / `--` (`...` while a toggle is
  in flight). Tap it to toggle (haptic buzz on tap).
- Status line near the bottom: `WiFi not connected` / `Set token in ha.local.h`
  / `Connecting...` / `Light is on|off` / `Unavailable` / `HA unreachable`, with
  the last HTTP status in the corner for debugging.
- Back = hardware button or right-swipe (both arrive as `ButtonShort`), or tap
  the top-left chevron.

Reachable from the launcher: swipe up from the watch face → **Home** tile (first
tile, before System).

---

## What's stubbed / MVP limits

- **One light only.** Entity is compile-time; no on-watch entity picker.
- **`light.` domain assumed** for the toggle service calls. A switch/other
  domain would need different service names.
- **No brightness/color** — just on/off.
- **HTTP only**, no TLS (see note above).
- **Config is compile-time** (header), not a web/NVS setting like Spotify's
  tokens. Changing the light/token means editing `ha.local.h` and reflashing.
- Toggle is **not idempotent-guarded**: if a 2xx response is lost after HA
  actually acted, a fast double-tap could double-toggle. For a lamp this is
  harmless; the 5 s poll re-syncs the displayed state regardless.

---

## Risks

- **Wrong URL/token/entity** is the most likely failure — the status line + the
  corner HTTP code are there to diagnose it (401 = bad token, 404 = bad entity,
  connection error = wrong URL/host unreachable).
- Watch and Pi must be on the same reachable network segment.
- `millis()`-based poll cadence resets across deep sleep; harmless here (the app
  re-polls on open). The screen does **not** block auto-sleep, so after the
  idle timeout the watch sleeps as normal and the app re-syncs on wake/open.

---

## Hardware-verify steps (Ewan — this is acceptance; only you can do it)

1. Create `secrets/ha.local.h` from the example and fill in the real Pi URL,
   a fresh long-lived token, and the lamp entity id (see table above).
2. Flash: `pio run -e ewatch -t upload -t monitor` (with the isolated core dir
   exported). Watch the serial — boot prints `HA: init (entity light.xxx, token
   set)`. If it says `token UNSET`, the local header wasn't picked up.
3. On the watch, join WiFi (Settings → WiFi, Client mode) and confirm the face
   shows connected.
4. Swipe up → tap **Home**. Expect the status line to go `Connecting...` → `Light
   is on`/`Light is off` within a few seconds, the button colour matching, and
   the corner code showing `200`.
5. Tap the big button. Expect: haptic buzz, the lamp physically toggles at the
   Pi, the button flips colour/label immediately, and the next poll keeps it in
   sync. Toggle a few times.
6. Cross-check against HA (Developer Tools → States, or the HA app) that the
   watch state matches the real `light.` state after each toggle.
7. Negative checks: break the token in `ha.local.h` → expect `HA unreachable`
   + `401`; turn WiFi off on the watch → expect `WiFi not connected`.

Report back what the status line + corner code show if anything is off — that
pins URL vs token vs entity vs network.
