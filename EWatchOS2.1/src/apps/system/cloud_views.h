// eWatch Cloud screens: CloudSettings (link this watch), CloudNotify (new
// message card), Inbox, InboxRead, ContactPicker. The service is core/cloud_msg.
#pragma once
#include "view.h"
#include "launcher.h"
#include "cloud_msg.h"

class CloudSettingsView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;
private:
  uint32_t sig = 0;               // state signature last drawn
  uint32_t unlinkArmMs = 0;       // first tap on Unlink arms it for 3 s
  void draw();
};

class CloudNotifyView : public View {
public:
  void onEnter() override;
  void render() override;
  void onEvent(const Event &e) override;
private:
  CloudNotice n = {};
  bool drawn = false;
};

class InboxView : public AuraListView {
public:
  InboxView();
  void onEnter() override;
  void render() override;
protected:
  void activate(int row) override;
  void drawOverlay(Arduino_Canvas *cv) override;
private:
  uint32_t rev = 0;
  void rebuild();
};

class InboxReadView : public View {
public:
  void onEnter() override;
  void onExit() override;
  void render() override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override;
private:
  CloudMsg m = {};
  char    *text = nullptr;        // PSRAM, kCloudTextMax + 1
  int      scroll = 0, maxScroll = 0;
  int16_t  dragY = 0;
  int      dragStart = 0;
  bool     dragging = false, moved = false, dirty = true;
  CloudImgState lastImg = CloudImgState::Idle;
  void draw();
};

class ContactPickerView : public AuraListView {
public:
  ContactPickerView();
  void onEnter() override;
  void render() override;
protected:
  void activate(int row) override;
  void drawOverlay(Arduino_Canvas *cv) override;
  bool interceptEvent(const Event &e) override;
private:
  size_t n = 0;
  bool   loadingShown = false;
  void rebuild();
};

// Render-loop hooks (controller.cpp).
void cloudViewsPollNotify();      // a new message: card, or a toast inside a busy app
void cloudToastPaint();           // send status / message toast over the frame

extern InboxView         vInbox;
extern InboxReadView     vInboxRead;
extern ContactPickerView vContactPicker;
extern CloudSettingsView vCloudSettings;
extern CloudNotifyView   vCloudNotify;
