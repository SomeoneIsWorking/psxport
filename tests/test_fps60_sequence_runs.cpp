// The captured-frame run grouping the fps60 presenter's diagnostic dump iterates.
//
// The discriminator this file exists for: two producers drawing adjacently into ONE layer with the
// same ownership answer must come out as TWO runs. Grouping by layer and ownership alone produced
// one run that named neither, which is how 1,613,113 verbatim items in a measured Spyro replay could
// be counted without anything being able to say whose they were.
#include "fps60_sequence_runs.h"

#include "testutil.h"

#include <vector>

namespace {

using psxport::fps60::groupSequenceRuns;
using psxport::fps60::SequenceRun;

RqItem item(int layer, uint32_t sequence, uint32_t producer, uint32_t node = 0) {
  RqItem result{};
  result.layer = layer;
  result.seq = sequence;
  result.draw_seq = sequence;
  result.sort_key = -1;
  result.painter_object = producer;
  result.dbg_node = node != 0 ? node : producer;
  result.has_xyf = true;
  result.nv = 3;
  return result;
}

// Place a triangle's three sub-pixel vertices so it covers the half-open box [x,x+w) x [y,y+h).
RqItem &atFloat(RqItem &target, float x, float y, float w, float h) {
  target.has_xyf = true;
  target.nv = 3;
  target.xsf[0] = x;
  target.ysf[0] = y;
  target.xsf[1] = x + w - 1.0f;
  target.ysf[1] = y;
  target.xsf[2] = x;
  target.ysf[2] = y + h - 1.0f;
  return target;
}

// The same, through the rounded integer vertices the 2D/HUD path uses.
RqItem &atInt(RqItem &target, int x, int y, int w, int h) {
  target.has_xyf = false;
  target.nv = 3;
  target.xs[0] = x;
  target.ys[0] = y;
  target.xs[1] = x + w - 1;
  target.ys[1] = y;
  target.xs[2] = x;
  target.ys[2] = y + h - 1;
  return target;
}

std::vector<SequenceRun> group(const std::vector<RqItem> &items, const std::function<bool(const RqItem &)> &owned) {
  // The grouper takes the EMITTED stream, which the presenter holds as pointers into two queues.
  std::vector<const RqItem *> stream;
  stream.reserve(items.size());
  for (const RqItem &item : items) {
    stream.push_back(&item);
  }
  std::vector<SequenceRun> runs;
  groupSequenceRuns(stream, owned, runs);
  return runs;
}

const auto nothingOwned = [](const RqItem &) {
  return false;
};

void test_two_producers_in_one_layer_are_two_runs(void) {
  const std::vector<RqItem> items{item(RQ_WORLD, 1, 0xAAAAu), item(RQ_WORLD, 2, 0xAAAAu), item(RQ_WORLD, 3, 0xBBBBu)};
  const auto runs = group(items, nothingOwned);

  CHECK_EQ(runs.size(), 2u);
  CHECK_EQ(runs[0].painterObject, 0xAAAAu);
  CHECK_EQ(runs[0].count(), 2u);
  CHECK_EQ(runs[1].painterObject, 0xBBBBu);
  CHECK_EQ(runs[1].count(), 1u);
  CHECK_EQ(runs[1].begin, 2u);
  CHECK_EQ(runs[1].end, 3u);
}

void test_the_runs_cover_every_captured_item_exactly_once(void) {
  const std::vector<RqItem> items{item(RQ_BACKGROUND, 1, 0),
                                  item(RQ_WORLD, 2, 0xAAAAu),
                                  item(RQ_WORLD, 3, 0xBBBBu),
                                  item(RQ_WORLD, 4, 0xBBBBu),
                                  item(RQ_HUD, 5, 0)};
  const auto runs = group(items, nothingOwned);

  CHECK(!runs.empty());
  std::size_t covered = 0;
  std::size_t next = 0;
  for (const auto &run : runs) {
    CHECK_EQ(run.begin, next);
    CHECK(run.end > run.begin);
    covered += run.count();
    next = run.end;
  }
  CHECK_EQ(next, items.size());
  CHECK_EQ(covered, items.size());
}

void test_ownership_splits_a_run_even_within_one_producer(void) {
  const std::vector<RqItem> items{item(RQ_WORLD, 1, 0xAAAAu), item(RQ_WORLD, 2, 0xAAAAu), item(RQ_WORLD, 3, 0xAAAAu)};
  const auto owned = [](const RqItem &candidate) {
    return candidate.seq == 1;
  };
  const auto runs = group(items, owned);

  CHECK_EQ(runs.size(), 2u);
  CHECK(runs[0].owned);
  CHECK_EQ(runs[0].count(), 1u);
  CHECK(!runs[1].owned);
  CHECK_EQ(runs[1].count(), 2u);
}

// An empty result must mean an empty frame and nothing else, so a reader can tell "captured
// nothing" from "the grouping matched nothing".
void test_an_empty_frame_produces_no_runs(void) {
  const auto runs = group({}, nothingOwned);
  CHECK_EQ(runs.size(), 0u);
}

// The grouping is by ADJACENCY, not by gathering a producer's items from across the frame: the
// captured order is the authored paint order and a run that reordered it would misreport which
// stretch of the picture replayed verbatim.
void test_a_producer_that_reappears_later_is_a_second_run(void) {
  const std::vector<RqItem> items{item(RQ_WORLD, 1, 0xAAAAu), item(RQ_WORLD, 2, 0xBBBBu), item(RQ_WORLD, 3, 0xAAAAu)};
  const auto runs = group(items, nothingOwned);

  CHECK_EQ(runs.size(), 3u);
  CHECK_EQ(runs[0].painterObject, 0xAAAAu);
  CHECK_EQ(runs[2].painterObject, 0xAAAAu);
  CHECK_EQ(runs[2].begin, 2u);
}

// Measured on Tomba! 2: every world prim in a frame shares painter object 0, so a key without the
// entity node made the whole reconstructed world one run of 643 items whose extent covered the
// picture — which names an owner no more precisely than the layer already did.
void test_two_entities_inside_one_painter_object_are_two_runs(void) {
  const std::vector<RqItem> items{
      item(RQ_WORLD, 1, 0, 0x800FD958u), item(RQ_WORLD, 2, 0, 0x800FD958u), item(RQ_WORLD, 3, 0, 0x8011AA20u)};
  const auto runs = group(items, nothingOwned);

  CHECK_EQ(runs.size(), 2u);
  CHECK_EQ(runs[0].dbgNode, 0x800FD958u);
  CHECK_EQ(runs[0].count(), 2u);
  CHECK_EQ(runs[1].dbgNode, 0x8011AA20u);
  CHECK_EQ(runs[1].count(), 1u);
}

// The other half of that discriminator: one entity's adjacent prims must stay ONE run, or a frame
// of 664 items would print 664 lines and the grouping would have stopped grouping.
void test_one_entity_stays_a_single_run(void) {
  const std::vector<RqItem> items{
      item(RQ_WORLD, 1, 0, 0x800FD958u), item(RQ_WORLD, 2, 0, 0x800FD958u), item(RQ_WORLD, 3, 0, 0x800FD958u)};
  const auto runs = group(items, nothingOwned);

  CHECK_EQ(runs.size(), 1u);
  CHECK_EQ(runs[0].count(), 3u);
  CHECK_EQ(runs[0].dbgNode, 0x800FD958u);
}

// ── the extent: what joins a stale screen tile to the producer that drew it ─────────────────────

void test_a_run_extent_is_the_union_of_its_items(void) {
  RqItem left = item(RQ_WORLD, 1, 0xAAAAu);
  RqItem right = item(RQ_WORLD, 2, 0xAAAAu);
  const std::vector<RqItem> items{atFloat(left, 10.0f, 20.0f, 8.0f, 4.0f), atFloat(right, 40.0f, 8.0f, 6.0f, 30.0f)};
  const auto runs = group(items, nothingOwned);

  CHECK_EQ(runs.size(), 1u);
  CHECK_EQ(runs[0].extent.x0, 10);
  CHECK_EQ(runs[0].extent.y0, 8);
  CHECK_EQ(runs[0].extent.x1, 46);
  CHECK_EQ(runs[0].extent.y1, 38);
  CHECK_EQ(runs[0].extent.width(), 36);
  CHECK_EQ(runs[0].extent.height(), 30);
}

// The two vertex representations are not interchangeable: the world path draws from the floats and
// the 2D/HUD path from the rounded ints, so reading the wrong pair would box the run somewhere the
// picture never had it — which is the one thing an attribution join must not do.
void test_the_extent_reads_whichever_vertices_the_rasterizer_would(void) {
  RqItem world = item(RQ_WORLD, 1, 0xAAAAu);
  RqItem hud = item(RQ_HUD, 2, 0xBBBBu);
  atFloat(world, 100.0f, 50.0f, 4.0f, 4.0f);
  atInt(hud, 7, 9, 3, 5);
  hud.xsf[0] = 900.0f; // must be ignored: has_xyf is clear
  hud.ysf[0] = 900.0f;
  const auto runs = group({world, hud}, nothingOwned);

  CHECK_EQ(runs.size(), 2u);
  CHECK_EQ(runs[0].extent.x0, 100);
  CHECK_EQ(runs[0].extent.x1, 104);
  CHECK_EQ(runs[1].extent.x0, 7);
  CHECK_EQ(runs[1].extent.y0, 9);
  CHECK_EQ(runs[1].extent.x1, 10);
  CHECK_EQ(runs[1].extent.y1, 14);
}

// The query the attribution actually makes: does this run cover the 16-pixel tile that failed to
// interpolate? It has to answer NO for a run that only touches the neighbouring tile, or every
// producer in the frame would look guilty.
void test_a_tile_query_separates_the_covering_run_from_its_neighbour(void) {
  RqItem inside = item(RQ_WORLD, 1, 0xAAAAu);
  RqItem beside = item(RQ_WORLD, 2, 0xBBBBu);
  atFloat(inside, 118.0f, 165.0f, 4.0f, 4.0f);
  atFloat(beside, 130.0f, 165.0f, 4.0f, 4.0f);
  const auto runs = group({inside, beside}, nothingOwned);

  CHECK_EQ(runs.size(), 2u);
  CHECK(runs[0].extent.intersects(112, 160, 16, 16));
  CHECK(!runs[1].extent.intersects(112, 160, 16, 16));
  CHECK(runs[1].extent.intersects(128, 160, 16, 16));
}

// A run that drew no vertices must report an EMPTY extent and match no tile. Reporting a one-pixel
// box at the origin would make it the answer to every query about the top-left corner.
void test_a_run_without_vertices_covers_nothing(void) {
  RqItem empty = item(RQ_WORLD, 1, 0xAAAAu);
  empty.nv = 0;
  const auto runs = group({empty}, nothingOwned);

  CHECK_EQ(runs.size(), 1u);
  CHECK(runs[0].extent.empty());
  CHECK_EQ(runs[0].extent.width(), 0);
  CHECK(!runs[0].extent.intersects(0, 0, 16, 16));
}

} // namespace

int main(void) {
  RUN(two_producers_in_one_layer_are_two_runs);
  RUN(the_runs_cover_every_captured_item_exactly_once);
  RUN(ownership_splits_a_run_even_within_one_producer);
  RUN(an_empty_frame_produces_no_runs);
  RUN(a_producer_that_reappears_later_is_a_second_run);
  RUN(two_entities_inside_one_painter_object_are_two_runs);
  RUN(one_entity_stays_a_single_run);
  RUN(a_run_extent_is_the_union_of_its_items);
  RUN(the_extent_reads_whichever_vertices_the_rasterizer_would);
  RUN(a_tile_query_separates_the_covering_run_from_its_neighbour);
  RUN(a_run_without_vertices_covers_nothing);
  return pt_summary();
}
