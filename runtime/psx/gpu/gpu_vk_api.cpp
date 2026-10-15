// gpu_vk_api.cpp — the Core-facing entry points (gpu_vk.h) over the calling Game's GpuVkState, and the
// resolution plan the overlay and the record present read.
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "overlay_glue.h"
#include "picture_announce.h"
#include "video_plan.h"

#include <cmath>

namespace {

// The internal-resolution cap: a 128 MiB budget at 14 bytes per pixel of VRAM scaled by i.
constexpr double kIresBudgetBytes = 128.0 * 1024.0 * 1024.0;
constexpr double kIresBytesPerPixel = 14.0;
constexpr int kIresCeiling = 8;

} // namespace

namespace psx::gpu {

VideoStatus videoStatus(Core &c) {
  const double budgetPixels = kIresBudgetBytes / kIresBytesPerPixel;
  int cap = static_cast<int>(
      std::sqrt(budgetPixels / (static_cast<double>(psx::gpu::kRecordVramWidth) * psx::gpu::kRecordVramHeight)));
  cap = cap < 1 ? 1 : (cap > kIresCeiling ? kIresCeiling : cap);
  VideoInputs v;
  const psx::present::PixelSize sink = c.game->gpu_dev.sinkSize();
  v.sinkW = sink.width;
  v.sinkH = sink.height;
  v.modsIres = c.game->mods.ires;
  v.iresCap = cap;
  const int displayWidth = c.gpuDevice().displayArea().width;
  const int nw = psx::present::widePresentation(c) ? c.game->guestDisplay.plan().presentationExtent.width
                                                   : (displayWidth > 0 ? displayWidth : WIDE_REFERENCE_NATIVE_W);
  const int i = psx::present::iresScale(v);
  return {.nativeWidth = nw,
          .ires = i,
          .framebufferWidth = nw * i,
          .framebufferHeight = PRESENT_NATIVE_LINES * i,
          .sinkWidth = v.sinkW,
          .sinkHeight = v.sinkH,
          .iresCap = cap};
}

void present(Core &core, int sx, int sy, int w, int h) {
  psx::ui::beginOverlayFrame(&core);
  psx::picture::announceOnChange(core, w);
  core.game->gpuVk().present(sx, sy, w, h);
}

void repaint(Core &core) {
  psx::ui::beginOverlayFrame(&core); // the overlay stays interactive while the game is frozen
  core.game->gpuVk().repaint();
}

} // namespace psx::gpu
