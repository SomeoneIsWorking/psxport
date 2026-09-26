#include "picture_announce.h"

#include "core.h"
#include "game.h"
#include "gpu_vk.h"
#include "mods.h" // ASPECT_4_3 / ASPECT_AUTO — which aspect was actually asked for

#include <lucent/log.h>

namespace psx::picture {

WideOutcome classifyWide(int aspect, bool enhancementsAllowed, int nativeWidth, int renderWidth) {
  if (renderWidth > nativeWidth) {
    return WideOutcome::Widened;
  }
  if (aspect == ASPECT_4_3) {
    return WideOutcome::NotRequested; // nobody asked for a wide picture
  }
  if (!enhancementsAllowed) {
    return WideOutcome::RefusedPure;
  }
  if (aspect == ASPECT_AUTO) {
    return WideOutcome::RefusedAuto;
  }
  return WideOutcome::RefusedUnexplained;
}

const char *wideOutcomeReason(WideOutcome outcome) {
  switch (outcome) {
  case WideOutcome::NotRequested:
  case WideOutcome::Widened:
    return "";
  case WideOutcome::RefusedPure:
    return "this Core's render mode is PURE, where no PC enhancement may touch the picture";
  case WideOutcome::RefusedAuto:
    return "aspect=3 is ASPECT_AUTO, which resolves to the SINK's aspect, and this run has no wide "
           "sink to resolve to";
  case WideOutcome::RefusedUnexplained:
    return "a non-4:3 aspect was requested and allowed, but the render width did not exceed the "
           "native width";
  }
  return "";
}

void announceOnChange(Core &core) {
  const Geometry now{
      core.game->mods.aspect, gpu_vk_wide_engine(&core), (int)core.game->gpu.s_disp_w, gpu_vk_wide_engine_w(&core)};
  if (now == core.rsub.announcedPicture) {
    return;
  }
  core.rsub.announcedPicture = now;
  lucent::info("wide",
               "native picture: aspect={} wide_engine={} native_width={} render_width={}",
               now.aspect,
               now.wideEngine,
               now.nativeWidth,
               now.renderWidth);
  const WideOutcome outcome =
      classifyWide(now.aspect, core.rsub.mode.enhancementsAllowed(), now.nativeWidth, now.renderWidth);
  if (const char *why = wideOutcomeReason(outcome); why[0] != '\0') {
    lucent::warn("wide",
                 "a wide picture was REQUESTED and did not happen: render_width={} is not greater than "
                 "native_width={} because {}; any widescreen claim from this run is void",
                 now.renderWidth,
                 now.nativeWidth,
                 why);
  }
}

} // namespace psx::picture
