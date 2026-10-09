// Cross-check: frames produced by the real TypeScript bridge must parse in
// the firmware's C parser, and decode to the same field values.
// Input on stdin: "#CF..." lines from the bridge. Prints one summary per frame.
#include <stdio.h>
#include <string.h>
#include "cf_logic.h"
int main() {
  char line[512]; int ok = 0, bad = 0;
  while (fgets(line, sizeof line, stdin)) {
    if (strncmp(line, "#CF", 3)) continue;
    CFFrame f; CFParseResult r = cfParseLine(line, f);
    if (r != CF_PARSE_OK && r != CF_PARSE_VERSION) { printf("BAD r=%d %.40s\n", r, line); bad++; continue; }
    ok++;
    if (r == CF_PARSE_VERSION) { printf("VERSION v=%u\n", f.version); continue; }
    if (f.type == CF_TYPE_STATE) {
      CFStateMsg s; memcpy(&s, f.payload, sizeof s);
      printf("S seq=%u g=%u p=%u tool=%s proj=%s 5h=%u 7d=%u ctx=%u lines=%u/%u last=%u flags=%u\n", s.seq, s.global_state, s.p_state,
             s.p_current_tool, s.p_project, s.five_used, s.seven_used, s.p_ctx, s.p_lines_added, s.p_lines_removed, (unsigned)s.p_last_turn_s, s.flags);
    } else if (f.type == CF_TYPE_SESSIONS) {
      CFSessionsMsg m; memcpy(&m, f.payload, sizeof m);
      printf("L count=%u row0=%s\n", m.count, m.rows[0].project);
    } else if (f.type == CF_TYPE_TIME) {
      CFTimeMsg t; memcpy(&t, f.payload, sizeof t);
      printf("T epoch=%u off=%d\n", (unsigned)t.epoch, t.utc_offset_min);
    }
  }
  fprintf(stderr, "ok=%d bad=%d\n", ok, bad);
  return bad ? 1 : 0;
}
