// touch_pad.cpp — see touch_pad.h.
#include "touch_pad.h"

#include "config_vars.h"
#include "host_input.h"

#include <lucent/log.h>

#include <utility>
#include <vector>

namespace psx::input {

namespace {

// A phone has no other input; a desktop shows the pad once a finger lands.
#if defined(__ANDROID__)
constexpr bool kTouchscreenPlatform = true;
#else
constexpr bool kTouchscreenPlatform = false;
#endif

touch_ui::Config padConfig() {
  using touch_ui::Placement;
  std::vector<touch_ui::Control> controls{
      {1, Placement::dpad_up, "direction_up", false, kButtonUp},
      {2, Placement::dpad_right, "direction_right", false, kButtonRight},
      {3, Placement::dpad_down, "direction_down", false, kButtonDown},
      {4, Placement::dpad_left, "direction_left", false, kButtonLeft},
      {5, Placement::action_primary, "face_circle", true, kButtonCircle},
      {6, Placement::action_secondary, "face_cross", true, kButtonCross},
      {7, Placement::action_tertiary, "face_square", true, kButtonSquare},
      {8, Placement::action_quaternary, "face_triangle", true, kButtonTriangle},
      {9, Placement::shoulder_left, "shoulder_l1", true, kButtonL1},
      {10, Placement::shoulder_left_2, "shoulder_l2", true, kButtonL2},
      {11, Placement::shoulder_right, "shoulder_r1", true, kButtonR1},
      {12, Placement::shoulder_right_2, "shoulder_r2", true, kButtonR2},
      {13, Placement::menu_left, "select", true, kButtonSelect},
      {14, Placement::menu_right, "start", true, kButtonStart},
  };
  return touch_ui::Config{.controls = std::move(controls)};
}

bool sameGeometry(const touch_ui::Geometry &a, const touch_ui::Geometry &b) {
  return a.output_width == b.output_width && a.output_height == b.output_height && a.display_scale == b.display_scale &&
         a.safe.left == b.safe.left && a.safe.top == b.safe.top && a.safe.right == b.safe.right &&
         a.safe.bottom == b.safe.bottom;
}

} // namespace

TouchPad::TouchPad()
    : touchSeen_(kTouchscreenPlatform), controls_(padConfig(), [this](std::uint32_t action, bool down) {
        const auto bits = static_cast<std::uint16_t>(action);
        held_ = down ? static_cast<std::uint16_t>(held_ | bits) : static_cast<std::uint16_t>(held_ & ~bits);
        lucent::debug("touch", "pad 0x{:04X} {}: port 1 mask 0x{:04X}", bits, down ? "down" : "up", mask());
      }) {
  followSetting();
}

void TouchPad::followSetting() {
  controls_.set_enabled(touchSeen_ && psx::config::touch_controls());
}

bool TouchPad::event(const SDL_Event &event) {
  if (event.type == SDL_EVENT_FINGER_DOWN) {
    touchSeen_ = true;
  }
  followSetting();
  return controls_.handle_event(event);
}

std::uint16_t TouchPad::mask() const {
  return static_cast<std::uint16_t>(~held_);
}

void TouchPad::layOut(SDL_Window *window, int width, int height) {
  followSetting();
  touch_ui::Geometry geometry{.output_width = width,
                              .output_height = height,
                              .display_scale = 1.0F,
                              .safe = {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)}};
  SDL_Rect safe{};
  int windowWidth = 0;
  int windowHeight = 0;
  if (window != nullptr && SDL_GetWindowSafeArea(window, &safe) &&
      SDL_GetWindowSize(window, &windowWidth, &windowHeight) && windowWidth > 0 && windowHeight > 0) {
    // The safe area is in window points; the controls draw in output pixels.
    const float sx = static_cast<float>(width) / static_cast<float>(windowWidth);
    const float sy = static_cast<float>(height) / static_cast<float>(windowHeight);
    geometry.display_scale = SDL_GetWindowDisplayScale(window);
    geometry.safe = {static_cast<float>(safe.x) * sx,
                     static_cast<float>(safe.y) * sy,
                     static_cast<float>(safe.x + safe.w) * sx,
                     static_cast<float>(safe.y + safe.h) * sy};
  }
  if (!sameGeometry(geometry, geometry_)) {
    geometry_ = geometry;
    controls_.set_geometry(geometry_);
  }
}

} // namespace psx::input
