// eWatch Cloud messages: the watch side.
//
// The contract (API, data formats, behaviour) is MESSAGES_DESIGN.md in the
// EwatchCloud repo (branch feat/messages). Summary:
//
//   * Linking: Settings -> eWatch Cloud shows an 8-char pairing code; the user
//     types it on ewatch.cloud -> Devices. The watch then holds a device token
//     ("ewd_...") in NVS namespace "ewcloud".
//   * Every time WiFi connects (and every 60 s while it stays up) the service
//     asks the cloud for unread messages; a new one raises a "New message"
//     card with View / Later.
//   * Apps send through cloudSendText / cloudSendImage after the user picks a
//     contact with cloudPickContact. Sending runs on a background task; the
//     result shows as a toast. Nothing here blocks the render task.
//
// Thread-safety: every function may be called from any task. Strings are
// copied before the call returns.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- link state --------------------------------------------------------
enum class CloudLink : uint8_t {
  Unlinked,   // no device token
  Pairing,    // showing a code, polling the cloud
  Linked,     // token saved
};

void        cloudInit();                 // boot: load token from NVS, register WiFi hook
CloudLink   cloudLinkState();
const char *cloudAccountName();          // "Ewan" once linked ("" otherwise); static buffer
void        cloudStartPairing();         // opens a WiFi window, asks for a code
void        cloudCancelPairing();
// The current code ("K7QX-4M2P") and seconds left; false when there is none yet.
bool        cloudPairingCode(char *out, size_t cap, uint32_t *secsLeft);
const char *cloudPairingError();         // "" or a short reason ("No WiFi", "Expired")
void        cloudUnlink();               // forget the token on this watch

// ---- contacts ----------------------------------------------------------
constexpr size_t kCloudMaxContacts = 32;
constexpr size_t kCloudNameMax     = 24;   // display name, printable ASCII
constexpr size_t kCloudIdMax       = 32;   // cuid

struct CloudContact {
  char id[kCloudIdMax];      // the contact's user id (used as "to")
  char name[kCloudNameMax];
  bool self;                 // "Me" (send to your own account / computer)
};

// Copies the cached list (refreshed on each WiFi connect). Returns the count.
size_t cloudContacts(CloudContact *out, size_t max);
void   cloudRefreshContacts();          // queue a refresh (opens a WiFi window)
bool   cloudContactsLoading();

// Opens the contact picker screen. When the user picks someone, `onPick` runs
// on the render task with that contact; Back runs it with nullptr. After the
// callback the picker returns to `returnTo` (a Screen id cast to uint8_t).
using CloudPickFn = void (*)(const CloudContact *picked, void *ctx);
void cloudPickContact(CloudPickFn onPick, void *ctx, uint8_t returnTo);

// ---- sending -------------------------------------------------------------
// Queue a message. Returns false only if the outbox is full (2 slots) or the
// watch isn't linked. The service turns WiFi on as needed and shows
// "Sending..." -> "Sent" / "Couldn't send: <reason>".
bool cloudSendText(const char *toUserId, const char *text);

// `pixels` is a w*h RGB565 buffer (native order, as in Arduino_Canvas). The
// service encodes it as EWI1 runs (design §5) on its own task, so the caller
// may free/reuse the buffer as soon as this returns (it is copied to PSRAM).
bool cloudSendImage(const char *toUserId, const uint16_t *pixels,
                    uint16_t w, uint16_t h, const char *caption /* may be nullptr */);

enum class CloudSendState : uint8_t { Idle, Sending, Sent, Failed };
CloudSendState cloudSendState(char *reason, size_t cap);   // last send

// ---- inbox ---------------------------------------------------------------
uint16_t cloudUnreadCount();            // last value from the server (0 if unknown)

// Render-task hooks (called from controller.cpp):
//   cloudTakeNotify(): true once per new message -> show the "New message" card
//   cloudToastPaint(): draws the send toast over the current frame, if any
bool cloudTakeNotify();
void cloudToastPaint();
