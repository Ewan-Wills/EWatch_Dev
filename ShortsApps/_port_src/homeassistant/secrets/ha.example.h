// Home Assistant secrets — EXAMPLE / TEMPLATE.
//
// Copy this file to `ha.local.h` (same folder) and fill in your real values.
// `ha.local.h` is git-ignored so your token never lands in version control.
//
//   cp secrets/ha.example.h secrets/ha.local.h
//   # then edit secrets/ha.local.h
//
// The app #includes "ha.local.h"; this example header is only a reference and
// a safe placeholder that lets the firmware compile out of the box.
#pragma once

// Base URL of your Home Assistant instance, NO trailing slash.
// Use the LAN IP of the Pi (http, not https — a self-signed cert would need a
// CA bundle we don't ship). Example: "http://192.168.0.50:8123".
#define HA_BASE_URL   "http://homeassistant.local:8123"

// Long-lived access token. In Home Assistant:
//   Profile (click your user, bottom-left) -> Security ->
//   "Long-lived access tokens" -> Create Token.
// Paste the whole token string here.
#define HA_TOKEN      "PASTE_YOUR_LONG_LIVED_TOKEN_HERE"

// The light entity to toggle (must be in the `light.` domain for MVP).
// Find it in HA: Developer Tools -> States -> filter "light.".
#define HA_ENTITY_ID  "light.lamp"

// Optional: friendly name shown on the watch. If left empty the entity id is
// shown instead.
#define HA_LIGHT_NAME "Lamp"
