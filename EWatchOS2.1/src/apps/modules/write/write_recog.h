// Handwriting recogniser for the Write app: $P point-cloud matching
// (Vatavu, Anthony & Wobbrock, "Gestures as Point Clouds", ICMI 2012) with
// the $Q speed-ups (Vatavu, Anthony & Wobbrock, MobileHCI 2018: squared
// Euclidean distance + early abandoning). Implemented from the papers'
// published pseudocode; no third-party code.
//
// Pure C++ (no Arduino / FreeRTOS), so the same file is compiled into the
// firmware, the offline simulator and the host bench (test/host/).
//
// Multi-stroke and stroke-order/direction independent: a glyph is the set of
// its points. Built-in templates (A-Z, 0-9) come from write_templates.h;
// user templates (taught on the watch) are added at runtime.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wrec {

constexpr int kN = 32;               // resample size (points per cloud)
constexpr int kMaxUser = 72;         // user templates kept (RAM + file)
constexpr int kUserPerChar = 3;      // newest N per character

struct Pt    { float x, y; };
struct RawPt { int16_t x, y; uint8_t stroke; };   // screen px + stroke index
struct Cand  { char ch; float dist; float score; bool user; };

// Build the built-in template clouds (idempotent; ~20 KB, PSRAM if present).
bool init();
// Host bench only: replace the built-in set with another blob (same format).
bool initFrom(const uint8_t *blob, int count);
int  builtinCount();
int  userCount();

// Points -> normalised cloud (resample to kN, scale to unit box, centroid at
// origin). Returns false for degenerate input (no extent / too few points).
bool normalize(const RawPt *pts, int n, Pt out[kN]);

// Best match per character, best first (distinct characters). Returns the
// number written (<= maxOut). score: 1 = identical, 0 = RMS point distance
// of half the glyph box or worse (see NOTES.md).
int  classify(const Pt cloud[kN], Cand *out, int maxOut);

// Edit gestures: a single straight stroke. Checked before classify().
enum class Edit : uint8_t { None, Space, Backspace, Enter };
Edit editGesture(const RawPt *pts, int n, int minLenPx = 50);
const char *editName(Edit e);

// User templates ("teach"): stored as normalised clouds.
bool addUser(char ch, const Pt cloud[kN]);   // replaces that char's oldest at the cap
void clearUser();
bool userAt(int i, char *ch, Pt out[kN]);

// Serialise user templates (for LittleFS). Format: "WRT1", u8 count,
// count * { char, int8 x[kN], int8 y[kN] } (coords * 100, clamped).
size_t userSerialize(uint8_t *buf, size_t cap);
bool   userDeserialize(const uint8_t *buf, size_t len);

}  // namespace wrec
