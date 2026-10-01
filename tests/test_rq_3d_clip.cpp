// test_rq_3d_clip.cpp — a 3D prim's draw-area clip on a GUEST-WIDENED picture.
//
// WHY THIS TEST EXISTS. Spyro 2 runs on the GTE path and its own title-owned `GuestProjectionPlan`
// widens the guest's projection (CR24/OFX to the widened centre), so its primitives arrive at the
// queue ALREADY in wide coordinates: 684 columns wide, while the guest still states its own drawing
// rectangle through GP1 E3/E4 at `x1 = 511`. Measured on Spyro 2's Glimmer: 1970 prims per frame
// spanning x -170..652, 82 of them past 511, and 0.0 % non-black across the columns 512..683.
//
// The 2D half of this problem is `test_rq_2d_clip.cpp` and it is solved at a DIFFERENT point: a 2D
// prim authored in 4:3 space needs its vertices AND its clip MOVED together. A 3D prim needs
// neither — its vertices came out of the widened projection — but it does need a clip rectangle wide
// enough to contain them. `RenderQueue::emitOrQueue` now applies `GuestProjectionPlan::guestClipRight`
// to `RQ_OM_DEPTH` items, and this test is that rule's evidence.
//
// The two directions are the test, because "a prim survives at 16:9" is only half a claim:
//   * at 16:9 the clip is the plan's edge, so a triangle whose right vertex is at 652 keeps all of
//     itself instead of being cut at 511;
//   * at 4:3 nothing runs, so the very same triangle is cut at exactly the guest's 511 — which is
//     what makes "wider" a change to the PICTURE and not a change to the rules.
#include "game.h"
#include "guest_widescreen_projection.h"
#include "mods.h"
#include "render_mode.h"
#include "render_queue.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr int kNativeWidth = 512; // Spyro 2's own 4:3 framebuffer width
constexpr int kGuestClipRight = kNativeWidth - 1;
constexpr int kWideWidth = 684; // 512 * 4/3, the smallest even width that reaches 16:9

// The triangle the measurement found: its right vertex is at 652, past the guest's own 511.
constexpr int kPrimLeft = 459;
constexpr int kPrimRight = 652;
constexpr int kPrimTop = 57;
constexpr int kPrimBottom = 59;

struct Submitted {
  int queued;
  int x0;
  int x1;
  int da_x0;
  int da_x1;
};

// One guest-widened GTE-path Core with a latched plan, or a 4:3 Core with the same guest clip.
//
// `aspect` is the USER's setting and `latch` says whether the title published a matching guest
// projection. They are deliberately separate arguments: the third case below proves the branch is
// gated on the TITLE having published, not merely on the user having asked for 16:9.
Submitted submitTerrainTriangle(int aspect, bool latch) {
  auto game = std::make_unique<Game>();
  game->gpu.s_disp_w = kNativeWidth;
  game->mods.aspect = aspect;
  // The GTE path is what `guestWidescreenAllowed()` answers for, and it is the only path on which a
  // title-owned guest projection is permitted to publish at all.
  game->core.rsub.mode.setPath(RenderPath::Gte);
  if (latch) {
    game->guestDisplay.latch(guest_projection_plan(GuestProjectionInputs{
        .path = RenderPath::Gte,
        .requested = aspect == ASPECT_4_3 ? PresentationAspect::Standard4x3 : PresentationAspect::Wide16x9,
        .nativePresentation = {kNativeWidth, 240},
        .nativeProjection = {{kNativeWidth, 240}, kNativeWidth},
    }));
  }
  Core &core = game->core;
  const int xs[4] = {kPrimLeft, kPrimRight, kPrimLeft, kPrimRight};
  const int ys[4] = {kPrimTop, kPrimTop, kPrimBottom, kPrimBottom};
  const int uv[4] = {0, 0, 0, 0};
  const unsigned char rgb[4] = {0x80, 0x80, 0x80, 0x80};
  game->rq.emitOrQueue(&core,
                       1,
                       RQ_WORLD,
                       RQ_OM_DEPTH,
                       4,
                       0,
                       0,
                       xs,
                       ys,
                       nullptr,
                       nullptr,
                       uv,
                       uv,
                       rgb,
                       rgb,
                       rgb,
                       nullptr,
                       // mode, tp_x, tp_y, clut_x, clut_y, tw_mx, tw_my, tw_ox, tw_oy,
                       // da_x0, da_y0 — then the CLIP the guest stated.
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       kGuestClipRight,
                       227,
                       0);
  const RqItem &item = game->rq.items[0];
  return Submitted{game->rq.n, item.xs[0], item.xs[1], item.da_x0, item.da_x1};
}

} // namespace

// THE POSITIVE. At 16:9 with a published guest projection, a triangle whose right vertex is at 652
// keeps the whole of itself: the clip is the plan's last column, not the guest's.
static void test_wide_keeps_a_depth_prim_past_the_guest_clip(void) {
  const Submitted wide = submitTerrainTriangle(ASPECT_16_9, true);
  CHECK_EQ(wide.queued, 1);
  // The VERTICES are untouched: they came out of the widened projection already. Moving them here
  // would be the second margin this rule exists to avoid (the kanban #73 double shift, 2D side).
  CHECK_EQ(wide.x0, kPrimLeft);
  CHECK_EQ(wide.x1, kPrimRight);
  // The left edge is left alone for the same reason the right one moves: only the RIGHT edge bounds
  // the extra columns, and a title's own left edge is still inside the widened canvas.
  CHECK_EQ(wide.da_x0, 0);
  CHECK_EQ(wide.da_x1, kWideWidth - 1);
  // The number the whole rule exists to produce: the prim is no longer cut before its own extent.
  CHECK(wide.da_x1 >= wide.x1);
}

// THE NEGATIVE, with its denominator. At 4:3 the branch does not execute: the identical submission
// is clipped at exactly the guest's own 511, so a console-shaped picture is bit-for-bit the console's.
static void test_four_three_clips_at_the_guest_edge(void) {
  const Submitted narrow = submitTerrainTriangle(ASPECT_4_3, true);
  CHECK_EQ(narrow.queued, 1);
  CHECK_EQ(narrow.x0, kPrimLeft);
  CHECK_EQ(narrow.x1, kPrimRight);
  CHECK_EQ(narrow.da_x0, 0);
  CHECK_EQ(narrow.da_x1, kGuestClipRight);
  CHECK(narrow.da_x1 < narrow.x1); // still cut — the 4:3 picture loses that vertex, as it must
}

// THE GATE IS THE TITLE'S PUBLICATION, NOT THE USER'S SETTING. 16:9 requested with no latched guest
// projection leaves the clip alone: the framework presents a wider canvas it must not invent geometry
// for, and this rule does not run on an unlatched plan.
static void test_unlatched_plan_leaves_the_clip_alone(void) {
  const Submitted unlatched = submitTerrainTriangle(ASPECT_16_9, false);
  CHECK_EQ(unlatched.queued, 1);
  CHECK_EQ(unlatched.da_x1, kGuestClipRight);
}

// NEVER NARROWS. A guest that already stated a wider rectangle than the plan keeps its own: the plan
// is a floor, not a target, and clamping down would crop a title that draws wide on purpose.
static void test_a_wider_guest_rectangle_is_left_alone(void) {
  auto game = std::make_unique<Game>();
  game->core.rsub.mode.setPath(RenderPath::Gte);
  game->guestDisplay.latch(guest_projection_plan(GuestProjectionInputs{
      .path = RenderPath::Gte,
      .requested = PresentationAspect::Standard4x3, // the plan is NOT a widening
      .nativePresentation = {kNativeWidth, 240},
      .nativeProjection = {{kNativeWidth, 240}, kNativeWidth},
  }));
  CHECK(!game->guestDisplay.plan().widescreen());
  Core &core = game->core;
  const int xs[4] = {0, 700, 0, 700};
  const int ys[4] = {0, 0, 10, 10};
  const int uv[4] = {0, 0, 0, 0};
  const unsigned char rgb[4] = {0, 0, 0, 0};
  game->rq.emitOrQueue(&core,
                       1,
                       RQ_WORLD,
                       RQ_OM_DEPTH,
                       4,
                       0,
                       0,
                       xs,
                       ys,
                       nullptr,
                       nullptr,
                       uv,
                       uv,
                       rgb,
                       rgb,
                       rgb,
                       nullptr,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       900,
                       227,
                       0);
  CHECK_EQ(game->rq.items[0].da_x1, 900);
}

int main(void) {
  RUN(wide_keeps_a_depth_prim_past_the_guest_clip);
  RUN(four_three_clips_at_the_guest_edge);
  RUN(unlatched_plan_leaves_the_clip_alone);
  RUN(a_wider_guest_rectangle_is_left_alone);
  return pt_summary();
}