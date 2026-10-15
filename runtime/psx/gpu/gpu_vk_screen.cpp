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
  if (!device->enabled()) {
    return;
  }
  bring_up();
  psx::ui::beginOverlayFrame(&game->core);
  const psx::present::PixelSize sink = device->sinkSize();
  ensure_present_img(sink.width, sink.height);
  if (!s_present_img) {
    return;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
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
  psx::ui::recordOverlayScreen(game, cmd, rp, s_present_img_w, s_present_img_h);
  SDL_EndGPURenderPass(rp);
  if (device->s_headless != 0) {
    device->submit(cmd, "present_screen");
    return;
  }
  show_present_image(cmd, false); // consumes cmd
}

// Bring the window/device (and the windowed overlay) up now rather than on the first present, so a
// host-only screen can create its UI before its first frame. Idempotent per device AND per Game, so
// every session in a host-lifetime-window product can call it; no-op when the GPU is disabled.
namespace psx::gpu {

void ensureDevice(Core &core) {
  if (core.game->gpu_dev.enabled()) {
    core.game->gpuVk().bring_up();
  }
}

} // namespace psx::gpu

// Route this Game's present somewhere other than the window.
//
// The ordinary product never calls these: its present IS the window (see show_present_image). A host
// running SEVERAL sessions at once calls the first, and every session's present then builds its
// picture and keeps it in its own present image instead of blitting it over the others — which is the
// only way several guests can share one window, because the window belongs to the host
// (psxport::HostPresentation) and each session's composite is its own.
//
// `image` is the size that session should build its picture at, which is the
// host's business: a panel is a fraction of the window, and a full-window image per session is memory
// nobody asked for. Zero keeps the sink's size.
void GpuVkState::presentToPane(psx::present::PixelSize image) {
  s_present_img_requested = image;
  setPresentTarget(PresentTarget::Pane);
}

void GpuVkState::presentToWindow() {
  s_present_img_requested = {};
  setPresentTarget(PresentTarget::Swapchain);
}
