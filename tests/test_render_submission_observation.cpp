#include "game.h"
#include "render_queue.h"
#include "testutil.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
class DiagnosticCapture {
public:
  DiagnosticCapture() {
    lucent::enable_channel("unscoped");
    lucent::set_sink([this](lucent::Level, std::string_view line) {
      lines.emplace_back(line);
    });
  }
  ~DiagnosticCapture() {
    lucent::set_sink(nullptr);
    lucent::enable_channel("unscoped", false);
  }
  std::vector<std::string> lines;
};

void submit(Core &core, RenderQueue &q) {
  const int xs[4] = {0, 8, 8, 0};
  const int ys[4] = {0, 0, 8, 8};
  const int uv[4] = {1, 2, 3, 4};
  const unsigned char rgb[4] = {64, 65, 66, 67};
  const float depth[4] = {0.2f, 0.3f, 0.4f, 0.5f};
  q.emitOrQueue(&core,
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
                depth,
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
                320,
                240,
                0);
}

void compareItems(const RqItem &left, const RqItem &right) {
  CHECK(left.layer == right.layer);
  CHECK(left.semi == right.semi);
  CHECK(left.nv == right.nv);
  CHECK(left.raw == right.raw);
  CHECK(left.order_mode == right.order_mode);
  CHECK(left.painter_flags == right.painter_flags);
  CHECK(left.shade_gouraud == right.shade_gouraud);
  CHECK(left.dither == right.dither);
  CHECK(left.painter_object == right.painter_object);
  CHECK(left.painter_replay.domain == right.painter_replay.domain);
  CHECK(left.painter_replay.key.ot_bin == right.painter_replay.key.ot_bin);
  CHECK(left.painter_replay.key.link_ordinal == right.painter_replay.key.link_ordinal);
  CHECK(left.painter_replay.key.chain_suborder == right.painter_replay.key.chain_suborder);
  CHECK(left.seq == right.seq);
  CHECK(left.draw_seq == right.draw_seq);
  CHECK(left.guest_packet == right.guest_packet);
  CHECK(left.guest_ot_order == right.guest_ot_order);
  CHECK(left.flush_ordinal == right.flush_ordinal);
  CHECK(std::equal(std::begin(left.xs), std::end(left.xs), std::begin(right.xs)));
  CHECK(std::equal(std::begin(left.ys), std::end(left.ys), std::begin(right.ys)));
  CHECK(std::equal(std::begin(left.xsf), std::end(left.xsf), std::begin(right.xsf)));
  CHECK(std::equal(std::begin(left.ysf), std::end(left.ysf), std::begin(right.ysf)));
  CHECK(left.has_xyf == right.has_xyf);
  CHECK(left.authored_depth == right.authored_depth);
  CHECK(std::equal(std::begin(left.us), std::end(left.us), std::begin(right.us)));
  CHECK(std::equal(std::begin(left.vs), std::end(left.vs), std::begin(right.vs)));
  CHECK(std::equal(std::begin(left.rs), std::end(left.rs), std::begin(right.rs)));
  CHECK(std::equal(std::begin(left.gs), std::end(left.gs), std::begin(right.gs)));
  CHECK(std::equal(std::begin(left.bs), std::end(left.bs), std::begin(right.bs)));
  CHECK(std::equal(std::begin(left.depth), std::end(left.depth), std::begin(right.depth)));
  CHECK(left.tp_x == right.tp_x);
  CHECK(left.tp_y == right.tp_y);
  CHECK(left.mode == right.mode);
  CHECK(left.clut_x == right.clut_x);
  CHECK(left.clut_y == right.clut_y);
  CHECK(left.tw_mx == right.tw_mx);
  CHECK(left.tw_my == right.tw_my);
  CHECK(left.tw_ox == right.tw_ox);
  CHECK(left.tw_oy == right.tw_oy);
  CHECK(left.da_x0 == right.da_x0);
  CHECK(left.da_y0 == right.da_y0);
  CHECK(left.da_x1 == right.da_x1);
  CHECK(left.da_y1 == right.da_y1);
  CHECK(left.tp_blend == right.tp_blend);
  CHECK(left.dbg_node == right.dbg_node);
  CHECK(left.sort_key == right.sort_key);
  CHECK(left.key_ord == right.key_ord);
  CHECK(left.sh_cast == right.sh_cast);
  CHECK(std::equal(std::begin(left.sh_vx), std::end(left.sh_vx), std::begin(right.sh_vx)));
  CHECK(std::equal(std::begin(left.sh_vy), std::end(left.sh_vy), std::begin(right.sh_vy)));
  CHECK(std::equal(std::begin(left.sh_vz), std::end(left.sh_vz), std::begin(right.sh_vz)));
}

void test_admission_preserves_observations_and_items() {
  {
    auto ordinary = std::make_unique<RenderQueue>();
    CHECK_EQ(ordinary->items[5].dbg_node, 0u);
  }
  auto game = std::make_unique<Game>();
  auto admission = std::make_unique<RenderQueue>(RenderQueue::Observation::Admission);
  auto &core = game->core;
  CHECK_EQ(admission->items[5].dbg_node, 0u);
  DiagnosticCapture diagnostics;
  for (int iteration = 0; iteration < 2; ++iteration) {
    admission->reset();
    submit(core, *admission);
    CHECK_EQ(admission->n, 1);
    CHECK_EQ(core.rsub.census.primsSeen(), 0u);
    CHECK_EQ(core.rsub.census.unscopedNative(), 0u);
    CHECK_EQ(core.rsub.census.guestOrigin(), 0u);
    CHECK_EQ(core.rsub.census.rowCount(), 0);
    CHECK(diagnostics.lines.empty());
  }
  submit(core, game->rq);
  CHECK_EQ(core.rsub.census.primsSeen(), 1u);
  CHECK_EQ(core.rsub.census.unscopedNative(), 1u);
  CHECK(!diagnostics.lines.empty());
  compareItems(admission->items[0], game->rq.items[0]);
  const auto diagnosticsBefore = diagnostics.lines.size();
  ++core.rsub.guestGp0Depth;
  submit(core, *admission);
  CHECK_EQ(core.rsub.census.guestOrigin(), 0u);
  submit(core, game->rq);
  --core.rsub.guestGp0Depth;
  CHECK_EQ(core.rsub.census.guestOrigin(), 1u);
  CHECK_EQ(core.rsub.census.primsSeen(), 1u);
  CHECK_EQ(diagnostics.lines.size(), diagnosticsBefore);
  CHECK_EQ(admission->n, game->rq.n);
  for (int index = 0; index < game->rq.n; ++index) {
    compareItems(admission->items[index], game->rq.items[index]);
  }
}
} // namespace

int main() {
  RUN(admission_preserves_observations_and_items);
  return pt_summary();
}
