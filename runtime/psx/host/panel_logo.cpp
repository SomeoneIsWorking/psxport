#include "panel_logo.h"

#include "render_device.h"

#include <utility>

namespace psx::host {

PanelLogo::PanelLogo(LogoImage image) : m_width(image.width), m_height(image.height), m_rgba(std::move(image.rgba)) {}

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
