#include "picture_announce.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_device.h"
#include "present_plan.h"

#include <lucent/log.h>

namespace psx::picture {

WideOutcome classifyWide(
    present::PresentationAspect aspect, bool titleProjects, bool recordPath, int nativeWidth, int renderWidth) {
  if (renderWidth > nativeWidth) {
    return WideOutcome::Widened;
  }
  if (aspect == present::PresentationAspect::Standard4x3) {
    return WideOutcome::NotRequested;
  }
  if (!titleProjects) {
    return WideOutcome::RefusedNoProjection;
  }
  if (!recordPath) {
    return WideOutcome::RefusedDevice;
  }
  // AUTO before the rest: it resolves to the sink, and the default window is 4:3.
  if (aspect == present::PresentationAspect::MatchSink) {
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
      psx::present::widePresentation(core), psx::present::widePresentationWidth(core), presentedFramebufferWidth);
  const present::GuestWidescreenProjection *projection = core.runtime.guestWidescreenProjection();
  const present::PresentationAspect aspect =
      projection != nullptr ? projection->presentationAspect(core) : present::presentationAspectFor(core.game->mods);
  const Geometry now{aspect, nativeWidth, renderWidth};
  if (now == core.rsub.announcedPicture) {
    return;
  }
  core.rsub.announcedPicture = now;
  lucent::info("wide",
               "native picture: aspect={} native_width={} render_width={}",
               static_cast<int>(now.aspect),
               now.nativeWidth,
               now.renderWidth);
  const WideOutcome outcome = classifyWide(now.aspect,
                                           projection != nullptr,
                                           core.rsub.mode.path() == present::RenderPath::Record,
                                           nativeWidth,
                                           renderWidth);
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
