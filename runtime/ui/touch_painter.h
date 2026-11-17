// touch_painter.h — TouchPainter: touch-ui's Painter over the RmlUi SDL_GPU renderer, inside its frame.
#pragma once

#include <touch_ui/touch_ui.h>

#include <cstdint>
#include <span>

class RmlRenderInterfaceGpu;

namespace psx::ui {

class TouchPainter final : public touch_ui::Painter {
public:
  explicit TouchPainter(RmlRenderInterfaceGpu &renderer) : renderer_(renderer) {}
  ~TouchPainter() override = default;
  TouchPainter(const TouchPainter &) = delete;
  TouchPainter &operator=(const TouchPainter &) = delete;

  std::uintptr_t upload(std::span<const std::uint8_t> rgba, int side) override;
  void release(std::uintptr_t image) override;
  void draw(std::uintptr_t image, std::span<const touch_ui::Vertex> vertices, std::span<const int> indices) override;

private:
  RmlRenderInterfaceGpu &renderer_;
};

} // namespace psx::ui
