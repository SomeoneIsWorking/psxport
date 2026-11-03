#include "drawn_logo.h"

#include "frame_record.h"
#include "gpu_device.h"
#include "vram_pixel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace psx::present {
namespace {

bool samples(const DrawPrimitive &primitive, std::span<const LogoTexture> textures) {
  if (!primitive.textured || primitive.kind == PrimitiveKind::Line) {
    return false;
  }
  const psx::gpu::VramPoint clut = psx::gpu::decodeClut(primitive.clutWord);
  return std::ranges::any_of(textures, [&primitive, clut](const LogoTexture &texture) {
    return texture.page.x == primitive.state.texPageX && texture.page.y == primitive.state.texPageY &&
           (!texture.clut || (texture.clut->x == clut.x && texture.clut->y == clut.y));
  });
}

// Screen bounds of the primitives drawn so far inside their draw areas, right and bottom exclusive.
struct Bounds {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool empty = true;

  void include(int x0, int y0, int x1, int y1) {
    if (empty) {
      *this = {x0, y0, x1, y1, false};
      return;
    }
    left = std::min(left, x0);
    top = std::min(top, y0);
    right = std::max(right, x1);
    bottom = std::max(bottom, y1);
  }

  void include(const DrawPrimitive &primitive) {
    int x0 = primitive.vertices[0].x;
    int y0 = primitive.vertices[0].y;
    int x1 = x0 + primitive.width;
    int y1 = y0 + primitive.height;
    // gpu.c leaves a polygon's rightmost column and bottom row undrawn.
    if (primitive.kind != PrimitiveKind::Sprite) {
      x1 = x0;
      y1 = y0;
      for (int vertex = 1; vertex < primitive.vertexCount; ++vertex) {
        const RecordVertex &at = primitive.vertices[static_cast<std::size_t>(vertex)];
        x0 = std::min(x0, at.x);
        y0 = std::min(y0, at.y);
        x1 = std::max(x1, at.x);
        y1 = std::max(y1, at.y);
      }
    }
    const RecordDrawState &state = primitive.state;
    x0 = std::max({x0, state.clipX0, 0});
    y0 = std::max({y0, state.clipY0, 0});
    x1 = std::min({x1, state.clipX1 + 1, psx::gpu::kVramWidth});
    y1 = std::min({y1, state.clipY1 + 1, psx::gpu::kVramHeight});
    if (x1 > x0 && y1 > y0) {
      include(x0, y0, x1, y1);
    }
  }
};

// GP0 words that make gpu.c draw `primitive` as it was recorded, its vertices relative to the draw offset
// `origin`.
class PrimitiveWords {
public:
  PrimitiveWords(const DrawPrimitive &primitive, psx::gpu::VramPoint origin) : primitive_(primitive), origin_(origin) {}

  void sendTo(psx::gpu::GpuDevice &device) const {
    const RecordDrawState &state = primitive_.state;
    device.gp0((0xE1u << 24) | texPage() | (primitive_.flipX ? 1u << 12 : 0u) | (primitive_.flipY ? 1u << 13 : 0u));
    device.gp0((0xE2u << 24) | static_cast<std::uint32_t>(state.windowMaskX & 0x1F) |
               (static_cast<std::uint32_t>(state.windowMaskY & 0x1F) << 5) |
               (static_cast<std::uint32_t>(state.windowOffsetX & 0x1F) << 10) |
               (static_cast<std::uint32_t>(state.windowOffsetY & 0x1F) << 15));
    device.gp0((0xE3u << 24) | corner(state.clipX0, state.clipY0));
    device.gp0((0xE4u << 24) | corner(state.clipX1, state.clipY1));
    device.gp0((0xE5u << 24) | (static_cast<std::uint32_t>(origin_.x) & 0x7FFu) |
               ((static_cast<std::uint32_t>(origin_.y) & 0x7FFu) << 11));
    if (primitive_.kind == PrimitiveKind::Sprite) {
      device.gp0(colour(0) | ((0x64u | flags()) << 24));
      device.gp0(position(0));
      device.gp0(texel(0) | (static_cast<std::uint32_t>(primitive_.clutWord) << 16));
      device.gp0((static_cast<std::uint32_t>(primitive_.height) << 16) | static_cast<std::uint32_t>(primitive_.width));
      return;
    }
    const std::uint32_t command =
        0x24u | flags() | (primitive_.gouraud ? 0x10u : 0u) | (primitive_.vertexCount == 4 ? 0x08u : 0u);
    for (int vertex = 0; vertex < primitive_.vertexCount; ++vertex) {
      if (vertex == 0) {
        device.gp0(colour(0) | (command << 24));
      } else if (primitive_.gouraud) {
        device.gp0(colour(vertex));
      }
      device.gp0(position(vertex));
      std::uint32_t attribute = 0;
      if (vertex == 0) {
        attribute = primitive_.clutWord;
      } else if (vertex == 1) {
        attribute = texPage();
      }
      device.gp0(texel(vertex) | (attribute << 16));
    }
  }

private:
  // Semi-transparency, and raw texture when the texel is not modulated.
  std::uint32_t flags() const {
    return (primitive_.semiTransparent ? 0x02u : 0u) | (primitive_.modulate ? 0u : 0x01u);
  }

  std::uint32_t texPage() const {
    const RecordDrawState &state = primitive_.state;
    return (static_cast<std::uint32_t>(state.texPageX / 64) & 0xFu) |
           ((static_cast<std::uint32_t>(state.texPageY / 256) & 1u) << 4) |
           ((static_cast<std::uint32_t>(state.blendMode) & 3u) << 5) |
           ((static_cast<std::uint32_t>(state.texMode) & 3u) << 7) | (1u << 10);
  }

  static std::uint32_t corner(int x, int y) {
    return (static_cast<std::uint32_t>(std::clamp(x, 0, 1023))) |
           (static_cast<std::uint32_t>(std::clamp(y, 0, 511)) << 10);
  }

  const RecordVertex &vertex(int index) const {
    return primitive_.vertices[static_cast<std::size_t>(index)];
  }

  std::uint32_t colour(int index) const {
    const RecordVertex &at = vertex(index);
    return static_cast<std::uint32_t>(at.r) | (static_cast<std::uint32_t>(at.g) << 8) |
           (static_cast<std::uint32_t>(at.b) << 16);
  }

  std::uint32_t position(int index) const {
    const RecordVertex &at = vertex(index);
    return (static_cast<std::uint32_t>(at.x - origin_.x) & 0x7FFu) |
           ((static_cast<std::uint32_t>(at.y - origin_.y) & 0x7FFu) << 16);
  }

  std::uint32_t texel(int index) const {
    const RecordVertex &at = vertex(index);
    return static_cast<std::uint32_t>(at.u) | (static_cast<std::uint32_t>(at.v) << 8);
  }

  const DrawPrimitive &primitive_;
  psx::gpu::VramPoint origin_;
};

} // namespace

std::optional<psx::host::LogoImage>
drawnLogo(const FrameRecord &record, std::span<const std::uint16_t> vram, std::span<const LogoTexture> textures) {
  std::vector<const DrawPrimitive *> drawn;
  Bounds bounds;
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !samples(*primitive, textures)) {
      continue;
    }
    bounds.include(*primitive);
    drawn.push_back(primitive);
  }
  if (drawn.empty() || bounds.empty) {
    return std::nullopt;
  }
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;

  // Binding the scratch device parks the live one, whose VRAM `vram` views.
  const std::vector<std::uint16_t> textureSource(vram.begin(), vram.end());
  psx::gpu::GpuDevice device;
  device.loadImage({0, 0, psx::gpu::kVramWidth, psx::gpu::kVramHeight}, textureSource, 0);
  const std::vector<std::uint16_t> cleared(static_cast<std::size_t>(width) * height, 0);
  device.loadImage({bounds.left, bounds.top, width, height}, cleared, 0);
  // Every pixel the logo writes carries the mask bit; texel 0 writes nothing and stays clear.
  device.gp0(0xE6000001u);
  const psx::gpu::VramPoint origin{bounds.left, bounds.top};
  for (const DrawPrimitive *primitive : drawn) {
    PrimitiveWords(*primitive, origin).sendTo(device);
  }

  const std::span<const std::uint16_t> drawnVram = device.vram();
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
  bool written = false;
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const std::uint16_t halfWord =
          drawnVram[(static_cast<std::size_t>(bounds.top + row) * psx::gpu::kVramWidth) + bounds.left + column];
      const psx::gpu::VramPixel pixel = psx::gpu::decodeVramPixel(halfWord);
      if (!pixel.mask) {
        continue;
      }
      written = true;
      std::uint8_t *out = &rgba[((static_cast<std::size_t>(row) * width) + column) * 4];
      out[0] = static_cast<std::uint8_t>(psx::gpu::scale5to8(pixel.red));
      out[1] = static_cast<std::uint8_t>(psx::gpu::scale5to8(pixel.green));
      out[2] = static_cast<std::uint8_t>(psx::gpu::scale5to8(pixel.blue));
      out[3] = 255;
    }
  }
  if (!written) {
    return std::nullopt;
  }
  return psx::host::LogoImage{width, height, std::move(rgba)};
}

} // namespace psx::present
