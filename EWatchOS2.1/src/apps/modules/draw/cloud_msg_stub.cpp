// STUB: deleted when merged with core/cloud_msg.cpp
//
// Stand-in for every cloud_msg.h function so EW_APPS=draw builds and runs on
// the watch and in ewsim_host before the real eWatch Cloud service (written
// in the feat/messages worktree as src/core/cloud_msg.cpp) lands. All
// definitions are weak, so the real service's strong ones win at link time
// even if this file is forgotten; defining EW_CLOUD_MSG_IMPL compiles it out.
//
//   * Linked as "Ewan", two contacts: "Me" (self) and "Ewan".
//   * cloudPickContact: no picker screen; logs and calls onPick at once with
//     the first non-self contact ("Ewan").
//   * cloudSendImage: sends nothing; logs
//       "DRAW: stub send to=<id> w=<w> h=<h> bytes=<n> sum=<fnv1a> caption=<c>"
//     (sum = drawChecksum of the pixels it was handed) and reports Sent.
#if !defined(EW_CLOUD_MSG_IMPL)
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "cloud_msg.h"
#include "ewlog.h"
#include "draw.h"

#define STUB __attribute__((weak))

static const CloudContact kStubContacts[] = {
  { "stub-me",   "Me",   true  },
  { "stub-ewan", "Ewan", false },
};
static CloudSendState sStubSend = CloudSendState::Idle;

STUB void        cloudInit() {}
STUB CloudLink   cloudLinkState() { return CloudLink::Linked; }
STUB const char *cloudAccountName() { return "Ewan"; }
STUB void        cloudStartPairing() {}
STUB void        cloudCancelPairing() {}
STUB bool        cloudPairingCode(char *out, size_t cap, uint32_t *secsLeft) {
  if (out && cap) out[0] = 0;
  if (secsLeft) *secsLeft = 0;
  return false;
}
STUB const char *cloudPairingError() { return ""; }
STUB void        cloudUnlink() {}

STUB size_t cloudContacts(CloudContact *out, size_t max) {
  size_t n = sizeof kStubContacts / sizeof kStubContacts[0];
  if (n > max) n = max;
  if (out) memcpy(out, kStubContacts, n * sizeof(CloudContact));
  return n;
}
STUB void cloudRefreshContacts() {}
STUB bool cloudContactsLoading() { return false; }

STUB void cloudPickContact(CloudPickFn onPick, void *ctx, uint8_t returnTo) {
  const CloudContact *c = &kStubContacts[1];
  EWLOGI("DRAW", "stub picker returnTo=%u picked=%s (%s)", (unsigned)returnTo, c->id, c->name);
  if (onPick) onPick(c, ctx);
}

STUB bool cloudSendText(const char *toUserId, const char *text) {
  EWLOGI("DRAW", "stub send text to=%s len=%u", toUserId ? toUserId : "?",
         (unsigned)(text ? strlen(text) : 0));
  sStubSend = CloudSendState::Sent;
  return toUserId && text;
}

STUB bool cloudSendImage(const char *toUserId, const uint16_t *pixels,
                         uint16_t w, uint16_t h, const char *caption) {
  uint32_t n = (uint32_t)w * h;
  EWLOGI("DRAW", "stub send to=%s w=%u h=%u bytes=%lu sum=%08lx caption=%s",
         toUserId ? toUserId : "?", (unsigned)w, (unsigned)h, (unsigned long)(n * 2),
         (unsigned long)(pixels ? drawChecksum(pixels, n) : 0), caption ? caption : "-");
  sStubSend = pixels && toUserId ? CloudSendState::Sent : CloudSendState::Failed;
  return pixels && toUserId;
}

STUB CloudSendState cloudSendState(char *reason, size_t cap) {
  if (reason && cap) reason[0] = 0;
  return sStubSend;
}

STUB uint16_t cloudUnreadCount() { return 0; }
STUB bool     cloudTakeNotify() { return false; }
STUB void     cloudToastPaint() {}
#endif  // !EW_CLOUD_MSG_IMPL
