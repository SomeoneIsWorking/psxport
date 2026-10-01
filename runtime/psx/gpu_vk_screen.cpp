// gpu_vk_screen.cpp — present a host screen (the title selector) as the whole picture.
//
// The screen is recorded into s_present_img, the image every present shot reads, and the window blit that
// follows must not draw an overlay pass over it: the screen is not an overlay (see RmlOverlay).
#include "core.h"
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "overlay_glue.h"

#include <lucent/log.h>

#include <cstdlib>

void GpuVkState::present_screen() {
  if (!gpu_vk_enabled()) {
    return;
  }
  GpuDevice &device = *GpuDevice::sInstance;
  init_gpu(game);
  overlay_glue_frame_begin(&game->core);
  int w = 0, h = 0;
  gpu_vk_present_sink_size(&w, &h);
  ensure_present_img(w, h);
  if (!s_present_img) {
    return;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device.s_dev);
  if (!cmd) {
    lucent::error("gpu_vk", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
    std::exit(2);
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = s_present_img;
  cti.clear_color = (SDL_FColor){0, 0, 0, 1};
  cti.load_op = SDL_GPU_LOADOP_CLEAR;
  cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  overlay_glue_record_screen(game, cmd, rp, s_present_img_w, s_present_img_h);
  SDL_EndGPURenderPass(rp);
  if (device.s_headless) {
    gpu_submit(cmd, "present_screen");
    return;
  }
  show_present_image(cmd, false); // consumes cmd
}

// Bring the window/device (and the windowed overlay) up now rather than on the first present, so a
// host-only screen can create its UI before its first frame. Idempotent per device AND per Game, so
// every session in a host-lifetime-window product can call it; no-op when the GPU is disabled.
void gpu_vk_ensure_device(Core *core) {
  if (gpu_vk_enabled()) {
    init_gpu(core ? core->game : nullptr);
  }
}

void gpu_vk_present_screen(Core *core) {
  core->game->gpu_vk.present_screen();
}
