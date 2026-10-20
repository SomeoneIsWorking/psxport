// render_screen.cpp — where a Game's present goes: the window, or its own image for a pane host.
#include "render_device.h"
#include "render_state.h"

namespace psx::render {

void RenderState::presentToPane(PixelSize image) {
  requestedImage_ = image;
  target_ = PresentTarget::Pane;
}

void RenderState::presentToWindow() {
  requestedImage_ = {};
  target_ = PresentTarget::Swapchain;
}

PixelSize RenderState::presentImageSize() {
  if (target_ == PresentTarget::Pane && requestedImage_.width > 0 && requestedImage_.height > 0) {
    return requestedImage_;
  }
  return device_.sinkSize();
}

} // namespace psx::render
