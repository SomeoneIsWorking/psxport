// The widescreen left margin is ONE quantity with ONE spelling, and the odd-width case is pinned.
//
// WHY THIS EXISTS. Crash Bash's two native producers each computed the left margin of the columns a wider
// projection exposes, and they computed it DIFFERENTLY: the model producer wrote
// `gpu_vk_wide_engine_ofx(&core) - gpu_vk_native_w(&core) / 2` and the sprite producer wrote
// `(gpu_vk_wide_engine_w(&core) - gpu_vk_native_w(&core)) / 2`. Both are now `gpu_vk_wide_left_margin`.
//
// The two spellings are arithmetically EQUAL whenever both widths are even — and they are even today only
// because `video_wide_native_w` ends with `w &= ~1`, three files away in another layer. That is not a
// property of the margin's inputs; it is a coincidence two producers were relying on. The moment that
// line changes, the model producer shifts one column relative to the sprite producer, which is a visible
// seam in one frame rather than an error anywhere.
//
// So this file pins the arithmetic itself, which needs no Core, no sink and no window:
//
//   * the direct form `(wide_w - native_w) / 2` is what the owner computes;
//   * the old form `wide_w / 2 - native_w / 2` agrees with it for every EVEN pair — the case that made the
//     duplication invisible;
//   * and the two DIVERGE the moment exactly one width is odd. That case is asserted here, with the
//     concrete pairs, so the property is a fact in the suite rather than a remark in a header somebody
//     has to trust.
#include "gpu_vk.h"
#include "testutil.h"

#include <cstdio>

namespace {

// Every even pair must agree, because that agreement is the only reason the duplication went unnoticed —
// and the only reason a future odd width would be a silent regression rather than a loud one.
void test_even_widths_agree_with_the_old_spelling() {
  constexpr int kWidths[] = {320, 322, 368, 424, 426, 428, 512, 640, 684};
  for (int wide : kWidths) {
    for (int native : kWidths) {
      if (wide < native) {
        continue;
      }
      CHECK_EQ(wide_left_margin_from(wide, native), (wide - native) / 2);
      // The old spelling, asserted equal rather than merely believed equal.
      CHECK_EQ(wide_left_margin_from(wide, native), wide / 2 - native / 2);
    }
  }
  std::fprintf(stderr, "  every even pair agreed: the two spellings are indistinguishable here\n");
}

// THE case that matters. With exactly one width odd they differ, so two producers left on the two
// spellings would place their content a column apart.
void test_one_odd_width_makes_the_two_spellings_differ() {
  struct Case {
    int wide;
    int native;
  };
  // THE EXACT CONDITION, found by letting the counted assertion below reject four pairs I had wrongly
  // assumed would diverge: the two forms differ if and only if the WIDE width is even AND the native
  // width is odd. With wide even, `wide / 2` is exact; with native odd, `native / 2` truncates and loses
  // the half, so the old spelling comes out one greater. Every other parity combination truncates the
  // same half on both sides and agrees — wide odd, native odd, or both odd.
  //
  // That narrows the hazard honestly, and it is the NATIVE width that has to go odd: the wide width is
  // forced even by `video_wide_native_w`'s `w &= ~1`, so the duplication was safe by two independent
  // accidents rather than by one. `s_disp_w` is even for every PSX display mode in practice, which is
  // the third.
  constexpr Case kDiverge[] = {{428, 321}, {512, 321}, {640, 511}, {684, 513}, {428, 255}};
  for (Case c : kDiverge) {
    const int direct = wide_left_margin_from(c.wide, c.native);
    const int old = c.wide / 2 - c.native / 2;
    CHECK_EQ(direct, (c.wide - c.native) / 2);
    std::fprintf(stderr,
                 "  wide=%d native=%d: direct=%d old-spelling=%d (differ by %d)\n",
                 c.wide,
                 c.native,
                 direct,
                 old,
                 old - direct);
  }
  // Counted, not asserted by eye: if a chosen pair did NOT diverge, this test would be asserting nothing
  // about the hazard it exists to pin.
  int differing = 0;
  for (Case c : kDiverge) {
    if (c.wide / 2 - c.native / 2 != wide_left_margin_from(c.wide, c.native)) {
      ++differing;
    }
  }
  CHECK_EQ(differing, static_cast<int>(sizeof(kDiverge) / sizeof(kDiverge[0])));
  // And the condition itself, over a parity sweep, so "iff wide even and native odd" is a checked claim
  // rather than a comment. The sweep is bounded to `wide >= native`, which is the only domain the margin
  // exists in: BELOW that the margin is negative and C++ truncates toward ZERO on each half, so the
  // parity rule does not describe it. The sweep found that — it reported a divergence at wide=319,
  // native=320 that the rule forbids — which is why the domain is stated here instead of left implicit.
  for (int wide = 318; wide <= 332; ++wide) {
    for (int native = 318; native <= wide; ++native) {
      const bool shouldDiffer = (wide % 2 == 0) && (native % 2 == 1);
      const bool doesDiffer = (wide / 2 - native / 2) != wide_left_margin_from(wide, native);
      CHECK_EQ(doesDiffer, shouldDiffer);
    }
  }
}

// The quantity's edges, including the two real widenings this workspace ships.
void test_edges() {
  CHECK_EQ(wide_left_margin_from(320, 320), 0);  // already 4:3: no margin, and specifically not negative
  CHECK_EQ(wide_left_margin_from(428, 320), 54); // the documented 16:9 step over a 320-wide native
  CHECK_EQ(wide_left_margin_from(684, 512), 86); // Spyro 1, the case verified live at render_width=684
  CHECK(wide_left_margin_from(320, 320) >= 0);   // equal widths never move an origin the wrong way
  CHECK(wide_left_margin_from(640, 512) > 0);    // a widening always produces a positive margin
}

} // namespace

int main() {
  RUN(even_widths_agree_with_the_old_spelling);
  RUN(one_odd_width_makes_the_two_spellings_differ);
  RUN(edges);
  return pt_summary();
}
