// render_window.h — the one SDL window and its GPU swapchain for the whole product run.
#pragma once

#include "pixel_size.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <string>

namespace psx::render {

// The window's creation size in display units, unless PSXPORT_PRESENT_SINK names one in pixels.
inline constexpr present::PixelSize kWindowSize{960, 720};

// Created before the device and destroyed after it, so teardown is explicit steps, not a destructor.
// Shown for a player, unmapped for a tool; everything after creation is the same window.
class RenderWindow {
public:
  RenderWindow() = default;
  RenderWindow(const RenderWindow &) = delete;
  RenderWindow &operator=(const RenderWindow &) = delete;

  // Opens the window, mapped only when `shown`. `size` is in display units, or in pixels when `exactPixels`.
  void create(bool shown, const char *title, present::PixelSize size, bool exactPixels);
  // Claims the window for `device` with a present mode that does not stall the guest thread.
  void claim(SDL_GPUDevice *device);
  // Teardown: leave the device, then destroy once the device is gone.
  void releaseDevice(SDL_GPUDevice *device);
  void destroy();
  // Tells the player `message` in a modal error box over a shown window; a tool's unmapped window only logs it.
  void showError(const char *title, const std::string &message) const;

  SDL_Window *window() const {
    return window_;
  }
  SDL_GPUTextureFormat swapchainFormat() const {
    return swapchainFormat_;
  }
  // The drawable in pixels.
  present::PixelSize pixelSize() const;
  // The refresh period of the window's display in ms; 0 when unknown.
  double refreshPeriodMs() const {
    return refreshPeriodMs_;
  }

private:
  // Read at creation and again when the window changes display or the display changes mode.
  void readRefreshPeriod();
  static bool SDLCALL watchDisplay(void *self, SDL_Event *event);

  SDL_Window *window_ = nullptr;
  bool shown_ = false;
  double refreshPeriodMs_ = 0.0;
  SDL_GPUTextureFormat swapchainFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
};

} // namespace psx::render
