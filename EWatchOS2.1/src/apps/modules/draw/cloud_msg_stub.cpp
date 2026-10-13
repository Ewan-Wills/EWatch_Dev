// =====================================================================
// TEST STUB - DELETE AT MERGE with the real eWatch Cloud service.
//
// Weak stand-ins for the two cloud_msg.h calls the Draw module makes, so
// EW_APPS=draw builds and runs (watch + ewsim_host) before the real
// implementation (core/cloud_*.cpp, written in the feat/messages worktree)
// lands. The real service's strong definitions override these at link time;
// defining EW_CLOUD_MSG_IMPL compiles this file out entirely.
//
//   cloudPickContact: no picker screen; calls onPick at once with a fake
//                     contact (id "stub-contact").
//   cloudSendImage:   sends nothing; logs
//                     "DRAW: send to=<id> bytes=<n> w=<w> h=<h> sum=<fnv1a>"
//                     (sum = drawChecksum of the pixels it was handed).
// =====================================================================
#if !defined(EW_CLOUD_MSG_IMPL)
#include <Arduino.h>
#include "cloud_msg.h"
#include "ewlog.h"
#include "draw.h"

__attribute__((weak)) void cloudPickContact(CloudPickFn onPick, void *ctx, uint8_t returnTo) {
  static const CloudContact kFake = { "stub-contact", "Stub", false };
  EWLOGI("DRAW", "stub picker returnTo=%u picked=%s", (unsigned)returnTo, kFake.id);
  if (onPick) onPick(&kFake, ctx);
}

__attribute__((weak)) bool cloudSendImage(const char *toUserId, const uint16_t *pixels,
                                          uint16_t w, uint16_t h, const char *caption) {
  (void)caption;
  uint32_t n = (uint32_t)w * h;
  EWLOGI("DRAW", "send to=%s bytes=%lu w=%u h=%u sum=%08lx", toUserId ? toUserId : "?",
         (unsigned long)(n * 2), (unsigned)w, (unsigned)h,
         (unsigned long)(pixels ? drawChecksum(pixels, n) : 0));
  return pixels && toUserId;
}
#endif  // !EW_CLOUD_MSG_IMPL
