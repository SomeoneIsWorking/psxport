// test_rq_3d_clip.cpp — a guest-widened picture's draw-area clip.
//
// WHY THIS TEST EXISTS. Spyro 2 runs on the GTE path and its own title-owned `GuestProjectionPlan`
// widens the presentation, so its primitives arrive at the queue in WIDE coordinates: 684 columns
// wide, while the guest still states its own drawing rectangle through GP1 E3/E4 at `x1 = 511`.
// Measured on Spyro 2's Glimmer: 1970 prims per frame spanning x -170..652, 82 of them past 511, and
// ink stopping at column 597 of a 684-column presentation.
//
// The 2D half of this problem is `test_rq_2d_clip.cpp`: a 2D prim authored in 4:3 space needs its
// vertices AND its clip MOVED together. A prim on a guest-widened frame needs no vertex move — the
// 2D block above already shifted it into the wide frame — but it DOES need a clip rectangle wide
// enough to contain the result. `RenderQueue::emitOrQueue` now applies
// `GuestProjectionPlan::guestClipRight`, and this test is that rule's evidence.
//
// WHY THE FIRST VERSION OF THE RULE WAS WRONG, which is why both producer shapes are here. It was
// keyed on `order_mode == RQ_OM_DEPTH`. Instrumented on Spyro 2 with the 2D block logging its
// inputs, 775,259 submissions per run arrive as `RQ_2D_AUTHORED_4_3` / `RQ_HUD` / `RQ_OM_2D_FG`,
// because the pure GTE-path policy in `gpu_native.cpp` forces `is3d = 0` and `bg = 0` for every
// guest prim — the picture is PSX painter order by construction, and `is3d`'s own writers have no
// callers, so it reads 0 for every title on Lightrec. An order-mode key therefore excluded the very
// primitives the rule exists for: the register was demonstrably widened and the margin stayed black.
// The rule is keyed on `gpu_vk_wide_presentation`, which is the question actually being asked — "were
// these coordinates produced by a projection the host has already widened" — and every shape here
// answers it the same way.
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
constexpr int kWideWidth = 684;                          // 512 * 4/3, the smallest even width that reaches 16:9
constexpr int kMargin = (kWideWidth - kNativeWidth) / 2; // 86, what rq_2d_xform shifts by

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

// How a producer presents itself to the queue. Both shapes below are real; neither is hypothetical.
struct Shape {
  int order_mode;
  int layer;
  Rq2dSpace space;
};

// What a guest OT prim ACTUALLY is on the pure GTE path — 775,259 submissions per Spyro 2 run.
static const Shape kGuestWorldShape{RQ_OM_2D_FG, RQ_HUD, RQ_2D_AUTHORED_4_3};
// What a native-depth world prim is.
static const Shape kNativeWorldShape{RQ_OM_DEPTH, RQ_WORLD, RQ_2D_AUTHORED_4_3};
static const Shape kShapes[] = {kGuestWorldShape, kNativeWorldShape};
static const int kShapeCount = static_cast<int>(sizeof kShapes / sizeof kShapes[0]);

static Submitted submit(int aspect, bool latch, Shape shape, int guestClipRight = kGuestClipRight) {
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
  const RenderQueue::Space2dScope declared(game->rq, shape.space);
  game->rq.emitOrQueue(&core,
                       1,
                       shape.layer,
                       shape.order_mode,
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
                       guestClipRight,
                       227,
                       0);
  const RqItem &item = game->rq.items[0];
  return Submitted{game->rq.n, item.xs[0], item.xs[1], item.da_x0, item.da_x1};
}

} // namespace

// THE POSITIVE, in the shape that actually occurs. At 16:9 the clip is the plan's last column.
//
// The clip is the LAST COLUMN OF THE CANVAS, not "past the vertex": in this shape the 2D block has
// already shifted the vertices by the margin, so the prim's right vertex 652 lands at 738 — beyond
// any 684-wide canvas, and correctly so, because a guest vertex off the right edge of the 4:3
// picture is off the right edge of the wider one too. What the clip has to reach is the canvas edge,
// and that is what is asserted. The guest geometry that DOES land in the margin is the part between
// 512 and 597 in 4:3 space, which is why the clip must be 683 and not 597.
static void test_wide_keeps_a_guest_world_prim_past_the_guest_clip(void) {
  const Submitted wide = submit(ASPECT_16_9, true, kGuestWorldShape);
  CHECK_EQ(wide.queued, 1);
  CHECK_EQ(wide.x1, kPrimRight + kMargin); // the 2D block moved it into the wide frame
  CHECK_EQ(wide.da_x1, kWideWidth - 1);
  CHECK(wide.da_x1 > kNativeWidth - 1); // strictly wider than the guest's own rectangle
}

// THE SAME CLAIM FOR A DEPTH-ORDERED WORLD PRIM, so the rule is not a GTE-path special case.
static void test_wide_keeps_a_depth_prim_past_the_guest_clip(void) {
  const Submitted wide = submit(ASPECT_16_9, true, kNativeWorldShape);
  CHECK_EQ(wide.queued, 1);
  // The VERTICES are untouched: they came out of a widened projection already. Moving them here
  // would be the second margin this rule exists to avoid (the kanban #73 double shift, 2D side).
  CHECK_EQ(wide.x0, kPrimLeft);
  CHECK_EQ(wide.x1, kPrimRight);
  CHECK_EQ(wide.da_x0, 0);
  CHECK_EQ(wide.da_x1, kWideWidth - 1);
  CHECK(wide.da_x1 >= wide.x1);
}

// THE NEGATIVE, with its denominator. At 4:3 the branch does not execute: the identical submission
// is clipped at exactly the guest's own 511, so a console-shaped picture is the console's.
static void test_four_three_clips_at_the_guest_edge(void) {
  for (int s = 0; s < kShapeCount; s++) {
    const Submitted narrow = submit(ASPECT_4_3, true, kShapes[s]);
    CHECK_EQ(narrow.queued, 1);
    CHECK_EQ(narrow.x1, kPrimRight);
    CHECK_EQ(narrow.da_x1, kGuestClipRight);
    CHECK(narrow.da_x1 < narrow.x1); // still cut — the 4:3 picture loses that vertex, as it must
  }
  printf("      [4:3 identity] swept %d producer shape(s)\n", kShapeCount);
}

// THE GATE IS THE TITLE'S PUBLICATION, NOT THE USER'S SETTING. 16:9 requested with no latched guest
// projection leaves the clip alone: the framework presents a wider canvas it must not invent geometry
// for, and this rule does not run on an unlatched plan.
static void test_unlatched_plan_leaves_the_clip_alone(void) {
  for (int s = 0; s < kShapeCount; s++) {
    const Submitted unlatched = submit(ASPECT_16_9, false, kShapes[s]);
    CHECK_EQ(unlatched.queued, 1);
    CHECK_EQ(unlatched.da_x1, kGuestClipRight);
  }
  printf("      [unlatched identity] swept %d producer shape(s)\n", kShapeCount);
}

// NEVER NARROWS. A guest that already stated a wider rectangle than the plan keeps its own: the plan
// is a floor, not a target, and clamping down would crop a title that draws wide on purpose.
//
// The expected value differs per shape and the difference is the point: in the guest shape the 2D
// block has already MOVED the rectangle by the margin before this rule sees it, so 900 arrives as
// 986 and the rule must leave THAT alone. Asserting one number for both shapes would have hidden the
// only interaction between the two rules that matters.
static void test_a_wider_guest_rectangle_is_left_alone(void) {
  const Submitted depth = submit(ASPECT_16_9, true, kNativeWorldShape, 900);
  CHECK_EQ(depth.queued, 1);
  CHECK_EQ(depth.da_x1, 900);
  const Submitted guest = submit(ASPECT_16_9, true, kGuestWorldShape, 900);
  CHECK_EQ(guest.queued, 1);
  CHECK_EQ(guest.da_x1, 900 + kMargin);
  printf("      [never narrows] 2 producer shapes: 900 kept, 900 kept after the 2D shift\n");
}

int main(void) {
  RUN(wide_keeps_a_guest_world_prim_past_the_guest_clip);
  RUN(wide_keeps_a_depth_prim_past_the_guest_clip);
  RUN(four_three_clips_at_the_guest_edge);
  RUN(unlatched_plan_leaves_the_clip_alone);
  RUN(a_wider_guest_rectangle_is_left_alone);
  return pt_summary();
}