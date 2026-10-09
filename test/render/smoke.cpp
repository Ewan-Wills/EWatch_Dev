#include "hostgfx.h"
int main() { HostGFX g(240, 280); g.fillScreen(0xEB45); g.fillArc(120,140,60,50,0,270,0xFFFF); g.setCursor(10,10); g.setTextColor(0); g.print("12:47"); g.writePNG("smoke.png"); }
