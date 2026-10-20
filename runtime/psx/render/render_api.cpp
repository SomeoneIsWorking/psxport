// render_api.cpp — the resolution plan the overlay, the menu and the record present read.
#include "core.h"
#include "game.h"
#include "guest_widescreen_projection.h"
#include "render.h"
#include "render_state.h"
#include "video_plan.h"

#include <algorithm>
#include <cmath>

namespace psx::render {
namespace {

// The internal-resolution cap: a 640 MiB budget at 14 bytes per pixel of VRAM scaled by i, which
// admits the 9 a 2160-line window asks for.
constexpr double kIresBudgetBytes = 640.0 * 1024.0 * 1024.0;
constexpr double kIresBytesPerPixel = 14.0;
constexpr int kIresCeiling = 8;

} // namespace

VideoStatus videoStatus(Core &core) {
  const double budgetPixels = kIresBudgetBytes / kIresBytesPerPixel;
  const int cap =
      std::clamp(static_cast<int>(std::sqrt(budgetPixels / (static_cast<double>(gpu::kVramWidth) * gpu::kVramHeight))),
                 1,
                 kIresCeiling);
  const PixelSize sink = core.game->render().presentImageSize();
  const int nativeWidth =
      present::widePresentation(core) ? present::widePresentationWidth(core) : present::nativeDisplaySize(core).width;
  const int ires =
      present::iresScale({.sinkW = sink.width, .sinkH = sink.height, .modsIres = core.game->mods.ires, .iresCap = cap});
  return {.nativeWidth = nativeWidth,
          .ires = ires,
          .framebufferWidth = nativeWidth * ires,
          .framebufferHeight = present::PRESENT_NATIVE_LINES * ires,
          .sinkWidth = sink.width,
          .sinkHeight = sink.height,
          .iresCap = cap};
}

} // namespace psx::render
