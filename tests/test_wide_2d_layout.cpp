// test_wide_2d_layout — the 2D layout rule, asked of a title that widened through the GUEST.
//
// `rq_2d_xform` (render_queue.h) is the rule and has its own hermetic test; this is the QUESTION its
// one application site asked before applying it. `RenderQueue::emitOrQueue` gated the rule on
// `gpu_vk_wide_engine`, which is
//
//     Mods::aspect != ASPECT_4_3 && RenderMode::enhancementsAllowed()
//
// and `enhancementsAllowed()` is `path == RenderPath::Native`. Every widescreen-only title declares
// `RenderCapabilities::widescreenOnly()`, whose default path is Gte, so on those titles the
// conjunction was FALSE ON EVERY FRAME and the rule never ran.
//
// Measured on Mega Man X4 (docs/issues/0019 in that repository), which is what named the cause: its
// all-2D 320-wide composition begins at host x=0 while 4:3 content begins at x=164, and that frame's
// primitive dump is 635 sprites, 635/635 marked 2D and 0/635 marked 3D, so moving the projection
// centre (OFX) moves none of that screen.
//
// This drives the shipping `wide_2d_extent` / `wide_2d_layout_active_for` over the four facts a Core
// would report, at the geometries that were affected. The Core-facing overload only gathers those
// facts, so the decision under test is the one the product makes.

#include "../runtime/psx/render_queue.h"
#include "../runtime/psx/wide_2d_layout.h"
#include "testutil.h"

namespace {

// A Core's four facts, named the way the framework names them.
struct Core4x3 {
  int host_wide;
  bool host_engaged;
  int guest_wide;
  bool guest_engaged;
  int native;
};

Wide2dExtent extent_of(const Core4x3 &core) {
  return wide_2d_extent(core.host_wide, core.host_engaged, core.guest_wide, core.guest_engaged, core.native);
}

bool active_of(const Core4x3 &core) {
  return wide_2d_layout_active_for(core.host_wide, core.host_engaged, core.guest_wide, core.guest_engaged, core.native);
}

} // namespace

// THE CASE THAT WAS BROKEN. A Gte-path title widens through its own latched guest projection and NOT
// through the host engine, which is the conjunction that used to be asked.
static void test_a_guest_projection_widening_makes_the_rule_active(void) {
  // Mega Man X4: 320 -> 428, host engine NOT engaged (its path is Gte).
  const Core4x3 x4{/*host_wide=*/0,
                   /*host_engaged=*/false,
                   /*guest_wide=*/428,
                   /*guest_engaged=*/true,
                   /*native=*/320};
  CHECK(active_of(x4));
  CHECK_EQ(extent_of(x4).wide, 428);
  CHECK_EQ(extent_of(x4).native, 320);
  // The margin the rule then applies is +54, which is what X4's own tables call for and what
  // tests/test_rq_widen_2d.cpp:61 already pins for these numbers.
  CHECK_EQ((extent_of(x4).wide - extent_of(x4).native) / 2, 54);
  // Tekken 3: 368 -> 492, same shape, and its 4:3 is 368 rather than 320.
  const Core4x3 tekken{0, false, 492, true, 368};
  CHECK(active_of(tekken));
  CHECK_EQ((extent_of(tekken).wide - extent_of(tekken).native) / 2, 62);
}

// The mechanism that already worked must keep working. Every widescreen measurement in the workspace
// so far came from a native-render title, so a regression here would be the expensive kind.
static void test_a_host_engine_widening_still_makes_the_rule_active(void) {
  const Core4x3 spyro{/*host_wide=*/684,
                      /*host_engaged=*/true,
                      /*guest_wide=*/0,
                      /*guest_engaged=*/false,
                      /*native=*/512};
  CHECK(active_of(spyro));
  CHECK_EQ(extent_of(spyro).wide, 684);
  CHECK_EQ((extent_of(spyro).wide - extent_of(spyro).native) / 2, 86);
}

// 4:3 must be quiet, or every ordinary frame in every title pays a transform it does not need.
static void test_four_three_three_is_not_active(void) {
  CHECK_EQ(active_of(Core4x3{0, false, 0, false, 320}), false);
  CHECK_EQ(active_of(Core4x3{320, false, 0, false, 512}), false);
  // At 4:3 the extent reports the title's own width on both sides, which is what makes the identity
  // the rule's own answer rather than a special case here.
  const Wide2dExtent extent = extent_of(Core4x3{0, false, 0, false, 320});
  CHECK_EQ(extent.wide, extent.native);
}

// A plan that resolved NARROW is not a widening. This is the case that could have produced a false
// positive: ASPECT_AUTO against a 4:3 sink leaves the guest plan un-widened, and a predicate that
// only asked "is a plan latched?" would lay the 2D layer out for nothing.
static void test_a_plan_that_resolved_narrow_is_not_a_widening(void) {
  CHECK_EQ(active_of(Core4x3{0, false, /*guest_wide=*/320, true, /*native=*/320}), false);
  CHECK_EQ(active_of(Core4x3{0, false, /*guest_wide=*/368, true, /*native=*/368}), false);
  // A plan engaged with a width BELOW the title's own is not a widening either; it would otherwise
  // produce a negative margin and shift the 2D layer the wrong way.
  CHECK_EQ(active_of(Core4x3{0, false, /*guest_wide=*/256, true, /*native=*/320}), false);
}

// The two mechanisms are ALTERNATIVES. A title either renders natively or it is a Gte-path title, and
// the owner takes the host first — so a Core that somehow has both engaged is widened once, by the
// host, and never by the sum. This is the double-shift hazard stated as a case.
static void test_the_mechanisms_are_alternatives_not_a_sum(void) {
  const Core4x3 both{/*host_wide=*/684, true, /*guest_wide=*/428, true, /*native=*/512};
  CHECK(active_of(both));
  CHECK_EQ(extent_of(both).wide, 684);            // the host's width, chosen
  CHECK(extent_of(both).wide != 684 + 428 - 512); // and emphatically not a sum
  // With the host not engaged, the guest is the fallback — which is the X4 shape again, from the same
  // inputs with one flag flipped.
  const Core4x3 guest_only = both;
  CHECK_EQ(wide_2d_extent(guest_only.host_wide,
                          /*host_engaged=*/false,
                          guest_only.guest_wide,
                          guest_only.guest_engaged,
                          guest_only.native)
               .wide,
           428);
}

int main(void) {
  RUN(a_guest_projection_widening_makes_the_rule_active);
  RUN(a_host_engine_widening_still_makes_the_rule_active);
  RUN(four_three_three_is_not_active);
  RUN(a_plan_that_resolved_narrow_is_not_a_widening);
  RUN(the_mechanisms_are_alternatives_not_a_sum);
  return pt_summary();
}
