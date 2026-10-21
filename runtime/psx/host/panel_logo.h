// One title's own logo as the picker draws it: an RGBA image and its lazily uploaded texture.
#pragma once

#include "logo_image.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::host {

// An RGBA8 image in memory plus the GPU texture the picker draws it from.
class PanelLogo {
public:
  // `image.rgba` is `width * height * 4` bytes of straight alpha in the host's usual order.
  explicit PanelLogo(LogoImage image);
  ~PanelLogo();
  PanelLogo(const PanelLogo &) = delete;
  PanelLogo &operator=(const PanelLogo &) = delete;

  bool empty() const {
    return m_width <= 0 || m_height <= 0;
  }
  int width() const {
    return m_width;
  }
  int height() const {
    return m_height;
  }
  // Created on first use against `device` and destroyed with this object.
  SDL_GPUTexture *texture(psx::render::RenderDevice &device) const;

private:
  int m_width;
  int m_height;
  std::vector<std::uint8_t> m_rgba;
  mutable psx::render::RenderDevice *m_textureDevice = nullptr;
  mutable SDL_GPUTexture *m_texture = nullptr;
};

} // namespace psx::host
