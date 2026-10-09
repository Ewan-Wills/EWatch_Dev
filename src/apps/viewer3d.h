// 3D model viewer app. Software-rendered painter's-algorithm renderer for the
// small cube model in apps/models/. Touch drag rotates; IMU tilt also feeds
// rotation while the finger is off the screen.
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

private:
  // Euler angles in radians.
  float angleX = 0.6f;
  float angleY = 0.8f;
  // Touch drag state.
  int   lastTx = -1, lastTy = -1;
  bool  dragging = false;
  bool  firstDraw = true;

  void drawScene();
};

#endif  // EWATCH_ENABLE_VIEWER3D
