#include "hostgfx.h"
#include "Arduino_Canvas.h"
int main() { HostGFX g; g.fillScreen(0xEB45); g.fillArc(120,140,60,50,0,270,0xFFFF); g.setCursor(10,10); g.setTextColor(0); g.print("12:47"); g.writePNG("/private/tmp/claude-501/smoke.png"); }
