// One title's own logo as the picker draws it: an RGBA image, its copy in user data and its lazily uploaded texture.
#pragma once

#include "logo_image.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::host {

struct TitleIdentity;

// The logo last read from the running title, kept in its user-data directory so a panel never focused in this
// process still shows it. Read from the player's own disc at run time; never shipped.
class SavedLogo {
public:
  explicit SavedLogo(const TitleIdentity &title);

  std::optional<LogoImage> load() const;
  // Replaces the saved logo atomically; false when it cannot be written.
  bool save(const LogoImage &image) const;

private:
  std::optional<std::filesystem::path> file_;
};

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
