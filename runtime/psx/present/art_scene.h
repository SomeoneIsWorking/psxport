// art_scene.h — a title's still art as its own screen draws it: VRAM uploads and raw sprites, drawn by gpu.c.
#pragma once

#include "vram_rect.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace psx::present {

// Straight-alpha RGBA8, `width * height * 4` bytes.
struct ArtImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

// Halfwords for one VRAM rectangle, row after row.
struct ArtUpload {
  gpu::VramRect rect;
  std::vector<std::uint16_t> words;
};

// A raw-texture sprite (GP0 0x65) at a canvas position, through a texpage and CLUT word.
struct ArtSprite {
  int x = 0;
  int y = 0;
  int u = 0;
  int v = 0;
  int width = 0;
  int height = 0;
  std::uint16_t texPage = 0;
  std::uint16_t clut = 0;
};

struct ArtScene {
  // Where gpu.c draws the picture; it overlaps no upload.
  gpu::VramRect canvas;
  std::vector<ArtUpload> uploads;
  // Drawn in order; texel 0 draws nothing.
  std::vector<ArtSprite> sprites;
};

// The scene drawn over a cleared canvas on a scratch device; a pixel no sprite wrote is transparent. nullopt, logged,
// for a canvas outside VRAM or over an upload, or a scene that writes no pixel.
std::optional<ArtImage> drawArt(const ArtScene &scene);

} // namespace psx::present
