// render_window.cpp — RenderWindow.
#include "render_window.h"

#include "config_vars.h"
#include "present_mode.h"
#include "render_check.h"

#include <lucent/log.h>

#include <algorithm>

namespace psx::render {

void RenderWindow::create(bool shown, const char *title, present::PixelSize size, bool exactPixels) {
  // HIGH_PIXEL_DENSITY sizes the swapchain and the UI in physical pixels on a scaled display.
  SDL_WindowFlags flags = exactPixels ? 0 : SDL_WINDOW_HIGH_PIXEL_DENSITY;
  const SDL_PropertiesID props = SDL_CreateProperties();
  if (shown) {
    flags |= psx::config::fullscreen_requested() ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE;
  } else {
    // SDL drops controller input while no window has keyboard focus, and an unmapped window never takes it.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
      // SDL_GPU gives an SDL-hidden window no swapchain image; a roleless surface counts as shown to SDL and the
      // compositor never maps it.
      SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN, true);
    } else {
      flags |= SDL_WINDOW_HIDDEN;
    }
  }
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, size.width);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, size.height);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
  shown_ = shown;
  window_ = SDL_CreateWindowWithProperties(props);
  SDL_DestroyProperties(props);
  GPUCHK(window_, "SDL_CreateWindow");
  readRefreshPeriod();
  GPUCHK(SDL_AddEventWatch(&RenderWindow::watchDisplay, this), "SDL_AddEventWatch");
  const present::PixelSize pixels = pixelSize();
  lucent::info("render",
               "window {}x{} pixels {} (video driver {})",
               pixels.width,
               pixels.height,
               shown ? "shown" : ((flags & SDL_WINDOW_HIDDEN) != 0 ? "hidden" : "roleless, never mapped"),
               SDL_GetCurrentVideoDriver());
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

void RenderWindow::showError(const char *title, const std::string &message) const {
  std::string line = message;
  std::replace(line.begin(), line.end(), '\n', ' ');
  lucent::error("render", "{}: {}", title, line);
  if (shown_ && !SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message.c_str(), window_)) {
    lucent::warn("render", "the error box could not open: {}", SDL_GetError());
  }
}

void RenderWindow::releaseDevice(SDL_GPUDevice *device) {
  if (window_ != nullptr) {
    SDL_ReleaseWindowFromGPUDevice(device, window_);
  }
}

void RenderWindow::destroy() {
  if (window_ != nullptr) {
    SDL_RemoveEventWatch(&RenderWindow::watchDisplay, this);
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  refreshPeriodMs_ = 0.0;
  swapchainFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
}

present::PixelSize RenderWindow::pixelSize() const {
  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window_, &width, &height);
  return {width, height};
}

void RenderWindow::readRefreshPeriod() {
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
  refreshPeriodMs_ =
      mode != nullptr && mode->refresh_rate > 0.0f ? 1000.0 / static_cast<double>(mode->refresh_rate) : 0.0;
}

bool SDLCALL RenderWindow::watchDisplay(void *self, SDL_Event *event) {
  auto &owner = *static_cast<RenderWindow *>(self);
  const bool windowMoved =
      event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED && event->window.windowID == SDL_GetWindowID(owner.window_);
  if (windowMoved || event->type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) {
    owner.readRefreshPeriod();
  }
  return true;
}

} // namespace psx::render
