#include "panel_art.h"

#include "game_runtime.h"
#include "render_device.h"
#include "title_catalog.h"
#include "title_disc.h"

#include <lucent/log.h>

#include <optional>
#include <utility>

namespace psx::host {

std::unique_ptr<PanelArt> PanelArt::read(const psx::game::GameRuntime &runtime, const TitleIdentity &title) {
  TitleDisc disc(title);
  if (!disc.opened()) {
    lucent::warn("picker", "{} has no readable disc; its panel shows a tile", title.displayName);
    return nullptr;
  }
  const std::optional<psx::present::ArtScene> scene = runtime.panelArt(disc.state());
  if (!scene) {
    lucent::info("picker", "{} names no panel art; its panel shows a tile", title.displayName);
    return nullptr;
  }
  std::optional<psx::present::ArtImage> image = psx::present::drawArt(*scene);
  if (!image) {
    lucent::warn("picker", "{}'s panel art did not draw; its panel shows a tile", title.displayName);
    return nullptr;
  }
  lucent::info("picker", "{} panel art {}x{} from its disc", title.displayName, image->width, image->height);
  return std::make_unique<PanelArt>(std::move(*image));
}

PanelArt::PanelArt(psx::present::ArtImage image) : image_(std::move(image)) {}

// The device outlives every panel, so a rebuilt picker does not leak one texture per title.
PanelArt::~PanelArt() {
  if (textureDevice_ != nullptr) {
    textureDevice_->release(texture_);
  }
}

SDL_GPUTexture *PanelArt::texture(psx::render::RenderDevice &device) const {
  if (texture_ != nullptr && textureDevice_ == &device) {
    return texture_;
  }
  if (textureDevice_ != nullptr) {
    textureDevice_->release(texture_);
  }
  texture_ = device.makeFilledTexture({image_.width, image_.height}, image_.rgba);
  textureDevice_ = &device;
  return texture_;
}

} // namespace psx::host
