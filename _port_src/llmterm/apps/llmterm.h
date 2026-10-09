// src/apps/llmterm.h
//
// llmterm — the watch as a terminal for a Mac-hosted local LLM.
//
// The watch does NOT run a model (WiFi is gated off entirely). A Python bridge
// on the Mac (pc/llmterm_bridge.py) talks to a local model — Ollama by default
// (http://localhost:11434/api/generate, streaming), with a llama.cpp server
// fallback — and relays tokens down the USB-CDC serial link.
//
// Serial protocol (newline-terminated ASCII, 115200; USB-CDC ignores baud):
//   Watch -> PC :  PROMPT <text>\n          one line, the chosen prompt
//   PC -> Watch :  >>TOK <chunk>\n          zero or more; a streamed token
//                  >>END\n                  stream finished
//                  >>ERR <message>\n         error (shown on the watch)
//   PC may emit other ">>"-prefixed lines (e.g. >>READY); the watch ignores
//   any line it doesn't recognise, so boot-log noise is harmless too.
//
//   Token chunks are escaped by the bridge so one token == one line:
//       '\'  -> "\\"   and   newline -> "\n"   (two chars each).
//   The watch un-escapes on receipt, so "\n" inside a token renders as a hard
//   line break in the wrapped output.
//
// MVP input is a short menu of canned prompts (on-watch free text entry is
// fiddly). Tap one to send it; tokens stream in, word-wrapped, with a blinking
// block caret, scrolling as the view fills. Back (SW2 / right-swipe) returns to
// the menu, or from the menu to the launcher.
#pragma once
#include "view.h"

class LlmTermView : public View {
public:
  void onEnter() override;
  void onExit()  override;
  void render()  override;
  void onEvent(const Event &e) override;

private:
  enum class Mode : uint8_t { Menu, Chat };
  Mode mode = Mode::Menu;

  // ---- menu ----
  int  menuSel = -1;           // last highlighted row (for cheap repaint)
  bool menuDirty = true;       // force a full menu repaint

  // ---- chat / streaming ----
  static constexpr uint32_t kRespCap = 4000;   // response byte budget
  char     resp[kRespCap + 1];
  uint32_t respLen   = 0;
  bool     streaming = false;  // request in flight (awaiting tokens / >>END)
  bool     sawAnyTok = false;  // got >=1 token for the current prompt
  bool     errState  = false;  // last stream ended on >>ERR
  const char *promptText = nullptr;  // prompt currently being answered

  // Serial line assembler.
  static constexpr uint16_t kLineCap = 512;
  char     line[kLineCap];
  uint16_t lineLen = 0;

  void pumpSerial();                 // drain Serial -> process complete lines
  void handleLine(const char *s);    // dispatch one protocol line
  void appendTokenEscaped(const char *chunk);  // un-escape + append to resp

  void sendPrompt(const char *text);
  void resetChat();

  void drawMenu();
  void drawChat();                   // full-screen (canvas if available)
};
