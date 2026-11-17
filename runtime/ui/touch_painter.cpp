// touch_painter.cpp — see touch_painter.h.
#include "touch_painter.h"

#include "rmlui_render_gpu.h"

#include <RmlUi/Core/Vertex.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace psx::ui {

namespace {

// The renderer blends premultiplied alpha; touch-ui hands straight alpha.
std::uint8_t premultiplied(float channel, float alpha) {
  return static_cast<std::uint8_t>(std::lround(std::clamp(channel * alpha, 0.0F, 1.0F) * 255.0F));
}

} // namespace

std::uintptr_t TouchPainter::upload(std::span<const std::uint8_t> rgba, int side) {
  std::vector<Rml::byte> pixels(rgba.begin(), rgba.end());
  for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const unsigned alpha = pixels[i + 3];
    for (std::size_t c = 0; c < 3; c++) {
      pixels[i + c] = static_cast<Rml::byte>((pixels[i + c] * alpha + 127U) / 255U);
    }
  }
  return renderer_.GenerateTexture({pixels.data(), pixels.size()}, {side, side});
}

void TouchPainter::release(std::uintptr_t image) {
  renderer_.ReleaseTexture(image);
}

void TouchPainter::draw(std::uintptr_t image,
                        std::span<const touch_ui::Vertex> vertices,
                        std::span<const int> indices) {
  std::vector<Rml::Vertex> converted;
  converted.reserve(vertices.size());
  for (const touch_ui::Vertex &vertex : vertices) {
    Rml::Vertex out;
    out.position = {vertex.position.x, vertex.position.y};
    out.colour = Rml::ColourbPremultiplied(premultiplied(vertex.red, vertex.alpha),
                                           premultiplied(vertex.green, vertex.alpha),
                                           premultiplied(vertex.blue, vertex.alpha),
                                           premultiplied(1.0F, vertex.alpha));
    out.tex_coord = {vertex.uv.x, vertex.uv.y};
    converted.push_back(out);
  }
  const Rml::CompiledGeometryHandle geometry =
      renderer_.CompileGeometry({converted.data(), converted.size()}, {indices.data(), indices.size()});
  if (geometry == 0) {
    return;
  }
  renderer_.RenderGeometry(geometry, {0.0F, 0.0F}, image);
  renderer_.ReleaseGeometry(geometry);
}

} // namespace psx::ui
