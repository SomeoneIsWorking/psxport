#include "panel_logo.h"

#include "render_device.h"

#include <cstring>
#include <memory>
#include <vector>

namespace psx::host {

PanelLogo::PanelLogo(LogoImage image) : m_width(image.width), m_height(image.height), m_rgba(std::move(image.rgba)) {}

const PanelLogo &PanelLogo::magnifiedNearest(int factor) const {
  if (factor <= 1 || m_width <= 0 || m_height <= 0) {
    return *this;
  }
  if (m_magnified != nullptr && m_magnifiedFactor == factor) {
    return *m_magnified;
  }
  // Each texel becomes a factor x factor block of itself, so the logo keeps the game's pixel grid.
  std::vector<std::uint8_t> scaled(
      static_cast<std::size_t>(m_width) * factor * static_cast<std::size_t>(m_height) * factor * 4, 0);
  const int outW = m_width * factor;
  const int outH = m_height * factor;
  for (int y = 0; y < outH; ++y) {
    const int sy = y / factor;
    for (int x = 0; x < outW; ++x) {
      const int sx = x / factor;
      const std::size_t from = (static_cast<std::size_t>(sy) * m_width + sx) * 4;
      const std::size_t to = (static_cast<std::size_t>(y) * outW + x) * 4;
      std::memcpy(&scaled[to], &m_rgba[from], 4);
    }
  }
  m_magnified = std::make_unique<PanelLogo>(LogoImage{outW, outH, std::move(scaled)});
  m_magnifiedFactor = factor;
  return *m_magnified;
}

// The device outlives every panel, so a rebuilt picker does not leak one texture per title.
PanelLogo::~PanelLogo() {
  if (m_textureDevice != nullptr) {
    m_textureDevice->release(m_texture);
  }
}

SDL_GPUTexture *PanelLogo::texture(psx::render::RenderDevice &device) const {
  if (empty()) {
    return nullptr;
  }
  if (m_texture != nullptr && m_textureDevice == &device) {
    return m_texture;
  }
  if (m_textureDevice != nullptr) {
    m_textureDevice->release(m_texture);
  }
  m_texture = device.makeFilledTexture({m_width, m_height}, m_rgba);
  m_textureDevice = &device;
  return m_texture;
}

} // namespace psx::host
