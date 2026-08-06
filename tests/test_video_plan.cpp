// test_video_plan.cpp — the RESOLUTION decisions must not depend on whether a window exists.
//
// WHAT THIS GATES. `runtime/recomp/video_plan.h` owns the two decisions that used to read the live
// WINDOW size: the AUTO internal-resolution scale, and the widened framebuffer width under
// ASPECT_AUTO. The shipped rule (gpu_vk.cpp:229/252) called `win_h()` / `win_w()`, which are
// documented as "live window size in pixels, falling back to native 4:3 before the window exists":
//
//     static int win_h(void) { int w=320,h=240; if (s_win) SDL_GetWindowSizeInPixels(s_win,&w,&h); ... }
//     if (i == 0) { i = (int)((win_h() / 240.0) + 0.5); if (i < 1) i = 1; }
//
// Headless has no `s_win`, so it took the 240 fallback and computed ires = 1 where the same build in
// its 960x720 window computed ires = 3. A headless capture was therefore NOT the user's picture, and
// almost every measurement in this project is taken headless. USER RULE: "windowed and headless
// should be equal anyway, it shouldn't change anything in the game. headless just means no window and
// no audio."
//
// THE FIX IS NOT A HEADLESS BRANCH. The presentation SINK exists in both legs — windowed it is the
// live drawable, headless it is `sink_size()`'s configured size (PSXPORT_PRESENT_SINK, defaulting to
// the window's own creation size, 960x720). Resolution is an explicit input; these decisions were
// simply reading the wrong surface.
//
// THE NEGATIVE CONTROL IS BUILT IN and is a COMMAND. `legacy_*()` below transcribe the rule psxport
// shipped at 9890eaa8, `win_h()` fallback included. Compile with -DPSXPORT_TEST_LEGACY_VIDEO_PLAN to
// run this suite against it and watch it go red; the default build additionally asserts that the
// legacy rule FAILS the property, so this file cannot pass while modelling nothing.
//
//   RED  : g++ -std=c++20 -I runtime/recomp -I tests -DPSXPORT_TEST_LEGACY_VIDEO_PLAN -o
//          scratch/bin/t_video tests/test_video_plan.cpp && scratch/bin/t_video
//   GREEN: the same without the define, or `ctest -R test_video_plan`.
//
// Hermetic: no SDL, no GPU, no window, no disc.

#include "video_plan.h"
#include "testutil.h"

// The window's creation size, which is also the headless sink's default (gpu_vk.cpp
// PRESENT_WINDOW_W/H). Spelled as literals here on purpose: a test that imported the constants it
// checks would pass no matter what they were changed to.
static const int SINK_W = 960, SINK_H = 720;

// The pre-window fallback `win_w()` / `win_h()` return, and the source of the whole defect.
static const int NO_WINDOW_W = 320, NO_WINDOW_H = 240;

// ---- the rule as shipped at 9890eaa8, kept ONLY as this suite's negative control -------------------
[[maybe_unused]] static int legacy_ires(const VideoInputs& in, bool hasWindow) {
  const int winH = hasWindow ? in.sinkH : NO_WINDOW_H;
  const int cap = in.iresCap < 1 ? 1 : in.iresCap;
  int i = in.modsIres;
  if (i == 0) { i = (int)((winH / 240.0) + 0.5); if (i < 1) i = 1; }
  if (i < 1) i = 1;
  if (i > cap) i = cap;
  return i;
}
[[maybe_unused]] static int legacy_wide_native_w(const VideoInputs& in, bool hasWindow) {
  const int native = in.nativeW > 0 ? in.nativeW : 320;
  auto scaled = [&](int for320) {
    if (native == 320) return for320;
    int w = (int)((double)for320 * native / 320.0 + 0.5);
    w &= ~1;
    return (in.vramW > 0 && w > in.vramW) ? in.vramW : w;
  };
  switch (in.aspect) {
    case ASPECT_16_9: return scaled(428);
    case ASPECT_21_9: return scaled(560);
    case ASPECT_AUTO: {
      const int ww = hasWindow ? in.sinkW : NO_WINDOW_W;
      const int wh = hasWindow ? in.sinkH : NO_WINDOW_H;
      int w = (int)(((double)native * 0.75 * ww) / wh + 0.5); w &= ~1;
      if (w < native) w = native;
      if (in.vramW > 0 && w > in.vramW) w = in.vramW;
      return w;
    }
    default: return native;
  }
}

// The units under test, with the window presented as a parameter the SHIPPED rule simply ignores —
// that asymmetry IS the fix.
static int ires_of(const VideoInputs& in, bool hasWindow) {
#ifdef PSXPORT_TEST_LEGACY_VIDEO_PLAN
  return legacy_ires(in, hasWindow);
#else
  (void)hasWindow;
  return video_ires_scale(in);
#endif
}
static int wide_of(const VideoInputs& in, bool hasWindow) {
#ifdef PSXPORT_TEST_LEGACY_VIDEO_PLAN
  return legacy_wide_native_w(in, hasWindow);
#else
  (void)hasWindow;
  return video_wide_native_w(in);
#endif
}

// ---- the input table -------------------------------------------------------------------------------
// Every case is a configuration one of the three ports actually renders in: Tomba!2 is 320 wide,
// spyro and spider1 are 512 wide, and each can be 4:3, 16:9, 21:9 or AUTO with a fixed or AUTO ires.
struct Case { const char* name; VideoInputs in; };
static VideoInputs mk(int nativeW, int aspect, int modsIres, int cap) {
  VideoInputs v;
  v.sinkW = SINK_W; v.sinkH = SINK_H;
  v.nativeW = nativeW; v.aspect = aspect; v.modsIres = modsIres; v.iresCap = cap;
  v.vramW = 1024;   // VRAM_W
  return v;
}
static const Case CASES[] = {
  { "Tomba!2 320 4:3, ires AUTO",   mk(320, ASPECT_4_3,  0, 8) },
  { "Tomba!2 320 16:9, ires AUTO",  mk(320, ASPECT_16_9, 0, 8) },
  { "Tomba!2 320 AUTO, ires AUTO",  mk(320, ASPECT_AUTO, 0, 8) },
  { "spyro 512 4:3, ires AUTO",     mk(512, ASPECT_4_3,  0, 8) },
  { "spyro 512 16:9, ires AUTO",    mk(512, ASPECT_16_9, 0, 8) },
  { "spyro 512 21:9, ires AUTO",    mk(512, ASPECT_21_9, 0, 8) },
  { "spyro 512 AUTO, ires AUTO",    mk(512, ASPECT_AUTO, 0, 8) },
  { "spider1 512 4:3, ires 1",      mk(512, ASPECT_4_3,  1, 8) },
  { "spider1 512 16:9, ires 4",     mk(512, ASPECT_16_9, 4, 8) },
  { "256-wide mode, ires AUTO",     mk(256, ASPECT_AUTO, 0, 8) },
  { "640-wide mode, ires AUTO",     mk(640, ASPECT_16_9, 0, 8) },
};
static const int NCASES = (int)(sizeof CASES / sizeof CASES[0]);

// ────────────────────────────────────────────────────────────────────────────────────────────────────
// THE PROPERTY — every resolution decision is identical with and without a window surface.
// ────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_ires_is_identical_with_and_without_a_window(void) {
  for (int i = 0; i < NCASES; ++i) {
    const int win = ires_of(CASES[i].in, /*hasWindow=*/true);
    const int hdl = ires_of(CASES[i].in, /*hasWindow=*/false);
    CHECK_EQ(hdl, win);
  }
  CHECK_EQ(NCASES, 11);   // the denominator: 11 configurations, both legs each
}

static void test_widescreen_width_is_identical_with_and_without_a_window(void) {
  for (int i = 0; i < NCASES; ++i) {
    const int win = wide_of(CASES[i].in, /*hasWindow=*/true);
    const int hdl = wide_of(CASES[i].in, /*hasWindow=*/false);
    CHECK_EQ(hdl, win);
  }
  CHECK_EQ(NCASES, 11);
}

// The concrete number the defect produced, so "identical" cannot be satisfied by both legs being
// wrong: a 960x720 sink is three PSX fields tall, so AUTO ires is 3 — not the 1 the 240-line
// no-window fallback produced.
static void test_a_960x720_sink_is_ires_3_in_both_legs(void) {
  const VideoInputs v = mk(512, ASPECT_4_3, /*AUTO*/ 0, 8);
  CHECK_EQ(ires_of(v, true), 3);
  CHECK_EQ(ires_of(v, false), 3);
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────
// The decisions themselves, pinned so the move out of gpu_vk.cpp is an equivalence proof.
// ────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_auto_ires_derives_from_the_sink_height(void) {
  struct { int h, want; } t[] = { {240,1}, {360,2}, {480,2}, {600,3}, {720,3}, {1080,5}, {2160,9} };
  int checked = 0;
  for (auto& e : t) {
    VideoInputs v = mk(320, ASPECT_4_3, 0, 16);   // cap high enough not to mask the derivation
    v.sinkH = e.h;
    CHECK_EQ(video_ires_scale(v), e.want);
    ++checked;
  }
  CHECK_EQ(checked, 7);
}

static void test_a_fixed_ires_ignores_the_sink_entirely(void) {
  for (int fixed = 1; fixed <= 4; ++fixed) {
    VideoInputs a = mk(320, ASPECT_4_3, fixed, 8); a.sinkH = 240;
    VideoInputs b = mk(320, ASPECT_4_3, fixed, 8); b.sinkH = 2160;
    CHECK_EQ(video_ires_scale(a), fixed);
    CHECK_EQ(video_ires_scale(b), fixed);
  }
}

static void test_the_memory_cap_clamps_both_auto_and_fixed(void) {
  VideoInputs autoV = mk(320, ASPECT_4_3, 0, 2); autoV.sinkH = 2160;   // would want 9
  CHECK_EQ(video_ires_scale(autoV), 2);
  VideoInputs fixedV = mk(320, ASPECT_4_3, 4, 2);
  CHECK_EQ(video_ires_scale(fixedV), 2);
  VideoInputs zeroCap = mk(320, ASPECT_4_3, 4, 0);                     // a nonsense cap floors at 1
  CHECK_EQ(video_ires_scale(zeroCap), 1);
}

// A 320-wide game must be bit-identical to the pre-existing widescreen targets — those numbers have
// consumers tuned to them, and this refactor must not re-derive them.
static void test_a_320_wide_game_keeps_its_historical_widescreen_targets(void) {
  CHECK_EQ(video_wide_native_w(mk(320, ASPECT_4_3,  1, 8)), 320);
  CHECK_EQ(video_wide_native_w(mk(320, ASPECT_16_9, 1, 8)), 428);
  CHECK_EQ(video_wide_native_w(mk(320, ASPECT_21_9, 1, 8)), 560);
}

// The 512-wide case the scaling exists for: 16:9 must WIDEN a 512-wide frame, not crop it to 428.
static void test_a_512_wide_game_widens_rather_than_crops(void) {
  const int w169 = video_wide_native_w(mk(512, ASPECT_16_9, 1, 8));
  CHECK_EQ(w169, 684);            // round(428 * 512/320) = 685 -> forced even
  CHECK(w169 > 512);
  const int w219 = video_wide_native_w(mk(512, ASPECT_21_9, 1, 8));
  CHECK_EQ(w219, 896);            // round(560 * 512/320) = 896
  CHECK(w219 > w169);
}

static void test_aspect_auto_matches_the_sink_aspect(void) {
  // 960x720 is 4:3, so AUTO must return the game's own native width unchanged.
  CHECK_EQ(video_wide_native_w(mk(320, ASPECT_AUTO, 1, 8)), 320);
  CHECK_EQ(video_wide_native_w(mk(512, ASPECT_AUTO, 1, 8)), 512);
  // A 16:9 sink widens: 320 * 0.75 * 1920/1080 = 426.67 -> 426 (even).
  VideoInputs wide = mk(320, ASPECT_AUTO, 1, 8); wide.sinkW = 1920; wide.sinkH = 1080;
  CHECK_EQ(video_wide_native_w(wide), 426);
  // A TALLER-than-4:3 sink never narrows below native — a narrower frame would crop the picture.
  VideoInputs tall = mk(320, ASPECT_AUTO, 1, 8); tall.sinkW = 800; tall.sinkH = 1200;
  CHECK_EQ(video_wide_native_w(tall), 320);
  // A missing sink degrades to native rather than dividing by zero.
  VideoInputs none = mk(512, ASPECT_AUTO, 1, 8); none.sinkW = 0; none.sinkH = 0;
  CHECK_EQ(video_wide_native_w(none), 512);
}

static void test_a_widened_frame_is_clamped_to_vram(void) {
  VideoInputs v = mk(640, ASPECT_21_9, 1, 8);   // 560 * 640/320 = 1120 > VRAM_W
  CHECK_EQ(video_wide_native_w(v), 1024);
}

#ifndef PSXPORT_TEST_LEGACY_VIDEO_PLAN
// THE SUITE'S OWN NEGATIVE CONTROL, asserted rather than described: the legacy rule must FAIL the
// property, on the cases that matter. If this ever passes, the properties above have stopped
// discriminating.
static void test_the_legacy_rule_fails_the_property(void) {
  int diverged_ires = 0, diverged_wide = 0;
  for (int i = 0; i < NCASES; ++i) {
    if (legacy_ires(CASES[i].in, true) != legacy_ires(CASES[i].in, false)) ++diverged_ires;
    if (legacy_wide_native_w(CASES[i].in, true) != legacy_wide_native_w(CASES[i].in, false))
      ++diverged_wide;
  }
  // 9 of the 11 cases ask for AUTO ires; every one of them reads 3 windowed and 1 headless.
  CHECK_EQ(diverged_ires, 9);
  // 3 of the 11 ask for ASPECT_AUTO; 960x720 and 320x240 are both 4:3 so the WIDTH happens to agree
  // — which is exactly why this defect survived: one of the two decisions is silently correct at the
  // default sink and only breaks on a non-4:3 window.
  CHECK_EQ(diverged_wide, 0);
  VideoInputs wide = mk(320, ASPECT_AUTO, 1, 8); wide.sinkW = 1920; wide.sinkH = 1080;
  CHECK_EQ(legacy_wide_native_w(wide, true), 426);
  CHECK_EQ(legacy_wide_native_w(wide, false), 320);   // headless silently loses the widescreen FOV
  CHECK(legacy_wide_native_w(wide, true) != legacy_wide_native_w(wide, false));
}
#endif

int main(void) {
  RUN(ires_is_identical_with_and_without_a_window);
  RUN(widescreen_width_is_identical_with_and_without_a_window);
  RUN(a_960x720_sink_is_ires_3_in_both_legs);
  RUN(auto_ires_derives_from_the_sink_height);
  RUN(a_fixed_ires_ignores_the_sink_entirely);
  RUN(the_memory_cap_clamps_both_auto_and_fixed);
  RUN(a_320_wide_game_keeps_its_historical_widescreen_targets);
  RUN(a_512_wide_game_widens_rather_than_crops);
  RUN(aspect_auto_matches_the_sink_aspect);
  RUN(a_widened_frame_is_clamped_to_vram);
#ifndef PSXPORT_TEST_LEGACY_VIDEO_PLAN
  RUN(the_legacy_rule_fails_the_property);
#endif
  return pt_summary();
}
