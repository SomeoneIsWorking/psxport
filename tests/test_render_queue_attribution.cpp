// test_render_queue_attribution — the full-queue attribution must tell a RUNAWAY from a CAPACITY
// shortfall, and must give both answers.
//
// WHY THIS EXISTS (Tomba! 2, 2026-09-19). The 16:9 leg of replays/bugs/machinery-cutscene.pad aborted
// at frame ~2940 with "render queue full (65536 items)"; the 4:3 leg ran the same 30,400 fields clean.
// The fatal's only account of the cause was the parenthesis "(runaway re-submission?)" — a hypothesis,
// with no counts, in the one message anyone would read afterwards. The two causes it conflates need
// opposite fixes: a runaway submit path must be found and stopped, while a genuine capacity shortfall
// means the measurement RQ_MAX was sized from no longer covers the widened frustum. Guessing wrong in
// either direction is how RQ_MAX gets raised until the symptom disappears.
//
// Hermetic: the report reads a span of RqItem and nothing else. No Core, no Game, no GPU, no disc.
#include "../runtime/psx/render_queue_attribution.h"
#include "testutil.h"

#include <vector>

using psxport::render::RenderQueueAttribution;

// One prim at a distinct position. `slot` moves the geometry, so two items built with different slots
// are genuinely different faces; two built with the same slot are the same face submitted twice.
static RqItem prim(int slot, std::uint32_t node, int layer) {
  RqItem item{};
  item.nv = 3;
  item.layer = static_cast<std::uint8_t>(layer);
  item.dbg_node = node;
  for (int v = 0; v < 3; ++v) {
    item.xs[v] = slot + v;
    item.ys[v] = slot * 3 + v;
    item.us[v] = slot & 0xFF;
    item.vs[v] = v;
  }
  return item;
}

static void test_a_frame_of_distinct_geometry_is_a_capacity_shortfall(void) {
  std::vector<RqItem> queue;
  queue.reserve(4096);
  for (int i = 0; i < 4096; ++i) {
    queue.push_back(prim(i, static_cast<std::uint32_t>(1 + (i % 64)), RQ_WORLD));
  }
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.prims(), 4096);
  CHECK_EQ(report.distinct(), 4096);
  CHECK_EQ(report.repeats(), 0);
  CHECK_EQ(report.nodes(), 64);
  CHECK(!report.looksLikeRunaway());
  CHECK(report.text().find("VERDICT: CAPACITY") != std::string::npos);
  CHECK(report.text().find("VERDICT: RUNAWAY") == std::string::npos);
}

static void test_a_scene_resubmitted_many_times_is_a_runaway(void) {
  // 64 distinct faces, pushed 64 times over: exactly the shape of a stuck render walk.
  std::vector<RqItem> queue;
  queue.reserve(4096);
  for (int pass = 0; pass < 64; ++pass) {
    for (int i = 0; i < 64; ++i) {
      queue.push_back(prim(i, 7u, RQ_WORLD));
    }
  }
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.prims(), 4096);
  CHECK_EQ(report.distinct(), 64);
  CHECK_EQ(report.repeats(), 4032);
  CHECK(report.looksLikeRunaway());
  CHECK(report.text().find("VERDICT: RUNAWAY") != std::string::npos);
}

static void test_a_repeat_is_not_hidden_by_a_changed_colour(void) {
  // A re-submitting walk can shade the second pass differently (a fade or light term advanced between
  // the two). If colour were part of the identity, that runaway would report as distinct geometry.
  std::vector<RqItem> queue;
  queue.push_back(prim(5, 3u, RQ_WORLD));
  RqItem shaded = prim(5, 3u, RQ_WORLD);
  for (int v = 0; v < 3; ++v) {
    shaded.rs[v] = 0x20;
    shaded.gs[v] = 0x40;
    shaded.bs[v] = 0x60;
    shaded.depth[v] = 0.5f;
  }
  queue.push_back(shaded);
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.distinct(), 1);
  CHECK_EQ(report.repeats(), 1);
}

static void test_moved_geometry_is_not_a_repeat(void) {
  // The mirror of the case above: identity must still separate two faces that differ only in position.
  std::vector<RqItem> queue;
  queue.push_back(prim(5, 3u, RQ_WORLD));
  queue.push_back(prim(6, 3u, RQ_WORLD));
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.distinct(), 2);
  CHECK_EQ(report.repeats(), 0);
}

// A world prim built the way drawWorldQuad builds one: sub-pixel float position is the authority and
// xs/ys are its rounded copy. `subpixel` shifts only the float position.
static RqItem worldPrim(int slot, float subpixel) {
  RqItem item = prim(slot, 9u, RQ_WORLD);
  item.has_xyf = 1;
  for (int v = 0; v < 3; ++v) {
    item.xsf[v] = static_cast<float>(item.xs[v]) + subpixel;
    item.ysf[v] = static_cast<float>(item.ys[v]);
  }
  return item;
}

static void test_two_faces_a_fraction_of_a_pixel_apart_are_not_a_repeat(void) {
  // THE INSTRUMENT'S OWN FAILURE MODE. World geometry is sub-pixel; the integer xs/ys are a rounded
  // copy. Keying on the rounded copy merges two genuinely different faces that land in the same pixel,
  // each merge is counted as a repeat, and enough of them manufacture a RUNAWAY verdict from a frame
  // that merely contains a lot of small geometry. That is a diagnostic inventing its own answer, which
  // is worse than the guess it replaced.
  std::vector<RqItem> queue;
  queue.reserve(2);
  queue.push_back(worldPrim(40, 0.10f));
  queue.push_back(worldPrim(40, 0.60f));
  CHECK_EQ(queue[0].xs[0], queue[1].xs[0]); // same pixel: the rounded copy cannot tell them apart
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.distinct(), 2);
  CHECK_EQ(report.repeats(), 0);
  CHECK(!report.looksLikeRunaway());
}

static void test_the_same_world_face_pushed_twice_is_still_a_repeat(void) {
  // The other answer: sub-pixel identity must not make every world prim look distinct, or a real
  // runaway of world geometry — which is exactly the Tomba! 2 case — would never be named.
  std::vector<RqItem> queue;
  queue.reserve(2);
  queue.push_back(worldPrim(40, 0.10f));
  queue.push_back(worldPrim(40, 0.10f));
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.distinct(), 1);
  CHECK_EQ(report.repeats(), 1);
}

static void test_a_mixed_queue_refuses_to_pick_a_side(void) {
  // Repeats present, but a minority. Neither verdict is supported, and inventing one is exactly the
  // failure this report replaces.
  std::vector<RqItem> queue;
  queue.reserve(130);
  for (int i = 0; i < 100; ++i) {
    queue.push_back(prim(i, 1u, RQ_WORLD));
  }
  for (int i = 0; i < 30; ++i) {
    queue.push_back(prim(i, 1u, RQ_WORLD));
  }
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.distinct(), 100);
  CHECK_EQ(report.repeats(), 30);
  CHECK(!report.looksLikeRunaway());
  CHECK(report.text().find("VERDICT: UNDECIDED") != std::string::npos);
}

static void test_the_layer_split_is_reported(void) {
  std::vector<RqItem> queue;
  queue.reserve(13);
  for (int i = 0; i < 10; ++i) {
    queue.push_back(prim(i, 1u, RQ_WORLD));
  }
  for (int i = 100; i < 103; ++i) {
    queue.push_back(prim(i, 2u, RQ_HUD));
  }
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(report.primsInLayer(RQ_WORLD), 10);
  CHECK_EQ(report.primsInLayer(RQ_HUD), 3);
  CHECK_EQ(report.primsInLayer(RQ_BACKGROUND), 0);
  CHECK(report.text().find("world=10") != std::string::npos);
  CHECK(report.text().find("hud=3") != std::string::npos);
}

static void test_the_largest_owner_is_named_first(void) {
  std::vector<RqItem> queue;
  queue.reserve(45);
  for (int i = 0; i < 5; ++i) {
    queue.push_back(prim(i, 11u, RQ_WORLD));
  }
  for (int i = 100; i < 140; ++i) {
    queue.push_back(prim(i, 22u, RQ_WORLD));
  }
  const RenderQueueAttribution report(queue.data(), static_cast<int>(queue.size()));
  CHECK_EQ(static_cast<int>(report.topNodes().size()), 2);
  CHECK_EQ(static_cast<int>(report.topNodes()[0].node), 22);
  CHECK_EQ(report.topNodes()[0].prims, 40);
  CHECK_EQ(static_cast<int>(report.topNodes()[1].node), 11);
}

static void test_an_empty_queue_says_so_instead_of_asserting(void) {
  // A report that cannot state its own denominator is not evidence; an empty one must still be honest
  // rather than crash inside a fatal handler, where a second fault would lose the original cause.
  const RenderQueueAttribution report(nullptr, 0);
  CHECK_EQ(report.prims(), 0);
  CHECK_EQ(report.distinct(), 0);
  CHECK(!report.looksLikeRunaway());
  CHECK(report.text().find("no owning nodes") != std::string::npos);
}

int main(void) {
  RUN(a_frame_of_distinct_geometry_is_a_capacity_shortfall);
  RUN(a_scene_resubmitted_many_times_is_a_runaway);
  RUN(a_repeat_is_not_hidden_by_a_changed_colour);
  RUN(moved_geometry_is_not_a_repeat);
  RUN(two_faces_a_fraction_of_a_pixel_apart_are_not_a_repeat);
  RUN(the_same_world_face_pushed_twice_is_still_a_repeat);
  RUN(a_mixed_queue_refuses_to_pick_a_side);
  RUN(the_layer_split_is_reported);
  RUN(the_largest_owner_is_named_first);
  RUN(an_empty_queue_says_so_instead_of_asserting);
  return pt_summary();
}
