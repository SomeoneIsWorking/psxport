// A guest-owned projection may widen a GTE- or record-path picture without opening the native-enhancement
// gate. The title supplies the aspect and changes its own projection/culling/layout; the framework
// only exposes the matching presentation extent. The PSX path remains the 4:3 reference.
#include "game.h"
#include "guest_widescreen_projection.h"
#include "mods.h"
#include "render_mode.h"
#include "testutil.h"

#include <memory>

namespace {

GuestProjectionPlan make_plan(RenderPath path,
                              PresentationAspect aspect,
                              int displayWidth,
                              int displayHeight,
                              int projectionWidth,
                              int projectionHeight,
                              int drawWidth) {
  return guest_projection_plan({
      .path = path,
      .requested = aspect,
      .nativePresentation = {displayWidth, displayHeight},
      .nativeProjection = {{projectionWidth, projectionHeight}, drawWidth},
      .sink = {1920, 1080},
      .vramWidth = 1024,
  });
}

void test_guest_projection_is_a_separate_permission_from_native_enhancements() {
  RenderMode mode;
  mode.setPath(RenderPath::Gte);
  CHECK(!mode.enhancementsAllowed());
  CHECK(mode.guestWidescreenAllowed());

  mode.setPath(RenderPath::Record);
  CHECK(!mode.enhancementsAllowed());
  CHECK(mode.guestWidescreenAllowed());

  mode.setPath(RenderPath::Device);
  CHECK(!mode.enhancementsAllowed());
  CHECK(!mode.guestWidescreenAllowed());

  mode.setPath(RenderPath::Native);
  CHECK(mode.enhancementsAllowed());
  CHECK(!mode.guestWidescreenAllowed());
}

void test_undeclared_or_reference_projection_stays_four_three() {
  const auto psx = make_plan(RenderPath::Device, PresentationAspect::Wide16x9, 320, 240, 320, 240, 320);
  const auto native = make_plan(RenderPath::Native, PresentationAspect::Wide16x9, 320, 240, 320, 240, 320);
  CHECK(!psx.widescreen());
  CHECK(!native.widescreen());
  CHECK_EQ((int)psx.aspect, (int)PresentationAspect::Standard4x3);
  CHECK_EQ((int)native.aspect, (int)PresentationAspect::Standard4x3);
}

void test_declared_guest_projection_drives_the_gte_presentation_extent() {
  const auto plan = make_plan(RenderPath::Gte, PresentationAspect::Wide16x9, 320, 240, 320, 240, 320);
  CHECK(plan.widescreen());
  CHECK_EQ(plan.nativeExtent.width, 320);
  CHECK_EQ(plan.nativeExtent.height, 240);
  CHECK_EQ(plan.presentationExtent.width, 428);
  CHECK_EQ(plan.presentationExtent.height, 240);
  CHECK_EQ(plan.presentationHorizontalMargin, 54);
  CHECK_EQ(plan.projectionHorizontalMargin, 54);
  CHECK_EQ(plan.projectionCenterX, 214);
  CHECK_EQ(plan.projectionClipRight, 427);
  CHECK_EQ(plan.guestDrawWidth, 428);
  CHECK_EQ(plan.guestClipRight, 427);
}

void test_declared_guest_projection_widens_the_record_canvas() {
  const auto wide = make_plan(RenderPath::Record, PresentationAspect::Wide16x9, 320, 240, 320, 240, 320);
  CHECK(wide.widescreen());
  CHECK_EQ(wide.presentationHorizontalMargin, 54);
  CHECK_EQ(wide.presentationExtent.width, 428);
  CHECK_EQ(wide.projectionCenterX, 160);
  CHECK_EQ(wide.guestDrawWidth, 320);
  const auto narrow = make_plan(RenderPath::Record, PresentationAspect::Standard4x3, 320, 240, 320, 240, 320);
  CHECK(!narrow.widescreen());
  CHECK_EQ(narrow.presentationHorizontalMargin, 0);
}

void test_four_three_is_identity_for_nonstandard_guest_framebuffers() {
  const GuestPresentationExtent extent =
      guest_presentation_extent(512, 256, 2560, 1440, PresentationAspect::Standard4x3, 1024);
  CHECK_EQ(extent.width, 512);
  CHECK_EQ(extent.height, 256);
}

void test_title_projection_extent_is_not_conflated_with_gp1_display_extent() {
  const auto plan = make_plan(RenderPath::Gte, PresentationAspect::Wide16x9, 368, 448, 384, 480, 368);
  CHECK_EQ(plan.nativeExtent.width, 368);
  CHECK_EQ(plan.presentationExtent.width, 492);
  CHECK_EQ(plan.presentationHorizontalMargin, 62);
  CHECK_EQ(plan.nativeProjectionExtent.width, 384);
  CHECK_EQ(plan.projectionExtent.width, 512);
  CHECK_EQ(plan.projectionHorizontalMargin, 64);
  CHECK_EQ(plan.projectionCenterX, 256);
  CHECK_EQ(plan.projectionClipRight, 511);
  CHECK_EQ(plan.nativeProjectionExtent.height, 480);
  CHECK_EQ(plan.projectionExtent.height, 480);
  CHECK_EQ(plan.nativeGuestDrawWidth, 368);
  CHECK_EQ(plan.guestDrawWidth, 492);
  CHECK_EQ(plan.guestDrawWidth % 2, 0);
  CHECK_EQ(plan.guestClipRight, 491);
}

void test_four_three_preserves_distinct_display_projection_and_draw_extents() {
  const auto plan = make_plan(RenderPath::Gte, PresentationAspect::Standard4x3, 368, 448, 384, 480, 368);
  CHECK_EQ(plan.presentationExtent.width, 368);
  CHECK_EQ(plan.projectionExtent.width, 384);
  CHECK_EQ(plan.guestDrawWidth, 368);
  CHECK_EQ(plan.presentationHorizontalMargin, 0);
  CHECK_EQ(plan.projectionHorizontalMargin, 0);
}

void test_latched_plan_is_stable_until_the_title_publishes_another_projection() {
  GuestPresentationState state;
  const auto wide = make_plan(RenderPath::Gte, PresentationAspect::Wide16x9, 320, 240, 320, 240, 320);
  state.latch(wide);
  CHECK_EQ(state.plan().presentationExtent.width, 428);

  // Merely computing a later policy result cannot change the frame the host will present.
  const auto narrow = make_plan(RenderPath::Gte, PresentationAspect::Standard4x3, 320, 240, 320, 240, 320);
  CHECK_EQ(narrow.presentationExtent.width, 320);
  CHECK_EQ(state.plan().presentationExtent.width, 428);
}

// The plan a title publishes when it is NOT widening (psxport's `gpu_vk_unlatch_guest_projection`).
// The guest has left the canvas the title widened — this title's front end runs 320x240 and 256x240
// screens while its resident frame is 512x240 — so every width in the plan is the guest's OWN
// display mode, both margins are zero, and `widescreen()` is false: the presenter samples the columns
// the guest authored and the letterbox centres them at their own aspect.
void test_retirement_plan_is_the_guests_own_display_mode() {
  const auto retired = make_plan(RenderPath::Gte,
                                 PresentationAspect::Standard4x3,
                                 /*display*/ 320,
                                 /*height*/ 240,
                                 /*projection*/ 320,
                                 240,
                                 /*draw*/ 320);
  CHECK_EQ(retired.presentationExtent.width, 320);
  CHECK_EQ(retired.projectionExtent.width, 320);
  CHECK_EQ(retired.guestDrawWidth, 320);
  CHECK_EQ(retired.presentationHorizontalMargin, 0);
  CHECK_EQ(retired.projectionHorizontalMargin, 0);
  CHECK_EQ(retired.guestClipLeft, 0);
  CHECK_EQ(retired.guestClipRight, 319);
  CHECK(!retired.widescreen());

  // And it replaces a latched wide plan: the state has no idea what "retired" means beyond being
  // the next publication, which is the whole contract the title's retire path relies on.
  GuestPresentationState state;
  state.latch(make_plan(RenderPath::Gte, PresentationAspect::Wide16x9, 512, 240, 512, 240, 512));
  CHECK(state.plan().widescreen());
  state.latch(retired);
  CHECK(!state.plan().widescreen());
  CHECK_EQ(state.plan().presentationExtent.width, 320);
}

void test_the_base_policy_maps_the_aspect_setting() {
  auto game = std::make_unique<Game>();
  const GuestWidescreenProjection policy;
  game->mods.aspect = ASPECT_4_3;
  CHECK(policy.presentationAspect(game->core) == PresentationAspect::Standard4x3);
  game->mods.aspect = ASPECT_16_9;
  CHECK(policy.presentationAspect(game->core) == PresentationAspect::Wide16x9);
  game->mods.aspect = ASPECT_21_9;
  CHECK(policy.presentationAspect(game->core) == PresentationAspect::UltraWide21x9);
  // Auto resolves against the sink later; folding it here would report wide on a 4:3 sink.
  game->mods.aspect = ASPECT_AUTO;
  CHECK(policy.presentationAspect(game->core) == PresentationAspect::MatchSink);
}

} // namespace

int main() {
  RUN(the_base_policy_maps_the_aspect_setting);
  RUN(guest_projection_is_a_separate_permission_from_native_enhancements);
  RUN(undeclared_or_reference_projection_stays_four_three);
  RUN(declared_guest_projection_drives_the_gte_presentation_extent);
  RUN(declared_guest_projection_widens_the_record_canvas);
  RUN(four_three_is_identity_for_nonstandard_guest_framebuffers);
  RUN(title_projection_extent_is_not_conflated_with_gp1_display_extent);
  RUN(four_three_preserves_distinct_display_projection_and_draw_extents);
  RUN(latched_plan_is_stable_until_the_title_publishes_another_projection);
  RUN(retirement_plan_is_the_guests_own_display_mode);
  return pt_summary();
}
