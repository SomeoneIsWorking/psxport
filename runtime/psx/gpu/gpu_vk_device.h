// gpu_vk_device.h — class GpuDevice: the SDL3 GPU device, its window, and the samplers and present
// pipelines every Game presenting through it shares. A Game owns one or presents through a host's
// (`Game::gpu_dev`); per-Game targets live on GpuVkState (gpu_vk_internal.h).
#pragma once
#include "gpu_vk_window.h" // GpuWindow — the window + swapchain (its own lifetime, not a field here)
#include "pixel_size.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// The presented-picture format: fixed so it exists identically windowed and headless.
inline constexpr SDL_GPUTextureFormat kPresentImageFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

class GpuDevice {
public:
  GpuDevice() = default;
  GpuDevice(const GpuDevice &) = delete;
  GpuDevice &operator=(const GpuDevice &) = delete;
  // Releases the window, device, samplers and pipelines bringUp() created, when it ran.
  ~GpuDevice();

  // A disabled device presents nothing; a test disables it before the first present.
  bool enabled() const {
    return enabled_;
  }
  void disable() {
    enabled_ = false;
  }
  // Is a live on-screen window up (or about to be)? Resolves the leg from config on the first ask.
  bool windowed();
  // Window, device, samplers and present pipelines. Idempotent; exits when no device can be created.
  void bringUp();
  // The present sink: the live drawable windowed, PSXPORT_PRESENT_SINK (default the window size) headless.
  psx::present::PixelSize sinkSize();

  // Submit, latching the device off for the rest of the process on the first failure.
  bool submit(SDL_GPUCommandBuffer *cmd, const char *where);
  // Submit and wait with a wall-clock bound; false (and latched) on failure or timeout.
  bool submitAndWait(SDL_GPUCommandBuffer *cmd, const char *where);
  bool faulted() const {
    return faulted_;
  }

  int s_inited = 0;
  int s_headless = 0;
  SDL_GPUDevice *s_dev = nullptr;
  // The window's title when the host that owns the window named it; null names it generically.
  const char *s_window_title = nullptr;
  GpuWindow s_window;

  SDL_GPUSampler *s_samp_nearest = nullptr; // integer VRAM sampler (nearest)
  SDL_GPUSampler *s_samp_linear = nullptr;  // RGBA image sampler (linear)
  SDL_GPUGraphicsPipeline *s_present_pipe = nullptr;
  SDL_GPUGraphicsPipeline *s_image_pipe = nullptr;

  // The fullscreen image present's source (RGBA8, recreated on size change).
  SDL_GPUTexture *s_img_tex = nullptr;
  SDL_GPUTransferBuffer *s_img_xfer = nullptr;
  int s_img_w = 0, s_img_h = 0;
  void ensureImageTexture(int width, int height);

private:
  // Record the fault for the next process; refuse to start while one stands.
  void persistFault(const char *where, const char *detail);
  bool faultPreflight();
  void latchFault(const char *where, const char *detail);

  bool enabled_ = true;
  int legResolved_ = 0;
  bool faulted_ = false;
  int sinkW_ = 0, sinkH_ = 0; // headless sink, resolved once
};
