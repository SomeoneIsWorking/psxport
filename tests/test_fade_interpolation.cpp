// How two screen-fade endpoints resolve at an interpolation factor.
//
// THE DEFECT THIS EXISTS FOR (Tomba! 2 issue 0021, measured 2026-09-22): a screen fade is a
// present-time composite, so nothing in the captured frame carries it and no producer reconstructs
// it. Both presents of a logic frame read the title's CURRENT level and composited the same one,
// which put the in-between present a whole frame early — and during a fade that is every pixel in
// the picture. 66 consecutive triples of the opening narration had every changed pixel at the next
// endpoint whatever t was.
//
// So the cases below are not arithmetic checks. Each one is an answer this must be able to give:
// the ramp midpoint that was missing, and the three cases where NOT interpolating is correct.
#include "fade_interpolation.h"

#include "testutil.h"

namespace {

FadeState fade(int mode, unsigned char r, unsigned char g, unsigned char b) {
  FadeState state{};
  state.mode = mode;
  state.r = r;
  state.g = g;
  state.b = b;
  return state;
}

constexpr int kNone = 0;
constexpr int kAdditive = 1;
constexpr int kSubtractive = 2;

// One packed comparison so a case reads as one CHECK_EQ with both sides printed: mode in the top
// byte, then r, g, b. A per-channel loop would print "got 212 want 208" without saying which.
long long packed(const FadeState &f) {
  return ((long long)f.mode << 24) | ((long long)f.r << 16) | ((long long)f.g << 8) | (long long)f.b;
}

long long packed(int mode, int r, int g, int b) {
  return ((long long)mode << 24) | ((long long)r << 16) | ((long long)g << 8) | (long long)b;
}

} // namespace

using psxport::fade::resolve;

// The ramp Tomba! 2's opening actually runs: a subtractive level stepping by 8 per logic frame.
// The in-between present belongs half a step back, and truncation would answer 216 here — leaving
// the whole ramp a frame early in a subtler way — so the midpoint is checked exactly.
static void test_ramp(void) {

  // THE RAMP. Tomba! 2's opening steps its subtractive level by 8 per logic frame; the in-between
  // present belongs half a step back. Truncation would answer 216 here and leave the whole ramp one
  // frame early in a subtler way, so the midpoint is checked exactly.
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), true, 0.5f)),
           packed(kSubtractive, 212, 212, 212));
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), true, 0.25f)),
           packed(kSubtractive, 214, 214, 214));
  CHECK_EQ(packed(resolve(fade(kAdditive, 100, 40, 0), fade(kAdditive, 108, 48, 8), true, 0.5f)),
           packed(kAdditive, 104, 44, 4));
  CHECK_EQ(packed(resolve(fade(kSubtractive, 0, 64, 255), fade(kSubtractive, 8, 128, 255), true, 0.5f)),
           packed(kSubtractive, 4, 96, 255));
  // A factor that does NOT land on an exact level. 216 + (208-216)*0.3 = 213.6, so rounding says
  // 214 and truncation says 213 — the one case that can tell them apart, and without it a
  // truncating resolve() passes every check above while biasing the whole ramp toward its previous
  // level. Same question the other way at 0.7: 210.4 rounds down to 210.
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), true, 0.3f)),
           packed(kSubtractive, 214, 214, 214));
  CHECK_EQ(packed(resolve(fade(kAdditive, 0, 0, 0), fade(kAdditive, 8, 8, 8), true, 0.3f)), packed(kAdditive, 2, 2, 2));
}

// t = 0 IS THE PREVIOUS ENDPOINT, exactly. This is the discriminator fps60_check.py --forced
// drives with PSXPORT_FPS60_TFORCE=0: if the forced present did not land on prev, the measurement
// could not tell interpolated content from content drawn a frame early.
static void test_forced_zero_is_the_previous_endpoint(void) {
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), true, 0.0f)),
           packed(kSubtractive, 216, 216, 216));
}

// THE CASES WHERE NOT INTERPOLATING IS THE RIGHT ANSWER. Each must return cur untouched, and a
// resolve() that lerped unconditionally would fail every one of them.
static void test_cases_that_must_not_interpolate(void) {
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), true, 1.0f)),
           packed(kSubtractive, 208, 208, 208));
  CHECK_EQ(packed(resolve(fade(kSubtractive, 216, 216, 216), fade(kSubtractive, 208, 208, 208), false, 0.5f)),
           packed(kSubtractive, 208, 208, 208));
  // A mode change is a discontinuity, not a ramp. Blending subtractive 200 into additive 8 would
  // composite 104 in a mode the title never asked for — brighter than either endpoint.
  CHECK_EQ(packed(resolve(fade(kSubtractive, 200, 200, 200), fade(kAdditive, 8, 8, 8), true, 0.5f)),
           packed(kAdditive, 8, 8, 8));
  CHECK_EQ(packed(resolve(fade(kSubtractive, 8, 8, 8), fade(kNone, 0, 0, 0), true, 0.5f)), packed(kNone, 0, 0, 0));
}

// The guard is a MODE guard, not a "did anything change" guard: equal modes at equal levels still
// resolve to that level, so a held fade does not shimmer between the two presents.
static void test_a_held_fade_does_not_move(void) {
  CHECK_EQ(packed(resolve(fade(kSubtractive, 120, 120, 120), fade(kSubtractive, 120, 120, 120), true, 0.5f)),
           packed(kSubtractive, 120, 120, 120));
}

int main(void) {
  RUN(ramp);
  RUN(forced_zero_is_the_previous_endpoint);
  RUN(cases_that_must_not_interpolate);
  RUN(a_held_fade_does_not_move);
  return pt_summary();
}
