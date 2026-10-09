#include "guest_widescreen_projection.h"

#include "core.h"
#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"

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
  const GameRuntime *runtime = core.game->runtime;
  const GuestWidescreenProjection *projection = runtime ? runtime->guestWidescreenProjection() : nullptr;
  if (projection == nullptr || !core.rsub.mode.guestWidescreenAllowed()) {
    return PresentationAspect::Standard4x3;
  }
  return projection->presentationAspect(core);
}

} // namespace

int gpu_vk_wide_presentation(Core *core) {
  if (gpu_vk_wide_engine(core)) {
    return 1;
  }
  return core->rsub.mode.guestWidescreenAllowed() && core->game->guestDisplay.plan().widescreen();
}

int gpu_vk_wide_presentation_w(Core *core) {
  if (gpu_vk_wide_engine(core)) {
    return gpu_vk_wide_engine_w(core);
  }
  return core->game->guestDisplay.plan().presentationExtent.width;
}

GuestProjectionPlan gpu_vk_latch_guest_projection(Core *core, GuestProjectionGeometry geometry) {
  if (!core || !core->game || !geometry.valid()) {
    lucent::error("wide",
                  "guest projection latch requires a bound Core/Game and positive projection/draw "
                  "geometry ({}x{}, draw={})",
                  geometry.extent.width,
                  geometry.extent.height,
                  geometry.drawWidth);
    std::abort();
  }

  Game *game = core->game;
  int sinkWidth = 0;
  int sinkHeight = 0;
  gpu_vk_present_sink_size(&sinkWidth, &sinkHeight);
  const int nativeWidth = game->gpu.s_disp_w > 0 ? game->gpu.s_disp_w : WIDE_REFERENCE_NATIVE_W;
  const int nativeHeight = game->gpu.s_disp_h > 0 ? game->gpu.s_disp_h : PRESENT_NATIVE_LINES;
  GuestProjectionPlan plan = guest_projection_plan({
      .path = core->rsub.mode.path(),
      .requested = requestedAspect(*core),
      .nativePresentation = {nativeWidth, nativeHeight},
      .nativeProjection = geometry,
      .sink = {sinkWidth, sinkHeight},
      .vramWidth = VRAM_W,
  });
  game->guestDisplay.latch(plan);
  return plan;
}

void gpu_vk_unlatch_guest_projection(Core &core) {
  Game &game = *core.game;
  // The guest's OWN display mode, read now rather than taken from the caller's geometry: the caller
  // is publishing that it is not widening this frame, so the frame it is presenting is the one the
  // guest just published, whatever width that is.
  const int width = game.gpu.s_disp_w > 0 ? game.gpu.s_disp_w : WIDE_REFERENCE_NATIVE_W;
  const int height = game.gpu.s_disp_h > 0 ? game.gpu.s_disp_h : PRESENT_NATIVE_LINES;
  int sinkWidth = 0;
  int sinkHeight = 0;
  gpu_vk_present_sink_size(&sinkWidth, &sinkHeight);
  game.guestDisplay.latch(guest_projection_plan({
      .path = core.rsub.mode.path(),
      .requested = PresentationAspect::Standard4x3,
      .nativePresentation = {width, height},
      .nativeProjection = {.extent = {width, height}, .drawWidth = width},
      .sink = {sinkWidth, sinkHeight},
      .vramWidth = VRAM_W,
  }));
}

GuestProjectionPlan gpu_vk_latch_record_display(Core &core, GuestPresentationExtent display) {
  int sinkWidth = 0;
  int sinkHeight = 0;
  gpu_vk_present_sink_size(&sinkWidth, &sinkHeight);
  const GuestProjectionPlan plan = guest_projection_plan({
      .path = core.rsub.mode.path(),
      .requested = requestedAspect(core),
      .nativePresentation = display,
      .nativeProjection = {.extent = display, .drawWidth = display.width},
      .sink = {sinkWidth, sinkHeight},
      .vramWidth = VRAM_W,
  });
  core.game->guestDisplay.latch(plan);
  return plan;
}
