// Template for secrets/secrets.local.h — the local, git-ignored key file.
//
// Keys are NEVER compiled by default. Only the opt-in env includes them:
//   cp secrets/secrets.example.h secrets/secrets.local.h   # then fill it in
//   pio run -e ewatch_secrets -t upload
// Every other env (ewatch, ewatch_v20) builds without any keys; enter them on
// the watch's web page instead (they're stored on the watch in NVS).
#pragma once

// Spotify app Client ID (developer.spotify.com -> your app -> Settings).
// Used only when the watch has no Client ID saved yet.
// #define EWATCH_SPOTIFY_CLIENT_ID "your-client-id"
