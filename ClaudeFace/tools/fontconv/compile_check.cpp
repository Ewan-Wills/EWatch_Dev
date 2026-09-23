// Syntax-only host compile check for the generated GFXfont headers.
// PROGMEM is defined as a no-op for a host build; gfxfont.h in this same
// directory is byte-for-byte the project's actual
// "GFX Library for Arduino" src/gfxfont.h. Run with:
//   xcrun clang++ -std=c++14 -fsyntax-only compile_check.cpp
#include <stdint.h>

#define PROGMEM

#include "gfxfont.h"

#include "CF_SpaceGroteskBold40.h"
#include "CF_SpaceGroteskBold22.h"
#include "CF_SpaceGroteskMed12.h"
#include "CF_SpaceGroteskMed9.h"
#include "CF_JetBrainsMono10.h"
