// test_bary_round — barycentric interpolation must ROUND, not truncate.
//
// REGRESSION UNDER TEST (Tomba!2, 2026-08-20). beetle seeds every interpolant with a half-LSB bias:
//
//   gpu_polygon.c:904   ig.u = (COORD_MF_INT(u) + (1 << (COORD_FBS - 1 - upscale_shift))) << ...
//   gpu_polygon.c:945   ig.r = (COORD_MF_INT(r) + (1 << (COORD_FBS - 1)))                 << ...
//
// so its DDA rounds to nearest rather than truncating. gpu_native.cpp already replicated that for
// U and V — the comment there says so — and did NOT for the interpolated COLOUR, which used a plain
// integer divide. Integer division truncates toward zero, so our colour is systematically LOW.
//
// MEASURED: at f1090 of replays/bugs/ingame-options-page.pad, after the dither fix, 5,534 pixels
// still differed from the beetle GPU oracle by exactly one 5-bit step — and 5,896 of the 6,031
// per-channel differences (97.6%) were ours-LOW. On the blue-only gradient of f1160 (kanban #112)
// the same effect is 100% ours-low in blue. A symmetric rounding error would be ~50/50; a
// systematic bias in one direction is truncation.
//
// Hermetic: the rule is one pure integer function. No GPU, no window, no disc.
//
// NEGATIVE-RESULT DISCIPLINE: both signs of the doubled area are asserted (a triangle's winding
// makes `aa` negative half the time, and rounding must not become floor-toward-negative there), and
// exact cases are asserted alongside the .5 cases so a fix that simply added +1 everywhere fails.
#include "../runtime/recomp/gpu_native_internal.h"
#include "testutil.h"

static void test_exact_values_are_unchanged(void) {
  // Weights summing to aa with an exact result: rounding must not shift it.
  CHECK_EQ(bary_round(1, 100, 1, 100, 2, 100, 4), 100); // uniform colour, any weights
  CHECK_EQ(bary_round(2, 0, 1, 40, 1, 40, 4), 20);      // (0*2 + 40 + 40)/4 = 20 exactly
}

static void test_rounds_to_nearest_instead_of_truncating(void) {
  // (0*1 + 0*1 + 42*2)/4 = 21 exactly; nudge to land on .5 and .25/.75 boundaries.
  CHECK_EQ(bary_round(1, 0, 1, 0, 1, 2, 4), 1); // 2/4 = 0.5 -> 1   (truncation gives 0)
  CHECK_EQ(bary_round(1, 0, 1, 0, 1, 1, 4), 0); // 1/4 = 0.25 -> 0
  CHECK_EQ(bary_round(1, 0, 1, 0, 1, 3, 4), 1); // 3/4 = 0.75 -> 1
  CHECK_EQ(bary_round(1, 0, 1, 0, 1, 6, 4), 2); // 6/4 = 1.5  -> 2  (truncation gives 1)
}

static void test_negative_doubled_area_gives_the_same_answer(void) {
  // A triangle's winding flips the sign of `aa` and of every weight with it. The interpolated value
  // is unchanged, so rounding must be applied in sign-normalized form — not as floor(), which would
  // round the wrong way for negative winding and put the bias back in.
  CHECK_EQ(bary_round(-1, 0, -1, 0, -1, 2, -4), 1);
  CHECK_EQ(bary_round(-1, 0, -1, 0, -1, 6, -4), 2);
  CHECK_EQ(bary_round(-2, 0, -1, 40, -1, 40, -4), 20);
}

int main(void) {
  RUN(exact_values_are_unchanged);
  RUN(rounds_to_nearest_instead_of_truncating);
  RUN(negative_doubled_area_gives_the_same_answer);
  return pt_summary();
}
