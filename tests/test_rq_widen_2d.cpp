// test_rq_widen_2d.cpp — the widescreen 2D layout rule (`rq_widen_2d_x`).
//
// WHY THIS TEST EXISTS. Tomba2 kanban #73: the score-pickup point popup renders, but at 16:9 it sits
// 54 px to the RIGHT of the character it is supposed to float over. Root cause, measured end to end
// on two headless instances with byte-identical world state:
//
//   * the popup's anchor is a GUEST GTE projection (gen_func_8003F7A0 = RTPS, store SXY2), and
//     native_boot.cpp writes GTE CR24 = OFX = nw/2 under widescreen — so the guest's own screen x is
//     ALREADY wide-final. Measured (scorepopup channel, pre-widen): 4:3 = 125/117/109,
//     16:9 = 179/171/163. Delta +54 = exactly margin, i.e. the projection is already widened.
//   * RenderQueue::emitOrQueue then added margin a SECOND time, because it GUESSED that every 2D
//     prim is authored in 4:3 space. Measured (preseqobj channel, it->xs[0] — what actually
//     rasterizes): 4:3 = 117/109/105, 16:9 = 225/217/213. Delta 108 = 2 x margin, on 3/3 glyphs.
//
// So the rule under test is not "how much do we shift 2D" — that part was always right — it is
// "WHOSE COORDINATES ARE THESE". A 2D producer is in one of exactly two screen spaces, and the queue
// must be TOLD which, not infer it. `Rq2dSpace` is that declaration and `rq_widen_2d_x` is the whole
// rule, extracted as pure arithmetic precisely so it can be tested with no Core, no GPU, no window.
//
// A NEGATIVE HERE CARRIES ITS DENOMINATOR: the 4:3 case asserts the no-op across an ENUMERATED sweep
// of every (space x layer x flat x untextured) combination and prints how many it covered, so
// "widescreen off changes nothing" is a counted claim rather than one spot check.
#include "testutil.h"
#include "render_queue.h"

// The two aspect shapes the framework ships, as the games actually see them.
static const int kNative320 = 320, kWide169 = 428;   // Tomba! 2 / Spider-Man — margin 54
static const int kNative512 = 512, kSpyro169 = 684;  // Spyro (512x240) — margin 86, NOT 182

// ---- 1. 4:3 is a no-op BY CONSTRUCTION, across every combination -------------------------------
// The whole widescreen path must be invisible when the user has not asked for it. Swept rather than
// spot-checked so the count is the evidence.
static void test_4_3_is_identity_everywhere(void) {
  int combos = 0;
  const int xs[] = { 0, 1, 159, 160, 161, 319 };
  const int layers[] = { RQ_BACKGROUND, RQ_WORLD, RQ_OVERLAY, RQ_HUD };
  const Rq2dSpace spaces[] = { RQ_2D_AUTHORED_4_3, RQ_2D_WIDE_FINAL };
  for (unsigned si = 0; si < sizeof spaces / sizeof *spaces; si++)
    for (unsigned li = 0; li < sizeof layers / sizeof *layers; li++)
      for (int flat = 0; flat <= 1; flat++)
        for (int untex = 0; untex <= 1; untex++)
          for (unsigned xi = 0; xi < sizeof xs / sizeof *xs; xi++) {
            combos++;
            CHECK_EQ(rq_widen_2d_x(xs[xi], kNative320, kNative320, spaces[si],
                                   layers[li], flat != 0, untex != 0), xs[xi]);
          }
  // Denominator: say what was actually swept, so "no change at 4:3" is a number.
  printf("      [4:3 identity] swept %d (space x layer x flat x untextured x x) combinations\n", combos);
  CHECK_EQ(combos, 2 * 4 * 2 * 2 * 6);
}

// ---- 2. 4:3-AUTHORED coordinates are CENTRED (the long-standing, correct behaviour) -------------
static void test_authored_4_3_is_centred(void) {
  const int margin = (kWide169 - kNative320) / 2;   // 54
  CHECK_EQ(margin, 54);
  // A HUD/overlay element keeps its native size and is registered with the centred world.
  CHECK_EQ(rq_widen_2d_x(0,   kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 54);
  CHECK_EQ(rq_widen_2d_x(160, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 214);
  CHECK_EQ(rq_widen_2d_x(320, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_HUD,     true, true), 374);
}

// ---- 3. Only a UNIFORM SOLID FILL stretches; anything with content is pillarboxed ---------------
// Stretching a flat colour is uniform (it backs the pillarbox bars). Stretching a GRADIENT or a
// TEXTURED backdrop spreads/squishes the picture, so those get the same centring as everything else.
static void test_only_flat_untextured_background_stretches(void) {
  // flat + untextured + RQ_BACKGROUND -> stretch to fill [0, ww)
  CHECK_EQ(rq_widen_2d_x(0,   kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, true), 0);
  CHECK_EQ(rq_widen_2d_x(320, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, true), 428);
  CHECK_EQ(rq_widen_2d_x(160, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, true), 214);
  // a GRADIENT background (not flat) -> centred, not stretched
  CHECK_EQ(rq_widen_2d_x(320, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, false, true), 374);
  // a TEXTURED background (title art / menu image) -> centred, not stretched
  CHECK_EQ(rq_widen_2d_x(320, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, false), 374);
  // stretch is a BACKGROUND-only rule — the same flat/untextured quad on another layer is centred
  CHECK_EQ(rq_widen_2d_x(320, kWide169, kNative320, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 374);
}

// ---- 4. WIDE-FINAL coordinates are ALREADY in the wide frame — never shifted -------------------
// THE #73 CASE. These x values came out of a projection the framework had already widened (guest GTE
// with CR24 = nw/2, or the native camera). Adding the centring margin to them shifts them off the
// thing they are anchored to, by exactly one margin.
static void test_wide_final_is_never_shifted(void) {
  const int layers[] = { RQ_BACKGROUND, RQ_WORLD, RQ_OVERLAY, RQ_HUD };
  int covered = 0;
  for (unsigned li = 0; li < sizeof layers / sizeof *layers; li++)
    for (int flat = 0; flat <= 1; flat++)
      for (int untex = 0; untex <= 1; untex++) {
        covered++;
        // No layer, and no material shape, may move a wide-final coordinate — INCLUDING the
        // flat+untextured RQ_BACKGROUND combination that stretches when it is 4:3-authored.
        CHECK_EQ(rq_widen_2d_x(214, kWide169, kNative320, RQ_2D_WIDE_FINAL,
                               layers[li], flat != 0, untex != 0), 214);
      }
  printf("      [wide-final] swept %d (layer x flat x untextured) combinations\n", covered);
  CHECK_EQ(covered, 4 * 2 * 2);

  // The MEASURED kanban #73 numbers, pinned as the regression case. The guest GTE produced these x
  // for the three score-popup glyphs at 16:9; the correct output is each unchanged. Before the fix
  // the queue returned 233/225/217 — one margin too far right, which is exactly what the user saw.
  CHECK_EQ(rq_widen_2d_x(179, kWide169, kNative320, RQ_2D_WIDE_FINAL, RQ_OVERLAY, true, false), 179);
  CHECK_EQ(rq_widen_2d_x(171, kWide169, kNative320, RQ_2D_WIDE_FINAL, RQ_OVERLAY, true, false), 171);
  CHECK_EQ(rq_widen_2d_x(163, kWide169, kNative320, RQ_2D_WIDE_FINAL, RQ_OVERLAY, true, false), 163);
}

// ---- 5. The margin scales from the GAME'S OWN 4:3 width, not from a hardcoded 320 ---------------
// psxport a0b88136 / 94e52472 / 2c54ce71 / 6dda8528 each fixed one instance of the "every PSX game is
// 320 wide" assumption. render_queue.cpp was a FIFTH instance that those commits missed: it is a
// no-op for a 320-wide game, so nothing caught it, but for Spyro (512x240) it centred by
// (684-320)/2 = 182 instead of (684-512)/2 = 86 and stretched by 684/320 instead of 684/512.
static void test_margin_scales_from_the_games_own_width(void) {
  const int margin = (kSpyro169 - kNative512) / 2;   // 86
  CHECK_EQ(margin, 86);
  CHECK_EQ(rq_widen_2d_x(0,   kSpyro169, kNative512, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 86);
  CHECK_EQ(rq_widen_2d_x(256, kSpyro169, kNative512, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 342);
  // and the stretch divides by the game's own width, so a full-width fill lands exactly on the edge
  CHECK_EQ(rq_widen_2d_x(512, kSpyro169, kNative512, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, true), 684);
  // the 320-hardcode would have produced these WRONG values — pinned so a regression is unambiguous
  CHECK(rq_widen_2d_x(0, kSpyro169, kNative512, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true) != 182);
}

// ---- 6. A narrower-or-equal "wide" width must not shift anything --------------------------------
// Defensive, and it is the shape a mis-set aspect table produced before a0b88136: if ww <= native the
// margin is <= 0, and the only correct action is to leave the coordinates alone rather than pull them
// left (which would crop the picture inward).
static void test_non_positive_margin_is_identity(void) {
  CHECK_EQ(rq_widen_2d_x(100, 320, 320, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 100);
  CHECK_EQ(rq_widen_2d_x(100, 300, 320, RQ_2D_AUTHORED_4_3, RQ_OVERLAY, true, true), 100);
  CHECK_EQ(rq_widen_2d_x(100, 300, 320, RQ_2D_AUTHORED_4_3, RQ_BACKGROUND, true, true), 100);
}

int main(void) {
  RUN(4_3_is_identity_everywhere);
  RUN(authored_4_3_is_centred);
  RUN(only_flat_untextured_background_stretches);
  RUN(wide_final_is_never_shifted);
  RUN(margin_scales_from_the_games_own_width);
  RUN(non_positive_margin_is_identity);
  return pt_summary();
}
