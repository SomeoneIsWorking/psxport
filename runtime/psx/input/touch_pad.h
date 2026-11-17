// touch_pad.h — psx::input::TouchPad: the on-screen PSX pad of a touchscreen (shared touch-ui), held into port 1.
#pragma once

#include <SDL3/SDL.h>
#include <touch_ui/touch_ui.h>

#include <cstdint>

namespace psx::input {

class TouchPad {
public:
  TouchPad();

  // A finger the pad shows for is the pad's: it reaches neither the overlay nor the game.
  bool event(const SDL_Event &event);
  // A physical controller in use hides the pad until the next touch.
  void noteController() {
    controls_.note_controller_input();
  }
  // Port 1's held buttons, active-low.
  std::uint16_t mask() const;
  // Place the controls for `window`'s output of `width` x `height` pixels and its safe area.
  void layOut(SDL_Window *window, int width, int height);
  // Whether the last layout has controls to draw.
  bool visible() const {
    return controls_.visible();
  }
  // Over the presented frame, inside `painter`'s open frame.
  void draw(touch_ui::Painter &painter) {
    controls_.present(painter);
  }

private:
  // Shown while the player's setting allows it and this process has seen a touchscreen.
  void followSetting();

  std::uint16_t held_ = 0; // active-high
  bool touchSeen_;
  touch_ui::Geometry geometry_{};
  touch_ui::Controls controls_;
};

} // namespace psx::input
