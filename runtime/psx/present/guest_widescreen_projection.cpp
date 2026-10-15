#include "guest_widescreen_projection.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "record_raster_setup.h"

#include <lucent/log.h>

#include <cstdlib>

PresentationAspect GuestWidescreenProjection::presentationAspect(const Core &core) const {
  if (core.game == nullptr) {
    return PresentationAspect::Standard4x3;
  }
  // ASPECT_AUTO is resolved against the live sink by the plan builder, so a headless run stays 4:3.
  switch (core.game->mods.aspect) {
  case ASPECT_4_3:
    return PresentationAspect::Standard4x3;
  case ASPECT_16_9:
    return PresentationAspect::Wide16x9;
  case ASPECT_21_9:
    return PresentationAspect::UltraWide21x9;
  case ASPECT_AUTO:
    return PresentationAspect::MatchSink;
  default:
    lucent::error("wide", "invalid aspect selector {}", core.game->mods.aspect);
    std::abort();
  }
}

bool GuestWidescreenProjection::guestCoordinatesWidened(const Core &) const {
  return false;
}

namespace {

PresentationAspect requestedAspect(const Core &core) {
  const GuestWidescreenProjection *projection = core.runtime.guestWidescreenProjection();
  if (projection == nullptr || core.rsub.mode.path() != RenderPath::Record) {
    return PresentationAspect::Standard4x3;
  }
  return projection->presentationAspect(core);
}

} // namespace

namespace psx::present {

bool widePresentation(const Core &core) {
  return core.rsub.mode.path() == RenderPath::Record && core.game->guestDisplay.plan().widescreen();
}

int widePresentationWidth(const Core &core) {
  return core.game->guestDisplay.plan().presentationExtent.width;
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

  Game *game = core.game;
  const psx::present::PixelSize sink = game->gpu_dev.sinkSize();
  const psx::gpu::DisplayArea area = core.gpuDevice().displayArea();
  const int nativeWidth = area.width > 0 ? area.width : WIDE_REFERENCE_NATIVE_W;
  const int nativeHeight = area.height > 0 ? area.height : PRESENT_NATIVE_LINES;
  GuestProjectionPlan plan = psx::present::guestProjectionPlan({
      .path = core.rsub.mode.path(),
      .requested = requestedAspect(core),
      .nativePresentation = {nativeWidth, nativeHeight},
      .nativeProjection = geometry,
      .sink = sink,
      .vramWidth = psx::gpu::kRecordVramWidth,
  });
  game->guestDisplay.latch(plan);
  return plan;
}

void unlatchGuestProjection(Core &core) {
  Game &game = *core.game;
  // The guest's OWN display mode, read now rather than taken from the caller's geometry: the caller
  // is publishing that it is not widening this frame, so the frame it is presenting is the one the
  // guest just published, whatever width that is.
  const psx::gpu::DisplayArea area = core.gpuDevice().displayArea();
  const int width = area.width > 0 ? area.width : WIDE_REFERENCE_NATIVE_W;
  const int height = area.height > 0 ? area.height : PRESENT_NATIVE_LINES;
  const psx::present::PixelSize sink = game.gpu_dev.sinkSize();
  game.guestDisplay.latch(psx::present::guestProjectionPlan({
      .path = core.rsub.mode.path(),
      .requested = PresentationAspect::Standard4x3,
      .nativePresentation = {width, height},
      .nativeProjection = {.extent = {width, height}, .drawWidth = width},
      .sink = sink,
      .vramWidth = psx::gpu::kRecordVramWidth,
  }));
}

GuestProjectionPlan latchRecordDisplay(Core &core, psx::present::PixelSize display) {
  const psx::present::PixelSize sink = core.game->gpu_dev.sinkSize();
  const GuestProjectionPlan plan = psx::present::guestProjectionPlan({
      .path = core.rsub.mode.path(),
      .requested = requestedAspect(core),
      .nativePresentation = display,
      .nativeProjection = {.extent = display, .drawWidth = display.width},
      .sink = sink,
      .vramWidth = psx::gpu::kRecordVramWidth,
  });
  core.game->guestDisplay.latch(plan);
  return plan;
}

} // namespace psx::present
