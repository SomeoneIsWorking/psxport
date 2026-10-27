#include "image_writer.h"

#include "fs_util.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <lucent/log.h>
#include <string>
#include <vector>

namespace {

void putU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void putU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

// One PNG chunk: length, type, data, CRC over type and data.
void putChunk(std::vector<std::uint8_t> &out, const char type[4], const std::vector<std::uint8_t> &data) {
  putU32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t typeAt = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uLong crc = crc32(0, out.data() + typeAt, static_cast<uInt>(out.size() - typeAt));
  putU32(out, static_cast<std::uint32_t>(crc));
}

} // namespace

// PNG is the default so captures are directly viewable; only an explicit .ppm path keeps the raw
// P6 representation. This function owns directory creation and error reporting because callers do
// not know which filesystem or image-codec operation failed.
bool psx::debug::writeImageRgb24(const char *path, const unsigned char *rgb, int width, int height) {
  if (!path || !rgb || width <= 0 || height <= 0) {
    lucent::error("image_write",
                  "refusing to write: path={} rgb={} {}x{}",
                  path ? path : "(null)",
                  static_cast<const void *>(rgb),
                  width,
                  height);
    return false;
  }
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("image_write", "cannot create the parent directory of {} — NOTHING written", path);
    return false;
  }

  const size_t length = std::strlen(path);
  const bool ppm = length > 4 && std::strcmp(path + length - 4, ".ppm") == 0;
  if (ppm) {
    FILE *file = std::fopen(path, "wb");
    if (!file) {
      lucent::error("image_write", "fopen({}) failed: {}", path, std::strerror(errno));
      return false;
    }
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0 &&
              std::fwrite(rgb, 3, static_cast<size_t>(width) * height, file) == static_cast<size_t>(width) * height;
    if (!ok) {
      lucent::error("image_write", "short write to {}: {}", path, std::strerror(errno));
    }
    if (std::fclose(file) != 0) {
      lucent::error("image_write", "fclose({}) failed: {}", path, std::strerror(errno));
      ok = false;
    }
    return ok;
  }

  SDL_Surface *surface =
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGB24, const_cast<unsigned char *>(rgb), width * 3);
  if (!surface) {
    lucent::error("image_write", "SDL_CreateSurfaceFrom failed for {}: {}", path, SDL_GetError());
    return false;
  }
  const bool ok = IMG_SavePNG(surface, path);
  if (!ok) {
    lucent::error("image_write", "IMG_SavePNG({}) failed: {}", path, SDL_GetError());
  }
  SDL_DestroySurface(surface);
  return ok;
}

bool psx::debug::writeAnimationRgba(const char *path,
                                    std::span<const std::uint8_t> rgba,
                                    int width,
                                    int height,
                                    std::span<const std::uint16_t> delaysMs) {
  const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 4;
  if (!path || width <= 0 || height <= 0 || delaysMs.empty() || rgba.size() != frameBytes * delaysMs.size()) {
    lucent::error("image_write",
                  "refusing to write clip {}: {}x{}, {} frame(s)",
                  path ? path : "(null)",
                  width,
                  height,
                  delaysMs.size());
    return false;
  }
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("image_write", "cannot create the parent directory of {} — NOTHING written", path);
    return false;
  }
  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> chunk;
  putU32(chunk, static_cast<std::uint32_t>(width));
  putU32(chunk, static_cast<std::uint32_t>(height));
  chunk.insert(chunk.end(), {8, 2, 0, 0, 0}); // 8-bit RGB, deflate, adaptive filter, no interlace
  putChunk(png, "IHDR", chunk);
  chunk.clear();
  putU32(chunk, static_cast<std::uint32_t>(delaysMs.size()));
  putU32(chunk, 1); // played once
  putChunk(png, "acTL", chunk);

  // Each row: filter 1 (Sub) then RGB.
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 3 + 1;
  std::vector<std::uint8_t> raw(rowBytes * height);
  std::vector<std::uint8_t> packed(compressBound(static_cast<uLong>(raw.size())));
  std::uint32_t sequence = 0;
  for (std::size_t frame = 0; frame < delaysMs.size(); ++frame) {
    const std::uint8_t *pixels = rgba.data() + frame * frameBytes;
    for (int y = 0; y < height; ++y) {
      std::uint8_t *row = raw.data() + y * rowBytes;
      const std::uint8_t *in = pixels + static_cast<std::size_t>(y) * width * 4;
      row[0] = 1;
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < 3; ++c) {
          const std::uint8_t left = x > 0 ? in[(x - 1) * 4 + c] : 0;
          row[1 + x * 3 + c] = static_cast<std::uint8_t>(in[x * 4 + c] - left);
        }
      }
    }
    uLongf packedBytes = static_cast<uLongf>(packed.size());
    if (compress2(packed.data(), &packedBytes, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
      lucent::error("image_write", "deflate failed on frame {} of {}", frame, path);
      return false;
    }
    chunk.clear();
    putU32(chunk, sequence++);
    putU32(chunk, static_cast<std::uint32_t>(width));
    putU32(chunk, static_cast<std::uint32_t>(height));
    putU32(chunk, 0);
    putU32(chunk, 0);
    putU16(chunk, delaysMs[frame]);
    putU16(chunk, 1000);
    chunk.insert(chunk.end(), {0, 0}); // dispose none, blend source
    putChunk(png, "fcTL", chunk);
    chunk.clear();
    if (frame > 0) {
      putU32(chunk, sequence++);
    }
    chunk.insert(chunk.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packedBytes));
    putChunk(png, frame == 0 ? "IDAT" : "fdAT", chunk);
  }
  putChunk(png, "IEND", {});

  FILE *file = std::fopen(path, "wb");
  if (!file) {
    lucent::error("image_write", "fopen({}) failed: {}", path, std::strerror(errno));
    return false;
  }
  bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  if (!ok) {
    lucent::error("image_write", "short write to {}: {}", path, std::strerror(errno));
  }
  if (std::fclose(file) != 0) {
    lucent::error("image_write", "fclose({}) failed: {}", path, std::strerror(errno));
    ok = false;
  }
  return ok;
}
