#include "gpu_vk_window.h"
#include "cfg.h"                 // PSXPORT_FULLSCREEN / PSXPORT_WINDOWED — the window's creation flags
#include "gpu_vk_check.h"        // GPUCHK — a null window/device handle ends the run
#include "gpu_vk_present_mode.h" // preferred_present_mode — the swapchain must not stall the guest thread
#include <lucent/log.h>

// ONE window for the whole product run. The device comes up around it (create -> device -> claim) and
// goes down around it (leave device -> device -> destroy); see gpu_vk_device.cpp for the teardown.

void GpuWindow::create(bool headless, const char *title) {
  if (headless) {
    return; // an output sink nobody shows: the same picture is composed headless (see init_gpu_device)
  }
  const int fullscreen =
      cfg_on("PSXPORT_FULLSCREEN") || (cfg_str("PSXPORT_WINDOWED") && atoi(cfg_str("PSXPORT_WINDOWED")) == 0);
  // HIGH_PIXEL_DENSITY: on a scaled display the swapchain, the internal resolution and the UI are
  // sized in physical pixels; without it they are sized in points and the compositor upscales them.
  const SDL_WindowFlags flags =
      SDL_WINDOW_HIGH_PIXEL_DENSITY | (fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE);
  s_win = SDL_CreateWindow(title, PRESENT_WINDOW_W, PRESENT_WINDOW_H, flags);
  GPUCHK(s_win, "SDL_CreateWindow");
}

void GpuWindow::claim(SDL_GPUDevice *dev) {
  GPUCHK(SDL_ClaimWindowForGPUDevice(dev, s_win), "SDL_ClaimWindowForGPUDevice");
  // The swapchain must NOT stall the guest thread. A freshly claimed window keeps SDL's DEFAULT
  // present mode, VSYNC, under which SDL_WaitAndAcquireGPUSwapchainTexture (show_present_image) sleeps
  // until the next vblank — on the one thread that runs the guest, the CD pump, MDEC and the DMA
  // completions. Ask for a non-blocking mode instead; see gpu_vk_present_mode.h for the measurement.
  const SDL_GPUPresentMode want =
      preferred_present_mode(SDL_WindowSupportsGPUPresentMode(dev, s_win, SDL_GPU_PRESENTMODE_MAILBOX),
                             SDL_WindowSupportsGPUPresentMode(dev, s_win, SDL_GPU_PRESENTMODE_IMMEDIATE));
  const bool set_ok = SDL_SetGPUSwapchainParameters(dev, s_win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, want);
  if (!set_ok) {
    lucent::warn("gpu_vk", "SDL_SetGPUSwapchainParameters({}) failed: {}", present_mode_name(want), SDL_GetError());
  }
  // The mode actually IN EFFECT, not the one asked for: on failure the swapchain keeps SDL's default,
  // which is VSYNC. Unguarded info — a normal windowed run must state whether its sink blocks.
  const SDL_GPUPresentMode got = set_ok ? want : SDL_GPU_PRESENTMODE_VSYNC;
  lucent::info("gpu_vk",
               "swapchain present mode: {}{}",
               present_mode_name(got),
               present_mode_blocks_caller(got) ? " (BLOCKING — every present stalls the guest thread until vblank)"
                                               : "");
  s_swap_fmt = SDL_GetGPUSwapchainTextureFormat(dev, s_win);
}

void GpuWindow::release_device(SDL_GPUDevice *dev) {
  if (s_win != nullptr) {
    SDL_ReleaseWindowFromGPUDevice(dev, s_win);
  }
}

void GpuWindow::destroy() {
  if (s_win != nullptr) {
    SDL_DestroyWindow(s_win);
    s_win = nullptr;
  }
  s_swap_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
}

int GpuWindow::pixel_width() const {
  int w = 320, h = 240;
  if (s_win) {
    SDL_GetWindowSizeInPixels(s_win, &w, &h);
  }
  return w > 0 ? w : 320;
}

int GpuWindow::pixel_height() const {
  int w = 320, h = 240;
  if (s_win) {
    SDL_GetWindowSizeInPixels(s_win, &w, &h);
  }
  return h > 0 ? h : 240;
}