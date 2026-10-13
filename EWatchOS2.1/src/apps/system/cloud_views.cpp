// eWatch Cloud screens. See cloud_views.h; the service is core/cloud_msg.cpp.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "cloud_views.h"
#include "cloud_msg.h"
#include "display.h"
#include "haptic.h"
#include "ewlog.h"
#include "power_mgr.h"
#include "ui_style.h"
#include "ui_icons.h"
#include "FreeSans12pt7b.h"

using namespace aura;


InboxView         vInbox;
InboxReadView     vInboxRead;
ContactPickerView vContactPicker;
CloudSettingsView vCloudSettings;
CloudNotifyView   vCloudNotify;

// ---------------------------------------------------------------- helpers
static bool inR(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static Arduino_GFX *beginFrame() {
  Arduino_Canvas *cv = frameCanvas();
  Arduino_GFX *d = cv ? (Arduino_GFX *)cv : gfx;
  d->fillScreen(kBg);
  return d;
}
static void endFrame() {
  Arduino_Canvas *cv = frameCanvas();
  if (cv) cv->flush();
}

// Word-wrap `s` into lines of at most `maxW` px with font f; calls emit(line,
// len, index) for each. Returns the number of lines.
template <typename F>
static int wrapText(Arduino_GFX *d, const GFXfont *f, const char *s, int maxW, F emit) {
  d->setFont(f);
  d->setTextSize(1);
  int lines = 0;
  char buf[64];
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    int best = 0, n = 0;
    for (;;) {
      if (s[n] == 0 || s[n] == '\n' || s[n] == ' ' || n >= (int)sizeof buf - 1) {
        memcpy(buf, s, n); buf[n] = 0;
        int16_t bx, by; uint16_t bw, bh;
        d->getTextBounds(buf, 0, 20, &bx, &by, &bw, &bh);
        if ((int)bw > maxW && best > 0) break;
        best = n;
        if (s[n] == 0 || s[n] == '\n' || n >= (int)sizeof buf - 1) break;
      }
      n++;
    }
    if (best == 0) {                       // one long word: hard-cut it
      best = 1;
      while (s[best] && s[best] != '\n') {
        memcpy(buf, s, best + 1); buf[best + 1] = 0;
        int16_t bx, by; uint16_t bw, bh;
        d->getTextBounds(buf, 0, 20, &bx, &by, &bw, &bh);
        if ((int)bw > maxW) break;
        best++;
      }
    }
    emit(s, best, lines++);
    s += best;
    if (*s == '\n') s++;
  }
  return lines;
}

static void fmtAge(char *out, size_t cap, uint32_t ts) {
  time_t now = time(nullptr);
  if (ts == 0 || now < 1700000000) { out[0] = 0; return; }
  long d = (long)now - (long)ts;
  if (d < 60) snprintf(out, cap, "now");
  else if (d < 3600) snprintf(out, cap, "%ldm", d / 60);
  else if (d < 86400) snprintf(out, cap, "%ldh", d / 3600);
  else snprintf(out, cap, "%ldd", d / 86400);
}

// ---------------------------------------------------------------- settings
static bool wifiReady() {
  ModelLock lk;
  return model.wifiEnabled && model.wifiMode == WifiMode::Client;
}

static const int16_t BTN_Y = 214, BTN_H = 44;

void CloudSettingsView::onEnter() { sig = 0; unlinkArmMs = 0; }

uint16_t CloudSettingsView::desiredFrameMs() const {
  return cloudLinkState() == CloudLink::Pairing ? 250 : 0;
}

void CloudSettingsView::render() {
  char code[16] = "";
  uint32_t left = 0;
  bool haveCode = cloudPairingCode(code, sizeof code, &left);
  uint32_t s = (uint32_t)cloudLinkState() * 1000003u ^ (haveCode ? (uint32_t)code[0] * 31u + code[5] : 0) ^ (left << 8) ^
               (uint32_t)strlen(cloudPairingError()) * 7u ^ (wifiReady() ? 0x55u : 0) ^ (unlinkArmMs ? 0x77u : 0);
  if (unlinkArmMs && millis() - unlinkArmMs > 3000) { unlinkArmMs = 0; s ^= 1; }
  if (s == sig) return;
  sig = s;
  draw();
}

void CloudSettingsView::draw() {
  Arduino_GFX *d = beginFrame();
  auraFontCentered(d, &FreeSans12pt7b, W / 2, 34, "eWatch Cloud", kText);
  CloudLink link = cloudLinkState();
  const char *err = cloudPairingError();
  if (link == CloudLink::Linked) {
    drawIcon(d, Icon::Cloud, W / 2, 84, 40, kBlue, kBg);
    auraTextCentered(d, W / 2, 112, "Linked to", 1, kTextDim);
    auraFontCentered(d, &FreeSans12pt7b, W / 2, 144, cloudAccountName(), kText);
    char line[40];
    uint16_t u = cloudUnreadCount();
    snprintf(line, sizeof line, u ? "%u unread" : "No unread messages", u);
    auraTextCentered(d, W / 2, 164, line, 1, kTextDim);
    auraPill(d, 20, BTN_Y - 40, 200, 32, kBlue, kText, "Check for messages", 1);
    auraPill(d, 20, BTN_Y + 4, 200, 32, unlinkArmMs ? kRed : kCard, unlinkArmMs ? kText : kRed,
             unlinkArmMs ? "Tap again to unlink" : "Unlink this watch", 1);
  } else if (link == CloudLink::Pairing) {
    char code[16] = "";
    uint32_t left = 0;
    if (cloudPairingCode(code, sizeof code, &left)) {
      auraTextCentered(d, W / 2, 62, "On ewatch.cloud open", 1, kTextDim);
      auraTextCentered(d, W / 2, 76, "Devices > Link a watch", 1, kText);
      auraTextCentered(d, W / 2, 90, "and enter this code:", 1, kTextDim);
      auraCard(d, 12, 104, W - 24, 64, 16, kCard);
      // Pixel font at size 3: monospace (easy to read off and type), 162 px
      // wide (FreeSansBold24 ran past the card at 9 characters).
      auraTextCentered(d, W / 2, 125, code, 3, kText);
      char t[24];
      snprintf(t, sizeof t, "%lu:%02lu left", (unsigned long)(left / 60), (unsigned long)(left % 60));
      auraTextCentered(d, W / 2, 182, t, 1, kTextDim);
    } else {
      auraTextCentered(d, W / 2, 120, "Getting a code...", 2, kTextDim);
    }
    auraPill(d, 50, BTN_Y + 4, 140, 32, kCard, kText, "Cancel", 1);
  } else {
    drawIcon(d, Icon::Cloud, W / 2, 82, 44, kTextDim, kBg);
    auraTextCentered(d, W / 2, 114, "Send and receive messages", 1, kText);
    auraTextCentered(d, W / 2, 128, "and drawings with your", 1, kText);
    auraTextCentered(d, W / 2, 142, "eWatch Cloud contacts.", 1, kText);
    if (!wifiReady()) auraTextCentered(d, W / 2, 166, "Turn on WiFi first", 1, kOrange);
    if (err[0]) auraTextCentered(d, W / 2, 184, err, 1, kRed);
    auraPill(d, 20, BTN_Y, 200, 40, kBlue, kText, "Link this watch", 2);
  }
  endFrame();
}

void CloudSettingsView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchToAnimated(Screen::Settings, Trans::PopRight); return; }
  if (e.type != EventType::Touch) return;
  CloudLink link = cloudLinkState();
  if (link == CloudLink::Linked) {
    if (inR(e.x, e.y, 20, BTN_Y - 44, 200, 40)) { cloudCheckNow(); hapticBuzz(70, 12); sig = 0; }
    else if (inR(e.x, e.y, 20, BTN_Y, 200, 40)) {
      if (unlinkArmMs) { cloudUnlink(); unlinkArmMs = 0; hapticBuzz(140, 40); }
      else { unlinkArmMs = millis(); hapticBuzz(70, 12); }
      sig = 0;
    }
  } else if (link == CloudLink::Pairing) {
    if (inR(e.x, e.y, 40, BTN_Y, 160, 44)) { cloudCancelPairing(); hapticBuzz(70, 12); sig = 0; }
  } else if (inR(e.x, e.y, 20, BTN_Y - 4, 200, 48)) {
    cloudStartPairing();
    hapticBuzz(90, 18);
    sig = 0;
  }
}

// ---------------------------------------------------------------- notify card
static Screen sNotifyReturn = Screen::Watch;

void CloudNotifyView::onEnter() { cloudNotice(&n); drawn = false; }

void CloudNotifyView::render() {
  if (drawn) return;
  drawn = true;
  Arduino_GFX *d = beginFrame();
  auraCard(d, 10, 18, W - 20, 186, 22, kCard);
  drawIcon(d, n.image ? Icon::Photo : Icon::Mail, 40, 48, 30, kBlue, kCard);
  d->setFont(nullptr);
  d->setTextSize(1);
  d->setTextColor(kTextDim);
  d->setCursor(64, 38);
  d->print("New message");
  d->setFont(&FreeSans12pt7b);
  d->setTextColor(kText);
  d->setCursor(64, 64);
  d->print(n.from);
  const char *pv = n.preview[0] ? n.preview : (n.image ? "Photo" : "");
  int y0 = 98;
  wrapText(d, &FreeSans12pt7b, pv, W - 52, [&](const char *s, int len, int i) {
    if (i >= 4) return;
    char b[64]; memcpy(b, s, len); b[len] = 0;
    d->setCursor(26, y0 + i * 26);
    d->setTextColor(kText);
    d->print(b);
  });
  d->setFont(nullptr);
  auraPill(d, 14, 220, 100, 42, kCard, kText, "Later", 2);
  auraPill(d, 126, 220, 100, 42, kBlue, kText, "View", 2);
  endFrame();
}

void CloudNotifyView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchTo(sNotifyReturn); return; }
  if (e.type != EventType::Touch || e.y < 210) return;
  if (e.x >= W / 2) {
    cloudOpenWhenLoaded(n.id);
    cloudRefreshInbox();
    switchTo(Screen::Inbox);
  } else {
    switchTo(sNotifyReturn);
  }
}

// ---------------------------------------------------------------- inbox
static AuraEntry sInboxRows[kCloudInboxMax];
static char sInboxLabels[kCloudInboxMax][kCloudNameMax + 8];
static char sInboxSubs[kCloudInboxMax][48];
static CloudMsg sInboxCopy[kCloudInboxMax];
static char sOpenId[kCloudIdMax] = "";      // InboxRead shows this one

// Copy src into out, cut to maxChars with "..." if longer.
static void fitText(char *out, size_t cap, const char *src, size_t maxChars) {
  size_t n = strlen(src);
  if (maxChars >= cap) maxChars = cap - 1;
  if (n <= maxChars) { memcpy(out, src, n + 1); return; }
  size_t k = maxChars - 3;
  while (k > 0 && src[k - 1] == ' ') k--;
  memcpy(out, src, k);
  memcpy(out + k, "...", 4);
}

InboxView::InboxView() : AuraListView(sInboxRows, 0, "Messages", Screen::AppList) {}

void InboxView::rebuild() {
  size_t n = cloudInbox(sInboxCopy, kCloudInboxMax);
  for (size_t i = 0; i < n; i++) {
    const CloudMsg &m = sInboxCopy[i];
    char age[8];
    fmtAge(age, sizeof age, m.ts);
    snprintf(sInboxLabels[i], sizeof sInboxLabels[i], "%s%s%s", m.from, age[0] ? "  " : "", age);
    // The list prints the sub line unclipped: cut it to the row (size 1 =
    // 6 px a character; 26 fit between the icon and the chevron).
    fitText(sInboxSubs[i], sizeof sInboxSubs[i], m.preview[0] ? m.preview : (m.image ? "Photo" : ""), 26);
    sInboxRows[i] = { sInboxLabels[i], m.image ? Icon::Photo : Icon::Mail,
                      m.read ? kLine : kBlue, Screen::InboxRead, nullptr, sInboxSubs[i] };
  }
  setEntries(sInboxRows, (int)n);
  rev = cloudInboxRevision();
}

void InboxView::onEnter() {
  rebuild();
  AuraListView::onEnter();
  cloudRefreshInbox();
}

void InboxView::render() {
  if (rev != cloudInboxRevision()) rebuild();
  char id[kCloudIdMax];
  if (cloudTakeOpenId(id, sizeof id)) {
    strncpy(sOpenId, id, sizeof sOpenId - 1);
    switchTo(Screen::InboxRead);
    return;
  }
  AuraListView::render();
}

void InboxView::activate(int row) {
  if (row < 0 || row >= rowCount()) return;
  strncpy(sOpenId, sInboxCopy[row].id, sizeof sOpenId - 1);
  switchToAnimated(Screen::InboxRead, Trans::PushLeft);
}

void InboxView::drawOverlay(Arduino_Canvas *cv) {
  if (rowCount() > 0) return;
  const char *msg = cloudLinkState() != CloudLink::Linked ? "Link this watch in"
                  : cloudInboxLoading() ? "Checking..." : "No messages yet";
  auraTextCentered(cv, W / 2, 130, msg, 2, kTextDim);
  if (cloudLinkState() != CloudLink::Linked)
    auraTextCentered(cv, W / 2, 154, "Settings > eWatch Cloud", 1, kTextDim);
}

// ---------------------------------------------------------------- read
static const int16_t READ_TOP = 56, READ_LINE = 26, READ_BOT = 220;

void InboxReadView::onEnter() {
  if (!text) text = (char *)heap_caps_malloc(kCloudTextMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  CloudMsg list[kCloudInboxMax];
  size_t n = cloudInbox(list, kCloudInboxMax);
  m = CloudMsg{};
  for (size_t i = 0; i < n; i++) if (strcmp(list[i].id, sOpenId) == 0) m = list[i];
  if (text) { text[0] = 0; cloudInboxText(m.id, text, kCloudTextMax + 1); }
  if (m.id[0]) cloudMarkRead(m.id);
  if (m.image) cloudLoadImage(m.id);
  scroll = 0; maxScroll = 0; dragging = false; dirty = true;
  lastImg = CloudImgState::Idle;
}

void InboxReadView::onExit() {}

uint16_t InboxReadView::desiredFrameMs() const {
  return (dragging || (m.image && lastImg == CloudImgState::Loading)) ? 33 : 0;
}

void InboxReadView::render() {
  if (m.image) {
    CloudImgState s = cloudImage(nullptr, nullptr, nullptr);
    if (s != lastImg) { lastImg = s; dirty = true; }
  }
  if (!dirty) return;
  dirty = false;
  draw();
}

void InboxReadView::draw() {
  Arduino_GFX *d = beginFrame();
  Arduino_Canvas *cv = frameCanvas();
  if (m.image && lastImg == CloudImgState::Ready && cv) {
    const uint16_t *px; uint16_t w, h;
    cloudImage(&px, &w, &h);
    int16_t x0 = (W - w) / 2, y0 = (H - h) / 2;
    uint16_t *fb = cv->getFramebuffer();
    for (int y = 0; y < h; y++) {
      int dy = y0 + y;
      if (dy < 0 || dy >= H) continue;
      for (int x = 0; x < w; x++) {
        int dx = x0 + x;
        if (dx >= 0 && dx < W) fb[dy * W + dx] = px[(size_t)y * w + x];
      }
    }
  }
  // header: sender and age
  char age[8];
  fmtAge(age, sizeof age, m.ts);
  if (!m.image || lastImg != CloudImgState::Ready) {
    d->setFont(&FreeSans12pt7b);
    d->setTextSize(1);                   // a size-2 status line may have left it at 2
    d->setTextColor(kText);
    d->setCursor(14, 32);
    d->print(m.id[0] ? m.from : "Message gone");
    d->setFont(nullptr);
    auraTextCentered(d, W - 26, 22, age, 1, kTextDim);
    d->drawFastHLine(14, 44, W - 28, kLine);
  } else {
    auraPill(d, 8, 8, auraTextWidth(m.from, 1) + 20, 22, kCard, kText, m.from, 1);
  }
  if (m.image) {
    if (lastImg == CloudImgState::Loading || lastImg == CloudImgState::Idle)
      auraTextCentered(d, W / 2, 130, "Loading photo...", 2, kTextDim);
    else if (lastImg == CloudImgState::Failed)
      auraTextCentered(d, W / 2, 130, "Couldn't load photo", 2, kRed);
  }
  if (text && text[0] && !(m.image && lastImg == CloudImgState::Ready)) {
    int lines = wrapText(d, &FreeSans12pt7b, text, W - 30, [&](const char *s, int len, int i) {
      int y = READ_TOP + 18 + i * READ_LINE - scroll;
      if (y < READ_TOP + 2 || y > READ_BOT) return;
      char b[64]; memcpy(b, s, len); b[len] = 0;
      d->setTextColor(kText);
      d->setCursor(15, y);
      d->print(b);
    });
    d->setFont(nullptr);
    int contentH = lines * READ_LINE + 10;
    maxScroll = contentH > READ_BOT - READ_TOP ? contentH - (READ_BOT - READ_TOP) : 0;
  }
  if (m.id[0]) auraPill(d, 60, 230, 120, 38, kBlue, kText, "Reply", 2);
  endFrame();
}

void InboxReadView::onEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { switchToAnimated(Screen::Inbox, Trans::PopRight); return; }
  if (e.type == EventType::Touch) {
    dragging = true; moved = false; dragY = e.y; dragStart = scroll;
  } else if (e.type == EventType::TouchHold && dragging) {
    int dy = (int)dragY - (int)e.y;
    if (abs(dy) > 6) moved = true;
    int s = dragStart + dy;
    if (s < 0) s = 0;
    if (s > maxScroll) s = maxScroll;
    if (s != scroll) { scroll = s; dirty = true; }
  } else if (e.type == EventType::TouchUp) {
    // Only a press that started here: the notification card switches on
    // touch-down, and that same tap's release must not hit Reply.
    bool mine = dragging;
    dragging = false;
    if (mine && !moved && m.id[0] && inR(e.x, e.y, 50, 222, 140, 54)) {
#if EW_APP_WRITE
      CloudContact c = {};
      strncpy(c.id, m.fromId, sizeof c.id - 1);
      strncpy(c.name, m.from, sizeof c.name - 1);
      cloudSetReplyTo(&c);
      hapticBuzz(70, 12);
      switchToAnimated(Screen::Write, Trans::PushLeft);
#endif
    }
  }
}

// ---------------------------------------------------------------- contact picker
static AuraEntry sPickRows[kCloudMaxContacts];
static CloudContact sPick[kCloudMaxContacts];
static CloudPickFn sPickFn = nullptr;
static void *sPickCtx = nullptr;
static Screen sPickReturn = Screen::AppList;

void cloudPickContact(CloudPickFn onPick, void *ctx, uint8_t returnTo) {
  sPickFn = onPick; sPickCtx = ctx; sPickReturn = (Screen)returnTo;
  switchToAnimated(Screen::ContactPicker, Trans::SheetUp);
}

ContactPickerView::ContactPickerView() : AuraListView(sPickRows, 0, "Send to", Screen::AppList) {}

void ContactPickerView::rebuild() {
  n = cloudContacts(sPick, kCloudMaxContacts);
  for (size_t i = 0; i < n; i++)
    sPickRows[i] = { sPick[i].name, sPick[i].self ? Icon::Cloud : Icon::Mail,
                     sPick[i].self ? kTeal : kBlue, Screen::ContactPicker, nullptr, nullptr };
  setEntries(sPickRows, (int)n);
}

void ContactPickerView::onEnter() {
  rebuild();
  AuraListView::onEnter();
  cloudRefreshContacts();
  loadingShown = true;
}

void ContactPickerView::render() {
  bool loading = cloudContactsLoading();
  if (loadingShown && !loading) { loadingShown = false; rebuild(); }
  AuraListView::render();
}

static void pickDone(const CloudContact *c) {
  CloudPickFn fn = sPickFn;
  void *ctx = sPickCtx;
  sPickFn = nullptr;
  Screen back = sPickReturn;
  if (fn) fn(c, ctx);
  switchToAnimated(back, Trans::SheetDown);
}

void ContactPickerView::activate(int row) {
  if (row < 0 || (size_t)row >= n) return;
  hapticBuzz(70, 12);
  pickDone(&sPick[row]);
}

bool ContactPickerView::interceptEvent(const Event &e) {
  if (e.type == EventType::ButtonShort) { pickDone(nullptr); return true; }
  return false;
}

void ContactPickerView::drawOverlay(Arduino_Canvas *cv) {
  if (n > 0) return;
  if (cloudLinkState() != CloudLink::Linked) {
    auraTextCentered(cv, W / 2, 124, "Not linked", 2, kTextDim);
    auraTextCentered(cv, W / 2, 148, "Settings > eWatch Cloud", 1, kTextDim);
  } else if (cloudContactsLoading()) {
    auraTextCentered(cv, W / 2, 130, "Loading...", 2, kTextDim);
  } else {
    auraTextCentered(cv, W / 2, 120, "No contacts yet", 2, kTextDim);
    auraTextCentered(cv, W / 2, 146, "Add people on ewatch.cloud", 1, kTextDim);
  }
}

// ---------------------------------------------------------------- notify + toast
static char     sToastText[48] = "";
static uint16_t sToastColor = kCard;
static uint32_t sToastUntil = 0;
static bool     sToastShown = false;
static CloudSendState sLastSend = CloudSendState::Idle;
static uint32_t sLastSendMs = 0;

static void toast(const char *t, uint16_t color, uint32_t ms) {
  strncpy(sToastText, t, sizeof sToastText - 1);
  sToastColor = color;
  sToastUntil = millis() + ms;
}

void cloudViewsPollNotify() {
  if (!cloudTakeNotify()) return;
  Screen s;
  { ModelLock lk; s = model.screen; }
  bool busy = s == Screen::Write || s == Screen::Draw || s == Screen::InboxRead ||
              s == Screen::ContactPicker || s == Screen::CloudSettings || s == Screen::CloudNotify ||
              s == Screen::Inbox;
  if (busy) {
    CloudNotice n;
    cloudNotice(&n);
    char t[48];
    snprintf(t, sizeof t, "Message from %s", n.from);
    toast(t, kBlue, 3000);
    if (s == Screen::Inbox) cloudRefreshInbox();
    return;
  }
  sNotifyReturn = s;
  powerOnActivity();
  switchTo(Screen::CloudNotify);
}

// Drawn straight to the panel over whatever the view drew (redrawn each
// frame while showing); when it ends, the frame canvas copy of that strip is
// pushed back.
void cloudToastPaint() {
  char reason[40];
  CloudSendState st = cloudSendState(reason, sizeof reason);
  uint32_t ms = cloudSendStateMs();
  if (st != sLastSend || ms != sLastSendMs) {
    sLastSend = st; sLastSendMs = ms;
    if (st == CloudSendState::Sending) toast("Sending...", kCard, 30000);
    else if (st == CloudSendState::Sent) toast("Sent", kGreen, 1500);
    else if (st == CloudSendState::Failed) {
      char t[64];
      snprintf(t, sizeof t, "Couldn't send: %s", reason);
      if (auraTextWidth(t, 1) + 28 > W - 16) snprintf(t, sizeof t, "Not sent: %s", reason);
      if (auraTextWidth(t, 1) + 28 > W - 16) snprintf(t, sizeof t, "%s", reason);
      toast(t, kRed, 3000);
    }
  }
  if (!gfx) return;
  bool on = sToastText[0] && (int32_t)(sToastUntil - millis()) > 0;
  if (on) {
    int16_t tw = auraTextWidth(sToastText, 1) + 28;
    if (tw > W - 16) tw = W - 16;
    auraPill(gfx, (W - tw) / 2, 6, tw, 26, sToastColor, kText, sToastText, 1);
    sToastShown = true;
  } else if (sToastShown) {
    sToastShown = false;
    sToastText[0] = 0;
    Arduino_Canvas *cv = frameCanvas();
    if (cv) {
      uint16_t *fb = cv->getFramebuffer();
      for (int y = 4; y < 34; y++) gfx->draw16bitRGBBitmap(0, y, fb + y * W, W, 1);
    }
  }
}
