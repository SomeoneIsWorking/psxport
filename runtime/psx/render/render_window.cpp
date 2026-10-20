// render_window.cpp — RenderWindow.
#include "render_window.h"

#include "config_vars.h"
#include "present_mode.h"
#include "render_check.h"

#include <lucent/log.h>

namespace psx::render {

void RenderWindow::create(bool headless, const char *title) {
  if (headless) {
    return;
  }
  // HIGH_PIXEL_DENSITY sizes the swapchain and the UI in physical pixels on a scaled display.
  const SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                (psx::config::fullscreen_requested() ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
  window_ = SDL_CreateWindow(title, kWindowSize.width, kWindowSize.height, flags);
  GPUCHK(window_, "SDL_CreateWindow");
}

void RenderWindow::claim(SDL_GPUDevice *device) {
  GPUCHK(SDL_ClaimWindowForGPUDevice(device, window_), "SDL_ClaimWindowForGPUDevice");
  const SDL_GPUPresentMode want =
      preferredPresentMode(SDL_WindowSupportsGPUPresentMode(device, window_, SDL_GPU_PRESENTMODE_MAILBOX),
                           SDL_WindowSupportsGPUPresentMode(device, window_, SDL_GPU_PRESENTMODE_IMMEDIATE));
  const bool set = SDL_SetGPUSwapchainParameters(device, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, want);
  if (!set) {
    lucent::warn("render", "SDL_SetGPUSwapchainParameters({}) failed: {}", presentModeName(want), SDL_GetError());
  }
  // A refused mode leaves SDL's default, VSYNC.
  const SDL_GPUPresentMode got = set ? want : SDL_GPU_PRESENTMODE_VSYNC;
  lucent::info("render",
               "swapchain present mode: {}{}",
               presentModeName(got),
               presentModeBlocksCaller(got) ? " (blocking: every present stalls the guest thread until vblank)" : "");
  swapchainFormat_ = SDL_GetGPUSwapchainTextureFormat(device, window_);
}

void RenderWindow::releaseDevice(SDL_GPUDevice *device) {
  if (window_ != nullptr) {
    SDL_ReleaseWindowFromGPUDevice(device, window_);
  }
}

void RenderWindow::destroy() {
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  swapchainFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
}

present::PixelSize RenderWindow::pixelSize() const {
  int width = 0;
  int height = 0;
  if (window_ != nullptr) {
    SDL_GetWindowSizeInPixels(window_, &width, &height);
  }
  return {width > 0 ? width : 320, height > 0 ? height : 240};
}

double RenderWindow::refreshPeriodMs() const {
  if (window_ == nullptr) {
    return 0.0;
  }
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
  return mode != nullptr && mode->refresh_rate > 0.0f ? 1000.0 / static_cast<double>(mode->refresh_rate) : 0.0;
}

} // namespace psx::render
