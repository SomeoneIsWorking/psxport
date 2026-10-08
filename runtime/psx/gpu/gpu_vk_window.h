// gpu_vk_window.h — class GpuWindow: the ONE SDL window and its GPU swapchain, for the whole product run.
//
// WHY THIS IS ITS OWN OWNER. The window is not a field of the device: it is created BEFORE the
// SDL_GPU device exists (SDL_CreateWindow, then SDL_CreateGPUDevice, then the claim), it outlives every
// session that presents into it, and the process is required to have exactly one (psxport::
// HostPresentation exists precisely to decide when that one is released). Its creation flags, its
// identity, its swapchain claim and its live drawable extent are therefore one lifetime, not four
// lines inside the renderer's device bring-up.
//
// WHAT IT DOES NOT OWN. The window's NAME is decided by the caller and passed in: naming a window after
// the first Game that presents is a HOST policy (HostPresentation::setWindowTitle), not a renderer
// decision. Destruction order is the device owner's: the window must leave the device before the device
// is destroyed, which is why these are explicit steps and not a destructor.
//
// The window is an OUTPUT SINK, never a rendering stage: nothing here decides what the picture looks
// like, and nothing here reads the guest.
#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// The window's creation size, defined ONCE and used both at SDL_CreateWindow and as the headless sink
// default, so the two cannot drift. (They were two hand-copied 960x720 literals; changing the window
// would silently have desynchronised the headless sink from it.)
enum { PRESENT_WINDOW_W = 960, PRESENT_WINDOW_H = 720 };

class GpuWindow {
public:
  GpuWindow() = default;
  GpuWindow(const GpuWindow &) = delete;
  GpuWindow &operator=(const GpuWindow &) = delete;

  // Open the window, unless this leg is headless (which creates no window at all). `title` is the
  // already-resolved identity the host or the presenting Game declared.
  void create(bool headless, const char *title);

  // Claim the open window for `dev` and settle its swapchain: a present mode that does NOT stall the
  // guest thread, and the swapchain's texture format. Windowed only, after the device exists.
  void claim(SDL_GPUDevice *dev);

  // Step 1 of teardown: leave the device. Step 2 is `destroy`, after the device itself is gone.
  void release_device(SDL_GPUDevice *dev);
  void destroy();

  SDL_Window *window() const {
    return s_win;
  }
  // Invalid headless, where there is no swapchain at all — the presented picture's own format is
  // leg-independent, so the renderer never needs one there.
  SDL_GPUTextureFormat swapchain_format() const {
    return s_swap_fmt;
  }

  // The live drawable extent in pixels. Falls back to 320x240 when there is no window, so these answer
  // "how big is the SINK" in the windowed leg ONLY (see gpu_vk_present_sink_size).
  int pixel_width() const;
  int pixel_height() const;

private:
  SDL_Window *s_win = nullptr;
  SDL_GPUTextureFormat s_swap_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
};