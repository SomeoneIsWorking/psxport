// SPDX-License-Identifier: GPL-3.0-or-later
// One title's own logo as the picker draws it: an RGBA image and its lazily uploaded texture.
#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <vector>

class GpuDevice;

namespace psx::host {

// An RGBA8 image in memory plus the GPU texture the picker draws it from.
class PanelLogo {
public:
  // `rgba` is `width * height * 4` bytes of straight alpha in the host's usual order.
  PanelLogo(int width, int height, std::vector<std::uint8_t> rgba);
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
  // Nearest-neighbour magnification, so a panel shows the game's own texels at its own scale.
  // Built on first ask and kept: a panel asks every frame, but the logo never changes.
  // Returns `*this` when there is nothing to magnify.
  const PanelLogo &magnifiedNearest(int factor) const;
  // Created on first use against `device` and destroyed with this object. Const because the
  // pixels never change: filling the texture cache does not alter the image.
  SDL_GPUTexture *texture(GpuDevice &device) const;

private:
  int m_width;
  int m_height;
  std::vector<std::uint8_t> m_rgba;
  // A texture belongs to the device that made it, and is released through that one.
  mutable SDL_GPUDevice *m_textureDevice = nullptr;
  mutable SDL_GPUTexture *m_texture = nullptr;
  // Lazy, and keyed by the factor it was built for; a logo is immutable, so one copy per factor
  // suffices. A factor of 0 means "not built yet".
  mutable std::unique_ptr<PanelLogo> m_magnified;
  mutable int m_magnifiedFactor = 0;
};

} // namespace psx::host
