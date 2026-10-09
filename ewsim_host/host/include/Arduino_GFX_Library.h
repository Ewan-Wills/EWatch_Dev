// ewsim_host: replaces GFX Library for Arduino's umbrella header. Pulls in the
// REAL library core (Arduino_GFX, canvases, Arduino_TFT, the ST7789 driver)
// plus a host Arduino_ESP32SPI whose "wire" is a model of the ST7789 panel RAM
// (host/src/panel.cpp). That panel RAM is the frame the recorder films.
#pragma once
#include "Arduino_DataBus.h"
#include "Arduino_GFX.h"
#include "Arduino_TFT.h"
#include "canvas/Arduino_Canvas.h"
#include "canvas/Arduino_Canvas_Indexed.h"
#include "canvas/Arduino_Canvas_3bit.h"
#include "canvas/Arduino_Canvas_Mono.h"
#include "display/Arduino_ST7789.h"
#include "host_panel.h"
