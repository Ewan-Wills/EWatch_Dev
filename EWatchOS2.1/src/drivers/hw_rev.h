// Board revision: selected at compile time with -DEWATCH_HW_REV=<n> from the
// PlatformIO env (platformio.ini): 21 = V2.1 (default env `ewatch`),
// 20 = V2.0 (env `ewatch_v20`). Everything that differs between the two boards
// is derived here; code includes this instead of testing the revision itself.
// Values measured with HWTests (tools/results/*-20260928-*.txt).
#pragma once

#ifndef EWATCH_HW_REV
#define EWATCH_HW_REV 21
#endif

#if EWATCH_HW_REV == 21
  #define EWATCH_HW_NAME "V2.1"
  // Battery divider R6 = R7 = 100k -> tap = 0.5 x VBAT. With the EN transistor
  // the tap only settles ~30-50 ms after EN goes high (2 ms reads ~0.78 V,
  // i.e. 0 %), so it's sampled >= BAT_SETTLE_MS after enabling.
  #define BAT_DIV_RATIO   0.5f
  #define BAT_SETTLE_MS   60
  // WiFi TX power cap in 0.25 dBm units (esp_wifi_set_max_tx_power scale).
  // HWTests 2026-09-28: joins only at 2 and 5 dBm (RSSI -49); 8/11/14 dBm
  // never associate (AUTH_EXPIRE); 17/20 dBm panic the chip (interrupt WDT).
  // 2 dBm = 8. Applies to AP and client mode. See the KB note
  // firmware/knowledge/ewatch-wifi-tx-power-by-board.md.
  #define WIFI_TX_QDBM    8
#elif EWATCH_HW_REV == 20
  #define EWATCH_HW_NAME "V2.0"
  #define BAT_DIV_RATIO   0.769f   // 30k / 100k divider (bodge to GPIO5)
  #define BAT_SETTLE_MS   60
  // Opposite of V2.1: joins only at 17 and 20 dBm (RSSI -81/-86); 2-14 dBm
  // never associate. Set 20 dBm explicitly rather than rely on the default.
  #define WIFI_TX_QDBM    80
#else
  #error "Unknown EWATCH_HW_REV (use 20 or 21)"
#endif
