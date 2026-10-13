// ewsim_host stub for src/core/cloud_net.cpp: no radio, so cloudHttp answers
// like a small eWatch Cloud server (MESSAGES_DESIGN.md) held in memory.
// The network is always "up". Pairing links on the second poll. Two contacts,
// two received messages (one text, one photo with no image data, so the reader
// shows its "couldn't load" state). Sends are recorded and acknowledged.
// Console (tests): "cloud fake say <text>" delivers a new text message from Ada
// (the next summary raises the notification card); "cloud fake fail <status>"
// makes the next send fail with that status; "cloud fake 401" answers the next
// request 401 although the token is fine; "cloud fake revoke" revokes it.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "cloud_net.h"
#include "host_sim.h"

namespace {
struct Msg { std::string id, from, fromId, text; bool image, read; uint32_t ts; };
std::vector<Msg> gMsgs = {
  {"m2", "Ada", "u_ada", "", true, false, 1792000100},
  {"m1", "Py B", "u_pyb", "Hello from the simulator! This is a longer message so the reader "
   "has to wrap it over several lines and you can scroll it with a finger.", false, false, 1792000000},
};
int gPolls = 0, gNext = 3, gFailNext = 0;
bool gStray401 = false, gRevoked = false;

int reply(uint8_t *out, size_t cap, size_t *outLen, int st, const std::string &body) {
  if (body.size() >= cap) return kNetTooBig;
  memcpy(out, body.data(), body.size());
  out[body.size()] = 0;
  *outLen = body.size();
  return st;
}

std::string esc(const std::string &s) {
  std::string o;
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o;
}

int unread() { int n = 0; for (auto &m : gMsgs) n += !m.read; return n; }
}  // namespace

bool cloudNetIsRelay(const char *base) { return base && strncmp(base, "usb:", 4) == 0; }
bool cloudNetUp(const char *) { return true; }
bool cloudNetRelayBusy() { return false; }

bool cloudNetRelayLine(const char *line) {
  if (!strncmp(line, "cloud fake say ", 15)) {
    char id[16];
    snprintf(id, sizeof id, "m%d", gNext++);
    gMsgs.insert(gMsgs.begin(), {id, "Ada", "u_ada", line + 15, false, false, 1792000000u + gNext * 100});
    Serial.printf("cloud fake: new message %s\n", id);
    return true;
  }
  if (!strncmp(line, "cloud fake fail ", 16)) { gFailNext = atoi(line + 16); return true; }
  if (!strcmp(line, "cloud fake 401")) { gStray401 = true; return true; }       // next request only
  if (!strcmp(line, "cloud fake revoke")) { gRevoked = true; return true; }     // token revoked
  return false;
}

int cloudHttp(const char *, const char *method, const char *path,
              const char *body, size_t, const char *token,
              uint8_t *out, size_t cap, size_t *outLen) {
  *outLen = 0;
  if (cap) out[0] = 0;
  std::string p = path, m = method;
  ewsimHostNote((std::string("cloud ") + m + " " + p).c_str());
  if (m == "POST" && p == "/devices/pair/start") {
    gPolls = 0;
    return reply(out, cap, outLen, 200,
                 "{\"code\":\"K7QX-4M2P\",\"deviceId\":\"d1\",\"secret\":\"s1\",\"expiresIn\":600,\"interval\":2}");
  }
  if (m == "POST" && p == "/devices/pair/poll") {
    if (++gPolls < 2) return reply(out, cap, outLen, 202, "{\"status\":\"pending\"}");
    return reply(out, cap, outLen, 200,
                 "{\"token\":\"ewd_sim\",\"account\":{\"id\":\"u_me\",\"displayName\":\"Sim Tester\"}}");
  }
  if (!token || gRevoked) return reply(out, cap, outLen, 401, "{\"error\":\"UNAUTHORIZED\"}");
  if (gStray401 && p != "/device/me") { gStray401 = false; return reply(out, cap, outLen, 401, "{\"error\":\"UNAUTHORIZED\"}"); }
  if (p == "/device/me") return reply(out, cap, outLen, 200, "{\"device\":{\"id\":\"d1\"},\"account\":{\"id\":\"u_me\"}}");
  if (p == "/whoami") return reply(out, cap, outLen, 200, "{\"user\":{\"id\":\"u_me\"}}");
  if (p == "/contacts")
    return reply(out, cap, outLen, 200,
                 "{\"contacts\":[{\"userId\":\"u_ada\",\"displayName\":\"Ada\"},"
                 "{\"userId\":\"u_pyb\",\"displayName\":\"Py B\"}]}");
  if (p == "/messages/summary") {
    std::string latest = "null";
    for (auto &x : gMsgs) if (!x.read) {
      latest = "{\"id\":\"" + x.id + "\",\"kind\":\"" + (x.image ? "IMAGE" : "TEXT") +
               "\",\"fromAscii\":\"" + esc(x.from) + "\",\"previewAscii\":\"" +
               esc(x.image ? "Photo" : x.text.substr(0, 80)) + "\"}";
      break;
    }
    return reply(out, cap, outLen, 200, "{\"unread\":" + std::to_string(unread()) + ",\"latest\":" + latest + "}");
  }
  if (p.rfind("/messages?", 0) == 0) {
    std::string s = "{\"messages\":[";
    for (size_t i = 0; i < gMsgs.size() && i < 10; i++) {
      auto &x = gMsgs[i];
      if (i) s += ",";
      s += "{\"id\":\"" + x.id + "\",\"f\":\"" + esc(x.from) + "\",\"fu\":\"" + x.fromId + "\",\"k\":\"" +
           (x.image ? "i" : "t") + "\",\"r\":" + (x.read ? "1" : "0") + ",\"ts\":" + std::to_string(x.ts) +
           (x.image ? ",\"iw\":240,\"ih\":240" : "") + ",\"t\":\"" + esc(x.text) + "\"}";
    }
    return reply(out, cap, outLen, 200, s + "]}");
  }
  if (m == "POST" && p.size() > 15 && p.rfind("/messages/", 0) == 0 && p.compare(p.size() - 5, 5, "/read") == 0) {
    std::string id = p.substr(10, p.size() - 15);
    for (auto &x : gMsgs) if (x.id == id) x.read = true;
    return reply(out, cap, outLen, 200, "{\"ok\":true}");
  }
  if (p.rfind("/messages/", 0) == 0 && p.find("/image") != std::string::npos)
    return reply(out, cap, outLen, 404, "{\"error\":\"NOT_FOUND\"}");
  if (m == "POST" && p == "/messages") {
    if (gFailNext) { int st = gFailNext; gFailNext = 0; return reply(out, cap, outLen, st, "{\"error\":\"SIM\"}"); }
    std::string b = body ? body : "";
    Serial.printf("cloud fake: sent %s\n", b.substr(0, 120).c_str());
    return reply(out, cap, outLen, 201, "{\"id\":\"s1\"}");
  }
  return reply(out, cap, outLen, 404, "{\"error\":\"NOT_FOUND\"}");
}
