// A 2D prim's draw-area clip lives in the same space as its vertices. `RenderQueue::emitOrQueue`
// moves an RQ_2D_AUTHORED_4_3 prim's vertices through the widescreen 2D transform; its `da_*` clip
// must move with them, or a panel authored at x 140..372 is drawn at 226..458 and clipped at 372
// (Spyro 1's pause panel painted 147 of its 232 columns at 16:9, spyro issue 0144).
#include "game.h"
#include "mods.h"
#include "render_queue.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr int kNativeWidth = 512; // Spyro's own 4:3 framebuffer width
constexpr int kPanelLeft = 140;
constexpr int kPanelRight = 372;

struct Submitted {
  int queued;
  int x0;
  int x1;
  int daX0;
  int daX1;
};

Submitted submitPanel(int aspect) {
  auto game = std::make_unique<Game>();
  game->gpu.s_disp_w = kNativeWidth;
  game->mods.aspect = aspect;
  Core &core = game->core;
  const int xs[4] = {kPanelLeft, kPanelRight, kPanelLeft, kPanelRight};
  const int ys[4] = {40, 40, 200, 200};
  const int uv[4] = {0, 0, 0, 0};
  const unsigned char rgb[4] = {0x40, 0x40, 0x40, 0x40};
  const RenderQueue::Space2dScope authored(game->rq, RQ_2D_AUTHORED_4_3);
  game->rq.emitOrQueue(&core,
                       1,
                       RQ_HUD,
                       RQ_OM_2D_FG,
                       4,
                       1,
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
                       kPanelRight,
                       239,
                       0);
  const RqItem &item = game->rq.items[0];
  return Submitted{game->rq.n, item.xs[0], item.xs[1], item.da_x0, item.da_x1};
}

} // namespace

// 4:3 is the identity: vertices and clip are exactly what the producer passed.
static void test_four_three_leaves_vertices_and_clip_unchanged(void) {
  const Submitted narrow = submitPanel(ASPECT_4_3);
  CHECK_EQ(narrow.queued, 1);
  CHECK_EQ(narrow.x0, kPanelLeft);
  CHECK_EQ(narrow.x1, kPanelRight);
  CHECK_EQ(narrow.daX0, 0);
  CHECK_EQ(narrow.daX1, kPanelRight);
}

// 16:9 centres the panel, and the clip is centred by the same shift, so every column is inside it.
static void test_sixteen_nine_clips_to_the_transformed_rectangle(void) {
  const Submitted wide = submitPanel(ASPECT_16_9);
  CHECK_EQ(wide.queued, 1);
  const int shift = wide.x0 - kPanelLeft;
  CHECK(shift > 0);
  CHECK_EQ(wide.x1, kPanelRight + shift);
  CHECK_EQ(wide.daX0, shift);
  CHECK_EQ(wide.daX1, kPanelRight + shift);
}

int main() {
  RUN(four_three_leaves_vertices_and_clip_unchanged);
  RUN(sixteen_nine_clips_to_the_transformed_rectangle);
  return pt_summary();
}
