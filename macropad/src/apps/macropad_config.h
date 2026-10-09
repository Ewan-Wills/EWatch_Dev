// ---------------------------------------------------------------------------
// Macropad layout + labels — EDIT THIS FILE to change the pad.
//
// The watch draws a MACROPAD_COLS x MACROPAD_ROWS grid of labelled tiles.
// Tapping tile N sends `MACRO N\n` over USB serial (N is 0-based, row-major:
// left-to-right, top-to-bottom). The PC listener (see pc/) maps each N to a
// real action. Keep kMacroLabels in the SAME order and length as the grid.
//
// Supported grids for the 240x280 screen: 2x3 (6 tiles, big & readable) or
// 3x3 (9 tiles, tighter). Other sizes work but labels may clip.
// ---------------------------------------------------------------------------
#pragma once

// Grid dimensions.
#define MACROPAD_COLS 2
#define MACROPAD_ROWS 3

// One label per tile, row-major. MUST have exactly MACROPAD_COLS*MACROPAD_ROWS
// entries. Short labels read best (<= 8 chars draw at the large font size).
static const char *const kMacroLabels[] = {
  "Copy",       // MACRO 0
  "Paste",      // MACRO 1
  "Undo",       // MACRO 2
  "Redo",       // MACRO 3
  "Mission",    // MACRO 4  (Mission Control)
  "Spotlight",  // MACRO 5
};

// Number of tiles the grid holds.
#define MACROPAD_COUNT (MACROPAD_COLS * MACROPAD_ROWS)
