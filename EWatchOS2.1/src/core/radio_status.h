// Radio link quality for status icons (watch face). Render task only.
//   Off    - the radio is switched off: show no icon
//   None   - on, but no link (grey)
//   Poor / Medium / Good - linked; red / orange / green
// WiFi is duty-cycled, so between sync windows its quality is the last
// session's RSSI; BLE is the connected peer's live RSSI (polled every 4 s).
#pragma once
#include <stdint.h>

enum class LinkQ : uint8_t { Off, None, Poor, Medium, Good };

LinkQ       wifiLinkQ();
LinkQ       bleLinkQ();
uint16_t    linkQColor(LinkQ q);   // RGB565
const char *linkQName(LinkQ q);
