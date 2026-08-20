// test_proj_params_geom.cpp — the camera's projection constants come from the GAME'S SETTER, and an
// unset projection is DISTINGUISHABLE from a set one.
//
// THE DEFECT THIS GATES. The native camera path recovered OFX/OFY/H by reading them back out of the
// GTE control registers (CR24/CR25/CR26) — `Fps60::sceneCam` and Tomba!2's `NativeScenePass::collect`
// both did `gte_read_ctrl(24)/(25)/(26)`. That is engine state consulted after the fact, the pattern
// the user banned outright, and it silently couples the native camera to whatever the guest last left
// in the GTE. The values were never unknown: the game STATES them, in libgte SetGeomOffset(160,120)
// and SetGeomScreen(350) (0x800846D0 / 0x800846F0, both RE'd and 0-diff), so the port can record them
// where they are set and never ask the GTE what it currently holds.
//
// WHY "VALID" IS A SEPARATE BIT AND NOT A DEFAULT. The stock values are OFX 160 / OFY 120 / H 350. If
// a fresh ProjParams reported those, "the game has not set a projection yet" and "the game set the
// stock projection" would be the same observation, and an RE gap would render a plausible picture
// instead of announcing itself. So the negative case is asserted directly: a fresh instance must NOT
// answer 160/120/350, and must report itself invalid until BOTH setters have run. Both classes are
// exercised — the invalid instance and the fully-set one — because a validity flag that is never seen
// false is not a check.
#include "testutil.h"

#include "proj_params.h"

// Fresh state: invalid, and NOT silently pre-loaded with the stock projection. This is the negative
// the whole design rests on — if it can't be told apart from a real set, the flag buys nothing.
static void test_a_fresh_instance_is_invalid_and_not_the_stock_values(void) {
  ProjParams pp;
  CHECK(!pp.geomValid());
  // The stock Tomba!2 projection is exactly 160/120/350. None of those may appear by default, or
  // "never set" reads as "set to stock".
  CHECK(pp.geomOfx() != 160.0f);
  CHECK(pp.geomOfy() != 120.0f);
  CHECK(pp.geomH() != 350.0f);
}

// Validity needs BOTH setters. The offset and the screen distance are separate libgte calls at
// separate guest addresses, and a camera built from one of them is a camera with a made-up half.
static void test_validity_requires_both_setters(void) {
  ProjParams offset_only;
  offset_only.setGeomOffset(160.0f, 120.0f);
  CHECK(!offset_only.geomValid());

  ProjParams screen_only;
  screen_only.setGeomScreen(350.0f);
  CHECK(!screen_only.geomValid());

  ProjParams both;
  both.setGeomOffset(160.0f, 120.0f);
  both.setGeomScreen(350.0f);
  CHECK(both.geomValid());
}

// The stock projection round-trips exactly. These are the audited 0-diff values (engine-ownership
// audit rows 40-41), so a correct capture reproduces the current image bit-for-bit.
static void test_the_stock_projection_round_trips(void) {
  ProjParams pp;
  pp.setGeomOffset(160.0f, 120.0f);
  pp.setGeomScreen(350.0f);
  CHECK_EQ((int)pp.geomOfx(), 160);
  CHECK_EQ((int)pp.geomOfy(), 120);
  CHECK_EQ((int)pp.geomH(), 350);
}

// WIDESCREEN PASSES THROUGH UNCHANGED. The projection center is widened at the SOURCE (the game's
// offset setter writes nw/2 — 214 at 16:9, 280 at 21:9) precisely so the guest GTE and every native
// re-projection agree on ONE center. If this channel re-derived or "corrected" the center, the 3D
// would reproject against a different center than the guest packets and double-image by ~54px
// (journal 5900/5926). So the widened value must arrive here verbatim.
static void test_a_widened_center_is_stored_verbatim(void) {
  ProjParams wide;
  wide.setGeomOffset(214.0f, 120.0f); // 16:9
  wide.setGeomScreen(350.0f);
  CHECK_EQ((int)wide.geomOfx(), 214);
  CHECK_EQ((int)wide.geomOfy(), 120);

  ProjParams ultrawide;
  ultrawide.setGeomOffset(280.0f, 120.0f); // 21:9
  ultrawide.setGeomScreen(350.0f);
  CHECK_EQ((int)ultrawide.geomOfx(), 280);
}

// A LATER SetGeomScreen REPLACES H AND TOUCHES NOTHING ELSE. This is not hypothetical: the per-area
// view init (Tomba!2 Pool::finalViewInit) runs SetGeomScreen a second time with the area's own draw
// range — 233 in one area, 350 elsewhere — long after the offset was set at display init. If the
// second call reset or invalidated the offset, every area that narrows its draw range would lose its
// projection center.
static void test_a_second_screen_call_replaces_only_H(void) {
  ProjParams pp;
  pp.setGeomOffset(160.0f, 120.0f);
  pp.setGeomScreen(350.0f);
  pp.setGeomScreen(233.0f); // the narrowed per-area draw range
  CHECK_EQ((int)pp.geomH(), 233);
  CHECK_EQ((int)pp.geomOfx(), 160);
  CHECK_EQ((int)pp.geomOfy(), 120);
  CHECK(pp.geomValid());
}

int main(void) {
  RUN(a_fresh_instance_is_invalid_and_not_the_stock_values);
  RUN(validity_requires_both_setters);
  RUN(the_stock_projection_round_trips);
  RUN(a_widened_center_is_stored_verbatim);
  RUN(a_second_screen_call_replaces_only_H);
  return pt_summary();
}
