// printer.example.h — tracked template for the printstream app's connection
// details. Copy to printer.local.h (git-ignored) and edit for your printer:
//
//   cp secrets/printer.example.h secrets/printer.local.h
//
// printstream.cpp includes printer.local.h when it exists and falls back to
// this file otherwise, so the firmware always compiles even with no secrets.
// secrets/ is on the include path via -Isecrets in platformio.ini.
#pragma once

// ---- Moonraker (print state / temps / progress / confirmed controls) ----
// Creality K2 Plus on the LAN, Moonraker REST, plain HTTP, no auth.
//   K2Plus-5821 : 192.168.0.214      K2Plus-248A : 192.168.0.244
#define PRINTER_HOST   "192.168.0.214"
#define MOONRAKER_PORT 7125

// ---- Camera (MJPEG proxy served by nginx on the printer) ----
// Discovered endpoint (Fluidd / mjpg-streamer-adaptive behind nginx:4408):
//   stream   : http://<host>:4408/webcam/?action=stream
//   snapshot : http://<host>:4408/webcam/?action=snapshot
// The cam daemon only serves frames while a print/camera is live; when it is
// down the app shows "Camera unavailable" and the stats page still works.
// The camera defaults to the same host as the printer.
#define CAM_HOST          PRINTER_HOST
#define CAM_PORT          4408
#define CAM_STREAM_PATH   "/webcam/?action=stream"
#define CAM_SNAPSHOT_PATH "/webcam/?action=snapshot"

// 1 = try the live MJPEG stream first (falls back to snapshot polling if the
// stream can't be opened or carries no per-part Content-Length). 0 = snapshot
// polling only.
#define CAM_USE_MJPEG 1

// Snapshot-fallback cadence (ms) when MJPEG is unavailable.
#define CAM_SNAPSHOT_MS 2000

// ---- Moonraker poll cadence ----
#define PS_POLL_PRINTING_MS 2000   // while printing / paused
#define PS_POLL_IDLE_MS     5000   // while idle / standby / complete
