// scea_splash.h — the SCEA license screen as a host image, from the baked asset (scea_asset.h).
#pragma once

#include <cstdint>
#include <vector>

namespace psx::platform {

inline constexpr int kSceaSplashWidth = 640;
inline constexpr int kSceaSplashHeight = 468;

// RGBA8, kSceaSplashWidth x kSceaSplashHeight: text pixels in their CLUT colour, everything else transparent black.
std::vector<std::uint8_t> decodeSceaSplash();

} // namespace psx::platform
