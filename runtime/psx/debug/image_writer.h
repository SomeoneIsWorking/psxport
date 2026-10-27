// image_writer.h — one host-file boundary for RGB24 captures.
#pragma once

#include <cstdint>
#include <span>

namespace psx::debug {

// Writes RGB24 pixels as PPM only for an explicit .ppm suffix; every other suffix uses PNG.
// Returns false when no complete image was written and logs the failing operation at its owner.
bool writeImageRgb24(const char *path, const unsigned char *rgb, int width, int height);

// Writes `delaysMs.size()` RGBA8 frames of `width`x`height`, packed in order in `rgba`, as one animated PNG
// played once; frame i shows for delaysMs[i]. Returns false when no complete file was written.
bool writeAnimationRgba(const char *path,
                        std::span<const std::uint8_t> rgba,
                        int width,
                        int height,
                        std::span<const std::uint16_t> delaysMs);

} // namespace psx::debug
