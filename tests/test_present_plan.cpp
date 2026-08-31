// test_present_plan.cpp — the presented picture must not depend on which leg built it.
//
// WHAT THIS GATES. `runtime/recomp/present_plan.h` decides everything about a presented frame:
// whether the composite runs, where it lands in the sink, which target it samples, the fade, and the
// 24bpp flag. The rule is that ONLY `to_swapchain` may differ between headless and windowed. If any
// other field can, then a headless measurement is a measurement of a different program — which is
// precisely what happened in issue 0005, where PSXPORT_SHOT_AT reported 99.95% non-black on an intro
// the user was watching go black in a window.
//
// THE NEGATIVE CONTROL IS BUILT IN, and is reproducible forever rather than being a paragraph in a
// commit message. `legacy_plan()` below is a transcription of the rule psxport actually shipped at
// af28715a: `GpuVkState::present()` returned at `if (s_headless) { SDL_SubmitGPUCommandBuffer(cmd);
// return; }` (gpu_vk.cpp:997) BEFORE `show_composite`, so headless produced no composite at all.
// Compile with -DPSXPORT_TEST_LEGACY_PRESENT_PLAN to run this suite against that rule and watch it
// go red. A test that has never been seen red proves nothing about what it covers, and a test whose
// red is a story rather than a command cannot be re-checked by the next session.
//
//   RED : g++ -std=c++20 -I runtime/recomp -I tests -DPSXPORT_TEST_LEGACY_PRESENT_PLAN
//              -o scratch/bin/t tests/test_present_plan.cpp && scratch/bin/t
//   GREEN: same without the define, or `ctest -R test_present_plan`.
//
// Hermetic: no SDL, no GPU, no window, no disc.

#include "present_plan.h"
#include "testutil.h"

// ---- the rule as shipped at af28715a, kept ONLY as this suite's negative control ------------------
// Headless never reached show_composite, so there was no plan and no picture; windowed computed the
// real one. Nothing else about the composite differed — which is the point: the split was not a
// subtle divergence in the maths, it was an entire missing stage.
[[maybe_unused]] static PresentPlan legacy_plan(const PresentInputs &in, bool headless) {
  if (headless) {
    return PresentPlan{}; // present() returned before show_composite
  }
  return plan_present(in, /*headless=*/false);
}

static PresentPlan plan(const PresentInputs &in, bool headless) {
#ifdef PSXPORT_TEST_LEGACY_PRESENT_PLAN
  return legacy_plan(in, headless);
#else
  return plan_present(in, headless);
#endif
}

// ---- the input table ------------------------------------------------------------------------------
// Every case is a real configuration one of the three ports actually presents in, so a green run
// carries a denominator that means something: 4:3 native, widescreen, high-res, faded, 24bpp.
struct Case {
  const char *name;
  PresentInputs in;
};
static const Case CASES[] = {
    {"native 4:3", {1280, 960, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 0}},
    {"native, y=256", {1280, 960, 0, 256, 320, 240, 320, 0, 0, 0, 0, 0, 0}},
    {"widescreen 512", {1280, 960, 0, 0, 512, 240, 320, 0, 0, 0, 0, 0, 0}},
    {"ires x4", {1280, 960, 0, 0, 320, 240, 320, 4, 0, 0, 0, 0, 0}},
    {"fade additive", {1280, 960, 0, 0, 320, 240, 320, 0, 1, 64, 32, 16, 0}},
    {"fade subtractive", {1280, 960, 0, 0, 320, 240, 320, 0, 2, 8, 8, 8, 0}},
    {"24bpp display", {1280, 960, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 1}},
    {"24bpp + ires x2", {1280, 960, 0, 0, 320, 240, 320, 2, 0, 0, 0, 0, 1}},
    {"tall window", {800, 1200, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 0}},
};
static const int NCASES = (int)(sizeof CASES / sizeof CASES[0]);

// ---- THE invariant --------------------------------------------------------------------------------
static void test_only_the_sink_differs_between_legs(void) {
  CHECK_EQ(NCASES, 9); // the denominator, asserted rather than assumed
  for (int i = 0; i < NCASES; i++) {
    const PresentPlan hl = plan(CASES[i].in, /*headless=*/true);
    const PresentPlan wn = plan(CASES[i].in, /*headless=*/false);
    // Same picture in both legs...
    CHECK(present_plan_same_picture(hl, wn));
    // ...and the sink is the only thing that differs.
    CHECK_EQ(hl.to_swapchain, false);
    CHECK_EQ(wn.to_swapchain, true);
  }
}

// The composite must RUN headless. Stated separately from the equality above because "both legs
// produced the same empty plan" would satisfy an equality check while covering nothing — that is the
// all-zero uniform output this project keeps mistaking for agreement.
static void test_the_composite_runs_in_both_legs(void) {
  for (int i = 0; i < NCASES; i++) {
    CHECK(plan(CASES[i].in, true).build);
    CHECK(plan(CASES[i].in, false).build);
  }
  CHECK_EQ(NCASES, 9);
}

// A non-degenerate picture: the plan must actually place a non-empty viewport, or "same in both
// legs" would be satisfied by two zeroed rects.
static void test_the_plan_is_not_degenerate(void) {
  for (int i = 0; i < NCASES; i++) {
    const PresentPlan p = plan(CASES[i].in, true);
    CHECK(p.viewport.w > 0);
    CHECK(p.viewport.h > 0);
    CHECK(p.disp[2] > 0);
    CHECK(p.disp[3] > 0);
  }
}

// ---- the composite rules themselves ---------------------------------------------------------------
static void test_ires_scales_the_sampled_rect(void) {
  PresentInputs in = {1280, 960, 16, 32, 320, 240, 320, 4, 0, 0, 0, 0, 0};
  const PresentPlan p = plan(in, true);
  CHECK_EQ(p.src_ires, 4);
  CHECK_EQ(p.disp[0], 64);   // 16 * 4
  CHECK_EQ(p.disp[1], 128);  // 32 * 4
  CHECK_EQ(p.disp[2], 1280); // 320 * 4
  CHECK_EQ(p.disp[3], 960);  // 240 * 4
}

static void test_a_scaled_present_is_never_24bpp(void) {
  PresentInputs in = {1280, 960, 0, 0, 320, 240, 320, 2, 0, 0, 0, 0, /*rgb24=*/1};
  CHECK_EQ(plan(in, true).fmt[0], 0); // ires composite is our own 1555 raster
  in.present_ires = 0;
  CHECK_EQ(plan(in, true).fmt[0], 1); // native read honours the display register
}

static void test_widescreen_letterboxes_to_its_own_width(void) {
  // 512:240 in a 1280x960 sink is wider than the sink, so it is height-limited by the aspect:
  // dw = 1280, dh = 1280 * 240 / 512 = 600, centred vertically.
  PresentInputs in = {1280, 960, 0, 0, 512, 240, 320, 0, 0, 0, 0, 0, 0};
  const PresentPlan p = plan(in, true);
  CHECK_EQ(p.viewport.w, 1280);
  CHECK_EQ(p.viewport.h, 600);
  CHECK_EQ(p.viewport.x, 0);
  CHECK_EQ(p.viewport.y, 180);
  // 4:3 in the same sink is NOT the same rect — proof the width term is live, not ignored.
  in.disp_w = 320;
  const PresentPlan q = plan(in, true);
  CHECK_EQ(q.viewport.w, 1280);
  CHECK_EQ(q.viewport.h, 960);
}

static void test_fade_is_carried_verbatim(void) {
  PresentInputs in = {1280, 960, 0, 0, 320, 240, 320, 0, 2, 1, 2, 3, 0};
  const PresentPlan p = plan(in, true);
  CHECK_EQ(p.fade[0], 2);
  CHECK_EQ(p.fade[1], 1);
  CHECK_EQ(p.fade[2], 2);
  CHECK_EQ(p.fade[3], 3);
}

// THE PLAN MUST CARRY ITS OWN SINK SIZE. Regression guard for a real defect: build_present_image
// originally called sink_w()/sink_h() again to size its target, so ONE present sampled the live
// window from three independent places (four counting the swapchain acquire). Mid-resize they
// disagree, and the letterbox is then computed for a rectangle that is not the one being drawn into.
// If the sink is not IN the plan, the renderer has to go and ask again — so pin it here.
static void test_the_plan_carries_its_own_sink(void) {
  for (int i = 0; i < NCASES; i++) {
    const PresentPlan p = plan(CASES[i].in, true);
    CHECK_EQ(p.sink_w, CASES[i].in.sink_w);
    CHECK_EQ(p.sink_h, CASES[i].in.sink_h);
    // and the viewport it computed must FIT inside that same sink — the two are one measurement
    CHECK(p.viewport.x >= 0 && p.viewport.y >= 0);
    CHECK(p.viewport.x + p.viewport.w <= p.sink_w);
    CHECK(p.viewport.y + p.viewport.h <= p.sink_h);
  }
}

// THE PRESENTED PICTURE MUST BE THE RIGHT SHAPE (issue 0008). On PSX every horizontal mode
// (256/320/368/512/640) scans into the SAME 4:3 screen area, so the framebuffer width sets the
// horizontal SAMPLING RATE, not the shape. The old rule letterboxed to `disp_w : 240`, and
// disp_w/240 == (4/3) x (disp_w/320) — i.e. it hardcoded 320 as the game's native 4:3 width. For a
// 320-wide game that constant IS its native width, so Tomba2 was correct in BOTH its modes and the
// bug was invisible there; a 512-wide game (spider1, spyro) had its picture stretched 1.6x wide.
// The rule is: aspect = 4:3 scaled by how much WIDER THAN ITS OWN NATIVE WIDTH the framebuffer is.
static void test_native_width_sets_the_aspect_not_the_framebuffer_width(void) {
  // 320-wide game, native: fills a 4:3 sink edge to edge.
  PresentInputs a = {960, 720, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 0};
  CHECK_EQ(plan(a, true).viewport.w, 960);
  CHECK_EQ(plan(a, true).viewport.h, 720);
  // 512-wide game, native: SAME shape. This is the row that fails against the old rule (960x450).
  PresentInputs b = {960, 720, 0, 0, 512, 240, 512, 0, 0, 0, 0, 0, 0};
  CHECK_EQ(plan(b, true).viewport.w, 960);
  CHECK_EQ(plan(b, true).viewport.h, 720);
  // 320-wide game widened to 428 for 16:9: a wide band, and the widescreen mod must keep working.
  PresentInputs c = {960, 720, 0, 0, 428, 240, 320, 0, 0, 0, 0, 0, 0};
  const PresentPlan pc = plan(c, true);
  CHECK_EQ(pc.viewport.w, 960);
  CHECK_EQ(pc.viewport.h, 538); // 960 * (3*320) / (4*428) = 538
  // 512-wide game widened to 683: the SAME 16:9-ish band, not a 2.85:1 sliver.
  PresentInputs d = {960, 720, 0, 0, 683, 240, 512, 0, 0, 0, 0, 0, 0};
  const PresentPlan pd = plan(d, true);
  CHECK_EQ(pd.viewport.w, 960);
  CHECK_EQ(pd.viewport.h, 539); // 960 * (3*512) / (4*683) = 539
  // And the two widescreen bands agree to within a pixel — the SAME requested aspect from two
  // different native widths, which is the whole point of scaling by native rather than by 320.
  CHECK(pc.viewport.h - pd.viewport.h <= 1 && pd.viewport.h - pc.viewport.h <= 1);
}

// THE FIX IS A PROVABLE NO-OP FOR EVERY 320-WIDE GAME — which is the whole of Tomba2Engine, the
// consumer this repo cannot test from here. The algebra: the old rule was disp_w : 240, the new one
// is 4*disp_w : 3*native_w; at native_w == 320 that is 4*disp_w : 960 == disp_w : 240. Identical.
// So Tomba2 cannot change in ANY mode, native or widescreen, at any framebuffer width. This asserts
// it across the whole plausible range rather than trusting the algebra, because "it should be
// identical" is exactly the kind of claim this project has been burned by.
static void test_a_320_wide_game_is_bit_identical_to_the_old_rule(void) {
  int compared = 0;
  for (int disp_w = 256; disp_w <= 640; disp_w++) {
    PresentInputs in = {960, 720, 0, 0, disp_w, 240, 320, 0, 0, 0, 0, 0, 0};
    const PaneRect got = plan(in, true).viewport;
    const PaneRect old = pane_letterbox(disp_w, 240, 960, 720); // the rule as it shipped
    CHECK_EQ(got.x, old.x);
    CHECK_EQ(got.y, old.y);
    CHECK_EQ(got.w, old.w);
    CHECK_EQ(got.h, old.h);
    compared++;
  }
  CHECK_EQ(compared, 385); // the denominator, asserted rather than assumed
}

// An unknown native width must not divide by zero or invent a widescreen: it degrades to 4:3, which
// is what every PSX display mode is, and is the only safe assumption when the game has not said.
static void test_unknown_native_width_degrades_to_4_3(void) {
  PresentInputs z = {960, 720, 0, 0, 512, 240, 0, 0, 0, 0, 0, 0, 0};
  const PresentPlan p = plan(z, true);
  CHECK(p.build);
  CHECK_EQ(p.viewport.w, 960);
  CHECK_EQ(p.viewport.h, 720);
}

// A sink with no area composes nothing — in EITHER leg. This is the only false `build`, and it is a
// statement about the sink, not about the leg.
static void test_an_empty_sink_builds_nothing(void) {
  const PresentInputs zero_w = {0, 960, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 0};
  const PresentInputs zero_h = {1280, 0, 0, 0, 320, 240, 320, 0, 0, 0, 0, 0, 0};
  CHECK(!plan(zero_w, true).build);
  CHECK(!plan(zero_w, false).build);
  CHECK(!plan(zero_h, true).build);
  CHECK(!plan(zero_h, false).build);
  CHECK(!plan(zero_w, false).to_swapchain); // nothing built, so nothing to show
}

static void test_static_2d_uses_native_source_width(void) {
  CHECK_EQ(present_display_width(true, 428, 320, true), 428);
  CHECK_EQ(present_display_width(true, 428, 320, false), 320);
  CHECK_EQ(present_display_width(false, 320, 320, false), 320);
}

int main(void) {
  RUN(only_the_sink_differs_between_legs);
  RUN(the_composite_runs_in_both_legs);
  RUN(the_plan_is_not_degenerate);
  RUN(ires_scales_the_sampled_rect);
  RUN(a_scaled_present_is_never_24bpp);
  RUN(widescreen_letterboxes_to_its_own_width);
  RUN(fade_is_carried_verbatim);
  RUN(the_plan_carries_its_own_sink);
  RUN(native_width_sets_the_aspect_not_the_framebuffer_width);
  RUN(a_320_wide_game_is_bit_identical_to_the_old_rule);
  RUN(unknown_native_width_degrades_to_4_3);
  RUN(an_empty_sink_builds_nothing);
  RUN(static_2d_uses_native_source_width);
  return pt_summary();
}
