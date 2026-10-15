#include "picture_announce.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h" // GameRuntime::guestWidescreenProjection — the TITLE's own answer
#include "gpu_device.h"
#include "gpu_vk.h"
#include "guest_widescreen_projection.h" // GuestWidescreenProjection::presentationAspect
#include "mods.h"                        // ASPECT_4_3 / ASPECT_AUTO
#include "present_plan.h"                // present_display_width — the presenter's OWN rule, not a second one

#include <lucent/log.h>

namespace psx::picture {

WideOutcome classifyWide(int aspect, bool titleProjects, bool recordPath, int nativeWidth, int renderWidth) {
  if (renderWidth > nativeWidth) {
    return WideOutcome::Widened;
  }
  if (aspect == ASPECT_4_3) {
    return WideOutcome::NotRequested;
  }
  if (!titleProjects) {
    return WideOutcome::RefusedNoProjection;
  }
  if (!recordPath) {
    return WideOutcome::RefusedDevice;
  }
  // AUTO before the rest: it resolves to the sink, and a headless sink is 4:3.
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
  case WideOutcome::RefusedNoProjection:
    return "this title declares no guest widescreen projection";
  case WideOutcome::RefusedDevice:
    return "RenderPath::Device presents the GPU device's own 4:3 picture";
  case WideOutcome::RefusedAuto:
    return "aspect=3 is ASPECT_AUTO, which resolves to the SINK's aspect, and this run has no wide sink to "
           "resolve to";
  case WideOutcome::RefusedUnexplained:
    return "a non-4:3 aspect was requested and possible, but the render width did not exceed the native width";
  }
  return "";
}

void announceOnChange(Core &core, int presentedFramebufferWidth) {
  const int nativeWidth = core.gpuDevice().displayArea().width;
  const int renderWidth = psx::present::presentDisplayWidth(
      psx::present::widePresentation(core) != 0, psx::present::widePresentationWidth(core), presentedFramebufferWidth);
  // The title's own answer when it has a guest projection; MatchSink is ASPECT_AUTO's trap.
  const GuestWidescreenProjection *projection = core.runtime.guestWidescreenProjection();
  int aspect = core.game->mods.aspect;
  if (projection != nullptr) {
    switch (projection->presentationAspect(core)) {
    case PresentationAspect::Standard4x3:
      aspect = ASPECT_4_3;
      break;
    case PresentationAspect::Wide16x9:
      aspect = ASPECT_16_9;
      break;
    case PresentationAspect::UltraWide21x9:
      aspect = ASPECT_21_9;
      break;
    case PresentationAspect::MatchSink:
      aspect = ASPECT_AUTO;
      break;
    }
  }
  const Geometry now{aspect, nativeWidth, renderWidth};
  if (now == core.rsub.announcedPicture) {
    return;
  }
  core.rsub.announcedPicture = now;
  lucent::info("wide",
               "native picture: aspect={} native_width={} render_width={}",
               now.aspect,
               now.nativeWidth,
               now.renderWidth);
  const WideOutcome outcome = classifyWide(
      now.aspect, projection != nullptr, core.rsub.mode.path() == RenderPath::Record, nativeWidth, renderWidth);
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
