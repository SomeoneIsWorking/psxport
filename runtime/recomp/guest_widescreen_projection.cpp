#include "guest_widescreen_projection.h"

#include "core.h"
#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"

#include <lucent/log.h>

#include <cstdlib>

int gpu_vk_wide_presentation(Core *core) {
  if (gpu_vk_wide_engine(core)) {
    return 1;
  }
  const Game *game = core->game;
  return core->rsub.mode.guestWidescreenAllowed() && !game->oracle && !game->sbs &&
         game->guestDisplay.plan().widescreen();
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
  PresentationAspect requested = PresentationAspect::Standard4x3;
  const GuestWidescreenProjection *projection = game->runtime ? game->runtime->guestWidescreenProjection() : nullptr;
  if (projection && core->rsub.mode.guestWidescreenAllowed() && !game->oracle && !game->sbs) {
    requested = projection->presentationAspect(*core);
  }

  GuestProjectionPlan plan = guest_projection_plan({
      .path = core->rsub.mode.path(),
      .requested = requested,
      .nativePresentation = {nativeWidth, nativeHeight},
      .nativeProjection = geometry,
      .sink = {sinkWidth, sinkHeight},
      .vramWidth = VRAM_W,
  });
  game->guestDisplay.latch(plan);
  return plan;
}
