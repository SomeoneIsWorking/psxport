#include "picture_announce.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h" // GameRuntime::guestWidescreenProjection — the TITLE's own answer
#include "gpu_vk.h"
#include "guest_widescreen_projection.h" // GuestWidescreenProjection::presentationAspect
#include "mods.h"                        // ASPECT_4_3 / ASPECT_AUTO — which aspect was actually asked for
#include "present_plan.h"                // present_display_width — the presenter's OWN rule, not a second one

#include <lucent/log.h>

namespace psx::picture {

WideOutcome classifyWide(int aspect, bool enhancementsAllowed, int nativeWidth, int renderWidth, int guestAspect) {
  if (renderWidth > nativeWidth) {
    return WideOutcome::Widened;
  }
  // A guest-owned request is asked FIRST, before the host aspect, because it is a different owner and
  // reporting it as "nobody asked" would be false: the title asked, through its own projection.
  const bool guestWantsWide = guestAspect != ASPECT_4_3;
  if (aspect == ASPECT_4_3 && !guestWantsWide) {
    return WideOutcome::NotRequested; // nobody asked for a wide picture
  }
  // AUTO is tested BEFORE the render mode, and the order is the point. Measured 2026-09-26 on
  // Tekken 3: its `aspect=3` leg was told "this Core's render mode is PURE" when the real cause was
  // that AUTO resolves to the sink and a headless run's sink is 4:3. A reason that is true but not
  // the operative one sends the reader to the wrong knob.
  // AUTO is a HOST-side resolution, so it only decides a host-owned request.
  if (aspect == ASPECT_AUTO && !guestWantsWide) {
    return WideOutcome::RefusedAuto;
  }
  if (!enhancementsAllowed) {
    // Two different owners, two different reasons, and conflating them is what voided Crash 1's claim.
    return guestWantsWide ? WideOutcome::RefusedGuestPure : WideOutcome::RefusedPure;
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
  case WideOutcome::RefusedGuestPure:
    return "the title's OWN guest projection asked for a wider projection, and this Core's render mode "
           "is PURE, so the host presenter did not widen — which is a different question from whether "
           "the guest widened, and the guest's own published values are the evidence for that";
  case WideOutcome::RefusedAuto:
    return "aspect=3 is ASPECT_AUTO, which resolves to the SINK's aspect, and this run has no wide "
           "sink to resolve to";
  case WideOutcome::RefusedUnexplained:
    return "a non-4:3 aspect was requested and allowed, but the render width did not exceed the "
           "native width";
  }
  return "";
}

void announceOnChange(Core &core, int presentedFramebufferWidth) {
  // The native width is what the title's own 4:3 framebuffer is, and the render width is what the
  // presenter will actually draw — both through the presenter's own helpers, so this line cannot
  // disagree with the picture it describes.
  const int nativeWidth = (int)core.game->gpu.s_disp_w;
  const int renderWidth = present_display_width(
      gpu_vk_wide_presentation(&core) != 0, gpu_vk_wide_presentation_w(&core), presentedFramebufferWidth);
  const Geometry now{core.game->mods.aspect, gpu_vk_wide_engine(&core), nativeWidth, renderWidth};
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
  // The TITLE's own answer to "is this wide?", which is a different owner from the host aspect. A
  // title with no guest projection contributes ASPECT_4_3 and this behaves exactly as it did before.
  // The title's own answer is a PresentationAspect, and only its WIDE arms count as a request.
  // MatchSink is deliberately NOT one: it resolves to whatever the run's sink is, which is the same
  // trap ASPECT_AUTO has, and a headless run has no wide sink to resolve to.
  // The runtime is THIS Core's own snapshot, never the process's last-installed one: a process that
  // runs several sessions presents from all of them, and the global would answer with whichever
  // session was installed last.
  int guestAspect = ASPECT_4_3;
  if (const GameRuntime *runtime = core.runtime; runtime != nullptr) {
    if (const GuestWidescreenProjection *projection = runtime->guestWidescreenProjection()) {
      switch (projection->presentationAspect(core)) {
      case PresentationAspect::Wide16x9:
        guestAspect = ASPECT_16_9;
        break;
      case PresentationAspect::UltraWide21x9:
        guestAspect = ASPECT_21_9;
        break;
      case PresentationAspect::Standard4x3:
      case PresentationAspect::MatchSink:
        break;
      }
    }
  }
  const WideOutcome outcome =
      classifyWide(now.aspect, core.rsub.mode.enhancementsAllowed(), now.nativeWidth, now.renderWidth, guestAspect);
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
