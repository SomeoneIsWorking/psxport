#include "drawn_logo.h"

#include "frame_record.h"
#include "vram_pixel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace psx::present {
namespace {

// A wordmark fits one display; anything larger is not a logo.
constexpr int kMaximumSide = 1024;

struct Canvas {
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

struct TexelSource {
  std::span<const std::uint16_t> vram;
  psx::gpu::TexPage page;
  psx::gpu::VramPoint clut;
  bool modulate = false;
};

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

void extend(int &left, int &top, int &right, int &bottom, const DrawPrimitive &primitive) {
  if (primitive.kind == PrimitiveKind::Sprite) {
    const RecordVertex &at = primitive.vertices[0];
    left = std::min(left, at.x);
    top = std::min(top, at.y);
    right = std::max(right, at.x + primitive.width);
    bottom = std::max(bottom, at.y + primitive.height);
    return;
  }
  for (int vertex = 0; vertex < primitive.vertexCount; ++vertex) {
    const RecordVertex &at = primitive.vertices[static_cast<std::size_t>(vertex)];
    left = std::min(left, at.x);
    top = std::min(top, at.y);
    right = std::max(right, at.x + 1);
    bottom = std::max(bottom, at.y + 1);
  }
}

// Texel 0 is not drawn; modulation is the GPU's colour * texel / 128.
void plot(Canvas &canvas, const TexelSource &source, int x, int y, int u, int v, int red, int green, int blue) {
  const int column = x - canvas.left;
  const int row = y - canvas.top;
  if (column < 0 || column >= canvas.width || row < 0 || row >= canvas.height) {
    return;
  }
  const std::uint16_t texel = psx::gpu::sampleTexel(source.vram, source.page, source.clut, u & 0xFF, v & 0xFF);
  if (texel == 0) {
    return;
  }
  const psx::gpu::VramPixel pixel = psx::gpu::decodeVramPixel(texel);
  std::array<int, 3> channel{
      psx::gpu::scale5to8(pixel.red), psx::gpu::scale5to8(pixel.green), psx::gpu::scale5to8(pixel.blue)};
  if (source.modulate) {
    channel[0] = std::min(255, channel[0] * red / 128);
    channel[1] = std::min(255, channel[1] * green / 128);
    channel[2] = std::min(255, channel[2] * blue / 128);
  }
  std::uint8_t *out = &canvas.rgba[((static_cast<std::size_t>(row) * canvas.width) + column) * 4];
  out[0] = static_cast<std::uint8_t>(channel[0]);
  out[1] = static_cast<std::uint8_t>(channel[1]);
  out[2] = static_cast<std::uint8_t>(channel[2]);
  out[3] = 255;
}

void drawSprite(Canvas &canvas, const TexelSource &source, const DrawPrimitive &primitive) {
  const RecordVertex &at = primitive.vertices[0];
  for (int row = 0; row < primitive.height; ++row) {
    for (int column = 0; column < primitive.width; ++column) {
      const int u = at.u + (primitive.flipX ? -column : column);
      const int v = at.v + (primitive.flipY ? -row : row);
      plot(canvas, source, at.x + column, at.y + row, u, v, at.r, at.g, at.b);
    }
  }
}

// Affine over pixel centres, as the GPU interpolates texture coordinates.
void drawTriangle(
    Canvas &canvas, const TexelSource &source, const RecordVertex &a, const RecordVertex &b, const RecordVertex &c) {
  const long long area =
      (static_cast<long long>(b.x - a.x) * (c.y - a.y)) - (static_cast<long long>(b.y - a.y) * (c.x - a.x));
  if (area == 0) {
    return;
  }
  const int left = std::min({a.x, b.x, c.x});
  const int right = std::max({a.x, b.x, c.x});
  const int top = std::min({a.y, b.y, c.y});
  const int bottom = std::max({a.y, b.y, c.y});
  const double inverse = 1.0 / static_cast<double>(area);
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const double px = x + 0.5;
      const double py = y + 0.5;
      const double wa = (((b.x - px) * (c.y - py)) - ((b.y - py) * (c.x - px))) * inverse;
      const double wb = (((c.x - px) * (a.y - py)) - ((c.y - py) * (a.x - px))) * inverse;
      const double wc = 1.0 - wa - wb;
      if (wa < 0.0 || wb < 0.0 || wc < 0.0) {
        continue;
      }
      const auto mix = [&](int ka, int kb, int kc) {
        return static_cast<int>((wa * ka) + (wb * kb) + (wc * kc));
      };
      plot(canvas,
           source,
           x,
           y,
           mix(a.u, b.u, c.u),
           mix(a.v, b.v, c.v),
           mix(a.r, b.r, c.r),
           mix(a.g, b.g, c.g),
           mix(a.b, b.b, c.b));
    }
  }
}

// Under one texel in fifty drawn is a fade or a part, not the wordmark; a saved empty panel would last.
bool holdsArtwork(const Canvas &canvas) {
  std::size_t visible = 0;
  for (std::size_t at = 3; at < canvas.rgba.size(); at += 4) {
    visible += canvas.rgba[at] != 0 ? 1 : 0;
  }
  return visible * 50 >= static_cast<std::size_t>(canvas.width) * canvas.height;
}

} // namespace

std::optional<psx::host::LogoImage>
drawnLogo(const FrameRecord &record, std::span<const std::uint16_t> vram, std::span<const LogoTexture> textures) {
  std::vector<const DrawPrimitive *> drawn;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !samples(*primitive, textures)) {
      continue;
    }
    if (drawn.empty()) {
      left = primitive->vertices[0].x;
      top = primitive->vertices[0].y;
      right = left;
      bottom = top;
    }
    extend(left, top, right, bottom, *primitive);
    drawn.push_back(primitive);
  }
  const int width = right - left;
  const int height = bottom - top;
  if (drawn.empty() || width <= 0 || height <= 0 || width > kMaximumSide || height > kMaximumSide) {
    return std::nullopt;
  }

  Canvas canvas{left, top, width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0)};
  for (const DrawPrimitive *primitive : drawn) {
    psx::gpu::TexPage page{primitive->state.texPageX,
                           primitive->state.texPageY,
                           primitive->state.blendMode,
                           static_cast<psx::gpu::TexDepth>(primitive->state.texMode & 3)};
    const TexelSource source{vram, page, psx::gpu::decodeClut(primitive->clutWord), primitive->modulate};
    if (primitive->kind == PrimitiveKind::Sprite) {
      drawSprite(canvas, source, *primitive);
      continue;
    }
    const std::array<RecordVertex, 4> &corner = primitive->vertices;
    drawTriangle(canvas, source, corner[0], corner[1], corner[2]);
    if (primitive->vertexCount == 4) {
      drawTriangle(canvas, source, corner[1], corner[2], corner[3]);
    }
  }
  if (!holdsArtwork(canvas)) {
    return std::nullopt;
  }
  return psx::host::LogoImage{width, height, std::move(canvas.rgba)};
}

} // namespace psx::present
