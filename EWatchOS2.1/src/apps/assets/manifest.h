// Registry of media assets compiled into the firmware. EDIT THIS FILE after
// running tools/encode_media.py to register newly-generated headers.
//
// Each entry:
//   { "Display name", MediaKind::Image|Video, W, H, FPS, FRAMES, pixels_ptr }
//
// "name", W, H, FPS, FRAMES, pixels are exposed by the encoder as constants
// inside a `media_<name>` namespace, so an entry typically looks like:
//
//     #include "logo.h"
//     ...
//     { "Logo", MediaKind::Image,
//       media_logo::W, media_logo::H,
//       media_logo::FPS, media_logo::FRAMES, media_logo::pixels },
//
#pragma once
#include "media.h"

// === user-included asset headers (uncomment / add) =================
#include "cwills.h"
#include "watch.h"

// #include "demo.h"

namespace media_manifest
{

  // === asset registry =================================================
  static const MediaAsset kAssets[] = {
      // Built-in procedural test pattern so the app shows something even with
      // no encoded assets present. Safe to keep or delete.
      {"Sample", MediaKind::BuiltinSample, 0, 0, 0, 0, nullptr},

      // Examples — uncomment after generating the headers.
      {"Claire Wills", MediaKind::Image,
       cwills::W, cwills::H,
       cwills::FPS, cwills::FRAMES, cwills::pixels},

      {"Watchception", MediaKind::Image,
       media_watch::W, media_watch::H,
       media_watch::FPS, media_watch::FRAMES, media_watch::pixels},
      // { "Demo", MediaKind::Video,
      //   media_demo::W, media_demo::H,
      //   media_demo::FPS, media_demo::FRAMES, media_demo::pixels },
  };
  constexpr int kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);

} // namespace media_manifest
