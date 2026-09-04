// test_texpage_dither_source — which GP0 command is allowed to change the dither-enable bit?
//
// REGRESSION UNDER TEST (Tomba!2's in-game START page on PSXPORT_RENDER_PATH=psx, 2026-08-20).
// A texpage word reaches the GPU by TWO routes, and they are not equivalent:
//
//   GP0(0xE1) DrawMode         beetle's Command_DrawMode: SetTPage(cmdw), THEN dtd = (cmdw >> 9) & 1
//   a primitive's embedded word beetle's SetTPage alone — page, mode, blend, tex-disable. NOT dtd.
//
// So on real hardware the dither-enable bit is owned by DrawMode and nothing else. A textured
// polygon carries a texpage word in vertex 1's UV slot, and bit 9 of THAT word means nothing.
//
// We applied it from both routes, so the first textured polygon whose embedded word happened to
// have bit 9 clear switched dithering off for everything drawn afterwards.
//
// MEASURED, and this is what makes it a defect rather than a reading of someone else's source: at
// f1090 of replays/bugs/ingame-options-page.pad, ours and the beetle GPU oracle differed on 27,561
// of 524,288 pixels (5.26%) with the feed proven complete (971 prims = 971 dispatched). The per-cell
// differ rate over a 76,800-pixel denominator rose monotonically with the magnitude of that cell's
// dither offset — 7.0% at |offset| 0, then 22.2%, 38.4%, 51.1%, 56.9% — which is the signature of
// one side dithering and the other not. Re-running with beetle's dither forced off collapsed the
// difference to 5,602 (1.07%), naming OUR side as the one that had stopped dithering. A pixel trace
// at (40,160) then showed `dith=false` on a textured, shaded pixel.
//
// Hermetic: texpage decoding is pure bit-twiddling on a GpuState. No GPU, no window, no disc.
//
// NEGATIVE-RESULT DISCIPLINE: both directions of both routes are asserted. A fix that simply stopped
// updating dither anywhere would pass a suite that only checked "the primitive route must not set
// it", and would silently disable dithering for good.
#include "../runtime/psx/gpu_native_internal.h"
#include "testutil.h"

// Bit 9 set, page 0, 4bpp — the word a DrawMode command would carry to turn dithering ON.
static const uint16_t kDitherOn = (uint16_t)(1u << 9);
static const uint16_t kDitherOff = 0u;

static void test_draw_mode_owns_the_dither_bit(void) {
  GpuState g;
  g.set_texpage(kDitherOff, GpuState::TexPageFrom::DrawMode);
  CHECK_EQ(g.s_tp_dither, 0);
  g.set_texpage(kDitherOn, GpuState::TexPageFrom::DrawMode);
  CHECK_EQ(g.s_tp_dither, 1);
  // ...and back off again: DrawMode must be able to CLEAR it, not only set it.
  g.set_texpage(kDitherOff, GpuState::TexPageFrom::DrawMode);
  CHECK_EQ(g.s_tp_dither, 0);
}

static void test_a_primitives_embedded_word_never_touches_it(void) {
  GpuState g;
  g.set_texpage(kDitherOn, GpuState::TexPageFrom::DrawMode);
  CHECK_EQ(g.s_tp_dither, 1);
  // THE REGRESSION: a textured polygon whose embedded word has bit 9 clear must leave dither alone.
  g.set_texpage(kDitherOff, GpuState::TexPageFrom::Primitive);
  CHECK_EQ(g.s_tp_dither, 1);
  // ...and the converse, so the rule is "ignore", not "force on": with dither off, an embedded word
  // carrying bit 9 must not switch it on either.
  g.set_texpage(kDitherOff, GpuState::TexPageFrom::DrawMode);
  CHECK_EQ(g.s_tp_dither, 0);
  g.set_texpage(kDitherOn, GpuState::TexPageFrom::Primitive);
  CHECK_EQ(g.s_tp_dither, 0);
}

static void test_the_primitive_route_still_updates_everything_else(void) {
  GpuState g;
  // page x=3 (=192), y=1 (=256), blend mode 2, colour mode 1 (8bpp) — none of which DrawMode owns
  // exclusively. If the fix had made the primitive route a no-op, this is what would break.
  const uint16_t tp = (uint16_t)(3u | (1u << 4) | (2u << 5) | (1u << 7));
  g.set_texpage(tp, GpuState::TexPageFrom::Primitive);
  CHECK_EQ(g.s_tp_x, 192);
  CHECK_EQ(g.s_tp_y, 256);
  CHECK_EQ(g.s_tp_blend, 2);
  CHECK_EQ(g.s_tp_mode, 1);
}

int main(void) {
  RUN(draw_mode_owns_the_dither_bit);
  RUN(a_primitives_embedded_word_never_touches_it);
  RUN(the_primitive_route_still_updates_everything_else);
  return pt_summary();
}
