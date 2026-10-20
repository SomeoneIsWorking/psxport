#include "guest_widescreen_projection.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "mods.h"
#include "render_device.h"
#include "vram_rect.h"

#include <lucent/log.h>

#include <cstdlib>

namespace psx::present {

PresentationAspect presentationAspectFor(const Mods &mods) {
  // ASPECT_AUTO is resolved against the live sink by the plan builder, so a headless run stays 4:3.
  switch (mods.aspect) {
  case ASPECT_4_3:
  case ASPECT_16_9:
  case ASPECT_21_9:
  case ASPECT_AUTO:
    return static_cast<PresentationAspect>(mods.aspect);
  default:
    lucent::error("wide", "invalid aspect selector {}", mods.aspect);
    std::abort();
  }
}

PresentationAspect GuestWidescreenProjection::presentationAspect(const Core &core) const {
  return presentationAspectFor(core.game->mods);
}

namespace {

PresentationAspect requestedAspect(const Core &core) {
  const GuestWidescreenProjection *projection = core.runtime.guestWidescreenProjection();
  if (projection == nullptr || core.rsub.mode.path() != RenderPath::Record) {
    return PresentationAspect::Standard4x3;
  }
  return projection->presentationAspect(core);
}

GuestProjectionPlan latch(Core &core, PixelSize nativePresentation, GuestProjectionGeometry nativeProjection) {
  const GuestProjectionPlan plan = guestProjectionPlan({
      .path = core.rsub.mode.path(),
      .requested = requestedAspect(core),
      .nativePresentation = nativePresentation,
      .nativeProjection = nativeProjection,
      .sink = core.game->renderDevice.sinkSize(),
      .vramWidth = gpu::kVramWidth,
  });
  core.game->guestDisplay.latch(plan);
  return plan;
}

} // namespace

bool widePresentation(const Core &core) {
  return core.rsub.mode.path() == RenderPath::Record && core.game->guestDisplay.plan().widescreen();
}

int widePresentationWidth(const Core &core) {
  return core.game->guestDisplay.plan().presentationExtent.width;
}

PixelSize nativeDisplaySize(Core &core) {
  const gpu::DisplayArea area = core.gpuDevice().displayArea();
  return {area.width > 0 ? area.width : WIDE_REFERENCE_NATIVE_W, area.height > 0 ? area.height : PRESENT_NATIVE_LINES};
}

GuestProjectionPlan latchGuestProjection(Core &core, GuestProjectionGeometry geometry) {
  if (core.game == nullptr || !geometry.valid()) {
    lucent::error("wide",
                  "guest projection latch requires a bound Core/Game and positive projection/draw "
                  "geometry ({}x{}, draw={})",
                  geometry.extent.width,
                  geometry.extent.height,
                  geometry.drawWidth);
    std::abort();
  }
  return latch(core, nativeDisplaySize(core), geometry);
}

GuestProjectionPlan latchRecordDisplay(Core &core, PixelSize display) {
  return latch(core, display, {.extent = display, .drawWidth = display.width});
}

} // namespace psx::present
