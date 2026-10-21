// render.h — the Core-facing render queries and the device-VRAM and presented-image captures.
#pragma once

#include "pixel_size.h"
#include "vram_rect.h"

#include <cstdint>
#include <span>

class Core;

namespace psx::render {

// Resolution plan: native width, internal-resolution scale, framebuffer, sink and scale cap.
struct VideoStatus {
  int nativeWidth = 320;
  int ires = 1;
  int framebufferWidth = 320;
  int framebufferHeight = 240;
  int sinkWidth = 0;
  int sinkHeight = 0;
  int iresCap = 4;
};
VideoStatus videoStatus(Core &core);

// The device's VRAM: a rect as an image, raw words along a row, or the whole of it raw.
void vramRegion(Core &core, const char *path, gpu::VramRect rect);
void vramWords(Core &core, gpu::VramPoint at, std::span<std::uint16_t> out);
void vramRaw(Core &core, const char *path);

// An RGBA8 (BGRA8 when `bgra`) readback of a presented image written as RGB24 and logged on `channel` with its
// non-black share.
void writePresentShot(
    const char *channel, const char *path, const std::uint8_t *rgba, present::PixelSize size, bool bgra);

} // namespace psx::render
