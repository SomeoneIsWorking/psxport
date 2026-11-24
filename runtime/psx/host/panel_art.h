// One title's selector picture: its art drawn from its own disc and the GPU texture the picker draws it from.
#pragma once

#include "art_scene.h"

#include <SDL3/SDL_gpu.h>

#include <memory>

namespace psx::game {
class GameRuntime;
} // namespace psx::game

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::host {

struct TitleIdentity;

class PanelArt {
public:
  // The title's art read from the player's disc; null when the title names none or the disc does not supply it.
  static std::unique_ptr<PanelArt> read(const psx::game::GameRuntime &runtime, const TitleIdentity &title);

  explicit PanelArt(psx::present::ArtImage image);
  ~PanelArt();
  PanelArt(const PanelArt &) = delete;
  PanelArt &operator=(const PanelArt &) = delete;

  int width() const {
    return image_.width;
  }
  int height() const {
    return image_.height;
  }
  // Created on first use against `device` and destroyed with this object.
  SDL_GPUTexture *texture(psx::render::RenderDevice &device) const;

private:
  psx::present::ArtImage image_;
  mutable psx::render::RenderDevice *textureDevice_ = nullptr;
  mutable SDL_GPUTexture *texture_ = nullptr;
};

} // namespace psx::host
