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

RqItem item(int layer, uint32_t sequence, uint32_t producer) {
  RqItem result{};
  result.layer = layer;
  result.seq = sequence;
  result.draw_seq = sequence;
  result.sort_key = -1;
  result.painter_object = producer;
  result.dbg_node = producer;
  result.has_xyf = true;
  result.nv = 3;
  return result;
}

std::vector<SequenceRun> group(const std::vector<RqItem> &items, const std::function<bool(const RqItem &)> &owned) {
  std::vector<SequenceRun> runs;
  groupSequenceRuns(items, owned, runs);
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

} // namespace

int main(void) {
  RUN(two_producers_in_one_layer_are_two_runs);
  RUN(the_runs_cover_every_captured_item_exactly_once);
  RUN(ownership_splits_a_run_even_within_one_producer);
  RUN(an_empty_frame_produces_no_runs);
  RUN(a_producer_that_reappears_later_is_a_second_run);
  return pt_summary();
}
