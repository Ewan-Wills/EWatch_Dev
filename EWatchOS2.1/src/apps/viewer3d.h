// 3D model viewer app. Software renderer with a per-pixel z-buffer.
//
//   * Model gallery — cow + cube from flash, torus knot / planet / gem
//     generated procedurally at runtime (no flash cost). Cycle with the
//     on-screen < > arrows.
//   * Shading modes — flat, Gouraud (smooth per-vertex), wireframe. Cycle
//     with a quick tap anywhere in the scene.
//   * Touch drag rotates; IMU tilt also feeds rotation while the finger is
//     off the screen.
//
// Compile-time gated by EWATCH_ENABLE_VIEWER3D — when 0 the class declaration
// and the implementation are both elided so the linker drops the math and
// the bundled model data.
#pragma once
#if defined(EWATCH_ENABLE_VIEWER3D) && EWATCH_ENABLE_VIEWER3D
#include "view.h"

class Viewer3DView : public View {
public:
  void onEnter() override;
  void render()  override;
  void onEvent(const Event &e) override;
  uint16_t desiredFrameMs() const override { return 33; }   // ~30 fps

private:
  // Euler angles in radians.
  float angleX = 0.6f;
  float angleY = 0.8f;
  // Touch drag state.
  int   lastTx = -1, lastTy = -1;
  bool  dragging  = false;
  bool  dragMoved = false;
  uint32_t pressMs = 0;

  int     modelIdx = 0;
  uint8_t shading  = 0;      // 0 flat, 1 gouraud, 2 wireframe

  void drawScene();
  void drawChrome();         // title + back + model/mode labels
  void switchModel(int dir);
  void cycleShading();
};

#endif  // EWATCH_ENABLE_VIEWER3D
