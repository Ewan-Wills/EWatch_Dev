// Compiled mirror of cube.obj — the watch firmware can't parse OBJ at runtime
// (no filesystem), so the renderer reads this header. Keep in sync with
// cube.obj if you edit either.
#pragma once
#include <stdint.h>

namespace cube_model {

  // Vertices (xyz triples). Same order as the 'v' lines in cube.obj.
  constexpr float V[][3] = {
    {-1.f, -1.f, -1.f},   // 1
    { 1.f, -1.f, -1.f},   // 2
    { 1.f,  1.f, -1.f},   // 3
    {-1.f,  1.f, -1.f},   // 4
    {-1.f, -1.f,  1.f},   // 5
    { 1.f, -1.f,  1.f},   // 6
    { 1.f,  1.f,  1.f},   // 7
    {-1.f,  1.f,  1.f},   // 8
  };
  constexpr uint8_t V_COUNT = sizeof(V) / sizeof(V[0]);

  // Triangles, 0-indexed (OBJ uses 1-indexed). CCW outward.
  constexpr uint8_t F[][3] = {
    {0, 3, 2}, {0, 2, 1},   // -Z back
    {4, 5, 6}, {4, 6, 7},   // +Z front
    {0, 1, 5}, {0, 5, 4},   // -Y bottom
    {3, 7, 6}, {3, 6, 2},   // +Y top
    {0, 4, 7}, {0, 7, 3},   // -X left
    {1, 2, 6}, {1, 6, 5},   // +X right
  };
  constexpr uint8_t F_COUNT = sizeof(F) / sizeof(F[0]);

  // Diffuse base color (RGB888 0..255 each). The renderer scales this by the
  // light dot-product and packs to RGB565.
  constexpr uint8_t BASE_R = 90;
  constexpr uint8_t BASE_G = 160;
  constexpr uint8_t BASE_B = 220;
}
