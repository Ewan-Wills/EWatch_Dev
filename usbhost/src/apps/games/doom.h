// DOOM — a Doom-style raycaster for the EWatch (ST7789 240x280, CST816S touch).
//
// The full id Software WAD won't fit in 4 MB of flash, so this is a self-
// contained Wolfenstein/Doom-style raycasting FPS: textured walls, billboard
// demons, hitscan shooting, a HUD, and a touch + side-button control scheme.
//
//   doomInit()   — build textures + level, paint the title frame. Call once
//                  from setup() after the display + touch are up.
//   doomFrame()  — advance and render one frame. Call repeatedly from loop().
#pragma once

void doomInit();
void doomFrame();
// True once after the player holds the side button ~2 s in-game (exit request).
bool doomExitRequested();
