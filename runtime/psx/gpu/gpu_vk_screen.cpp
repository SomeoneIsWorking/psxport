// gpu_vk_screen.cpp — bring the device up before a host-only screen, and route a Game's present to a pane.
#include "core.h"
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"

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
