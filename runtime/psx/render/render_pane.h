// render_pane.h — one upright rectangle of a present image, cut by two seam lines.
#pragma once

#include <SDL3/SDL_gpu.h>

namespace psx::render {

// Drawn 1:1 from `texture`, or as a flat `tint` when `solid`. Coordinates are present-image pixels, y down.
struct Pane {
  SDL_GPUTexture *texture = nullptr;
  bool solid = false;
  float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
  // The left and right boundary columns at the image's top and bottom; neighbours share them exactly.
  float seamLeftTop = 0.0f, seamLeftBottom = 0.0f;
  float seamRightTop = 0.0f, seamRightBottom = 0.0f;
  float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
  float alpha = 1.0f;
  float desaturation = 0.0f; // 0 = the picture's own colour, 1 = grey
};

} // namespace psx::render
