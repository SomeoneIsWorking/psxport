#include "art_scene.h"

#include "gpu_device.h"
#include "vram_pixel.h"

#include <lucent/log.h>

#include <cstddef>

namespace psx::present {
namespace {

bool overlaps(const gpu::VramRect &a, const gpu::VramRect &b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

bool insideVram(const gpu::VramRect &rect) {
  return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 && rect.x + rect.width <= gpu::kVramWidth &&
         rect.y + rect.height <= gpu::kVramHeight;
}

std::uint32_t corner(int x, int y) {
  return static_cast<std::uint32_t>(x) | (static_cast<std::uint32_t>(y) << 10);
}

} // namespace

std::optional<ArtImage> drawArt(const ArtScene &scene) {
  const gpu::VramRect &canvas = scene.canvas;
  if (!insideVram(canvas)) {
    lucent::error("art", "canvas {}x{} at {},{} is outside VRAM", canvas.width, canvas.height, canvas.x, canvas.y);
    return std::nullopt;
  }
  gpu::GpuDevice device;
  for (const ArtUpload &upload : scene.uploads) {
    if (!insideVram(upload.rect) || overlaps(upload.rect, canvas) ||
        upload.words.size() != static_cast<std::size_t>(upload.rect.width) * upload.rect.height) {
      lucent::error("art",
                    "upload {}x{} at {},{} ({} words) is outside VRAM, over the canvas or the wrong size",
                    upload.rect.width,
                    upload.rect.height,
                    upload.rect.x,
                    upload.rect.y,
                    upload.words.size());
      return std::nullopt;
    }
    device.loadImage(upload.rect, upload.words, 0);
  }
  device.loadImage(canvas, std::vector<std::uint16_t>(static_cast<std::size_t>(canvas.width) * canvas.height, 0), 0);
  // Every pixel a sprite writes carries the mask bit; texel 0 writes nothing and stays clear.
  device.gp0(0xE6000001u);
  device.gp0((0xE3u << 24) | corner(canvas.x, canvas.y));
  device.gp0((0xE4u << 24) | corner(canvas.x + canvas.width - 1, canvas.y + canvas.height - 1));
  device.gp0((0xE5u << 24) | (static_cast<std::uint32_t>(canvas.x) & 0x7FFu) |
             ((static_cast<std::uint32_t>(canvas.y) & 0x7FFu) << 11));
  for (const ArtSprite &sprite : scene.sprites) {
    device.gp0((0xE1u << 24) | (sprite.texPage & 0x1FFu) | (1u << 10));
    device.gp0(0x65808080u);
    device.gp0((static_cast<std::uint32_t>(sprite.x) & 0x7FFu) |
               ((static_cast<std::uint32_t>(sprite.y) & 0x7FFu) << 16));
    device.gp0((static_cast<std::uint32_t>(sprite.u) & 0xFFu) | ((static_cast<std::uint32_t>(sprite.v) & 0xFFu) << 8) |
               (static_cast<std::uint32_t>(sprite.clut) << 16));
    device.gp0((static_cast<std::uint32_t>(sprite.height) << 16) | static_cast<std::uint32_t>(sprite.width));
  }

  const std::span<const std::uint16_t> drawn = device.vram().all();
  ArtImage image{canvas.width,
                 canvas.height,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(canvas.width) * canvas.height * 4, 0)};
  bool written = false;
  for (int row = 0; row < canvas.height; ++row) {
    for (int column = 0; column < canvas.width; ++column) {
      const gpu::VramPixel pixel =
          gpu::decodeVramPixel(drawn[(static_cast<std::size_t>(canvas.y + row) * gpu::kVramWidth) + canvas.x + column]);
      if (!pixel.mask) {
        continue;
      }
      written = true;
      std::uint8_t *out = &image.rgba[((static_cast<std::size_t>(row) * canvas.width) + column) * 4];
      out[0] = static_cast<std::uint8_t>(gpu::scale5to8(pixel.red));
      out[1] = static_cast<std::uint8_t>(gpu::scale5to8(pixel.green));
      out[2] = static_cast<std::uint8_t>(gpu::scale5to8(pixel.blue));
      out[3] = 255;
    }
  }
  if (!written) {
    lucent::error("art", "{} sprite(s) wrote no pixel", scene.sprites.size());
    return std::nullopt;
  }
  return image;
}

} // namespace psx::present
