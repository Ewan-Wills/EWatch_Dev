// eWatch Cloud: the one function that talks to the network. Kept apart from
// cloud_msg.cpp so the host simulator can replace it (tools/ewsim_host/host/
// stubs/cloud_net.cpp answers like a small fake server).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Errors (negative "status"): no WiFi association, couldn't connect (DNS,
// TCP or TLS: certificate not trusted, handshake timeout), timed out
// mid-response, response bigger than the buffer.
constexpr int kNetNoWifi = -1, kNetConnect = -2, kNetTimeout = -3, kNetTooBig = -4;

// True while the radio is associated (a request can be made).
bool cloudNetUp();

// One request to base + path (base "https://..." = TLS verified against the
// Let's Encrypt roots in cloud_ca.h; "http://..." = plain, for local tests).
// body/bodyLen may be nullptr/0. token: device token for "Authorization:
// Bearer", or nullptr. The response body goes to out (cap bytes, NUL added
// when it fits); *outLen = its length. Returns the HTTP status or kNet*.
// Blocks: call from the cloud task only. ~10 s worst case.
int cloudHttp(const char *base, const char *method, const char *path,
              const char *body, size_t bodyLen, const char *token,
              uint8_t *out, size_t cap, size_t *outLen);
