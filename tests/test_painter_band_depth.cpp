#include "painter_band_depth.h"

#include "render_queue.h"
#include "testutil.h"

#include <memory>
#include <vector>

// A banded authored domain says: every face at one OT bin is drawn at that bin's single depth, so the
// depth buffer separates bins and never contradicts the replay. These fixtures prove the checker sees
// each way that contract can be broken — not only that it passes a clean frame, which a checker that
// never looks at anything also does.

static std::unique_ptr<RenderQueue> makeQueue() {
  auto queue = std::make_unique<RenderQueue>();
  queue->n = 0;
  queue->seq = 0;
  queue->consumed = 0;
  return queue;
}

// One face of `domain` at `bin`, declaring band depth `band` and submitting `submitted` on all four
// vertices. They differ only in the fixture that proves the declaration is checked against the queue.
static void addFace(RenderQueue &queue,
                    PainterObjectId object,
                    uint32_t seq,
                    uint32_t domain,
                    uint16_t bin,
                    uint32_t suborder,
                    float band,
                    float submitted) {
  RqItem &item = queue.items[queue.n++];
  item = RqItem{};
  item.painter_object = object;
  item.seq = seq;
  item.draw_seq = seq;
  item.layer = RQ_WORLD;
  item.order_mode = RQ_OM_DEPTH;
  item.nv = 3;
  item.mode = 3;
  item.painter_replay = {
      .domain = domain, .key = {.ot_bin = bin, .link_ordinal = 0, .chain_suborder = suborder}, .band_ord = band};
  for (int vertex = 0; vertex < 4; ++vertex) {
    item.depth[vertex] = submitted;
  }
}

static std::vector<const RqItem *> streamOf(const RenderQueue &queue) {
  std::vector<const RqItem *> stream;
  stream.reserve((size_t)queue.n);
  for (int i = 0; i < queue.n; ++i) {
    stream.push_back(&queue.items[i]);
  }
  return stream;
}

// `accepted` reports the plan's own verdict rather than asserting it here: CHECK expands to a bare
// `return`, so it belongs in the void test cases, not in a value-returning helper.
static PainterBandDepthResult check(RenderQueue &queue, bool &accepted) {
  queue.sortQueue();
  const std::vector<const RqItem *> stream = streamOf(queue);
  const PainterObjectPlan plan = planPainterItemStream(stream);
  accepted = plan.accepted();
  if (!accepted) {
    return {};
  }
  return rq_check_painter_band_depths(stream, plan);
}

// Bin 40 is farther than bin 12, so it takes the smaller depth: larger depth is nearer under
// GREATER_OR_EQUAL. Two objects, so the domain is genuinely replayed rather than trivially ordered.
static void addCleanDomain(RenderQueue &queue) {
  addFace(queue, 0x11u, 10, 0xD0u, 40, 0, 0.0100f, 0.0100f);
  addFace(queue, 0x11u, 11, 0xD0u, 40, 1, 0.0100f, 0.0100f);
  addFace(queue, 0x22u, 12, 0xD0u, 12, 0, 0.0400f, 0.0400f);
}

void test_banded_domain_passes(void) {
  auto queue = makeQueue();
  addCleanDomain(*queue);
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(result.ok());
  CHECK_EQ(result.domains, 1);
  CHECK_EQ(result.commands_scanned, 3);
  CHECK_EQ(result.banded, 3);
  CHECK_EQ(result.bins, 2);
  CHECK_EQ(result.kept_own_depth, 0);
}

// THE NEGATIVE THAT MATTERS MOST: a producer that declares no band keeps per-vertex depth and is
// COUNTED. Without this, a checker that silently passed every unbanded frame would look identical to
// one doing its job, and every measured count above would be vacuous.
void test_unbanded_domain_is_counted_not_passed_silently(void) {
  auto queue = makeQueue();
  addFace(*queue, 0x11u, 10, 0xD0u, 40, 0, 0.0f, 0.0100f);
  addFace(*queue, 0x22u, 11, 0xD0u, 12, 0, 0.0f, 0.0400f);
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(result.ok());
  CHECK_EQ(result.commands_scanned, 2);
  CHECK_EQ(result.banded, 0);
  CHECK_EQ(result.kept_own_depth, 2);
  CHECK_EQ(result.bins, 0);
}

void test_one_bin_two_depths_is_refused(void) {
  auto queue = makeQueue();
  addFace(*queue, 0x11u, 10, 0xD0u, 40, 0, 0.0100f, 0.0100f);
  addFace(*queue, 0x11u, 11, 0xD0u, 40, 1, 0.0200f, 0.0200f); // same bin, different band
  addFace(*queue, 0x22u, 12, 0xD0u, 12, 0, 0.0400f, 0.0400f);
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(!result.ok());
  CHECK_EQ((int)result.fault, (int)PainterBandDepthResult::Fault::InconsistentBin);
  CHECK_EQ((int)result.fault_bin, 40);
}

// The exact shape of Spyro issue 0120: retail put the gem at the FARTHER bin, and the port gave it
// the NEARER depth, so the depth buffer drew it through what should occlude it.
void test_farther_bin_with_nearer_depth_is_refused(void) {
  auto queue = makeQueue();
  addFace(*queue, 0x11u, 10, 0xD0u, 171, 0, 0.012709f, 0.012709f); // farther bin, nearer depth
  addFace(*queue, 0x22u, 11, 0xD0u, 105, 0, 0.009630f, 0.009630f);
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(!result.ok());
  CHECK_EQ((int)result.fault, (int)PainterBandDepthResult::Fault::NonMonotone);
  CHECK_EQ((int)result.fault_bin, 105);
}

void test_equal_depths_on_different_bins_are_refused(void) {
  auto queue = makeQueue();
  addFace(*queue, 0x11u, 10, 0xD0u, 40, 0, 0.0100f, 0.0100f);
  addFace(*queue, 0x22u, 11, 0xD0u, 12, 0, 0.0100f, 0.0100f); // merges two bins the game kept apart
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(!result.ok());
  CHECK_EQ((int)result.fault, (int)PainterBandDepthResult::Fault::NonMonotone);
}

// A declaration nothing checks is a comment. This is the fixture that makes the band a contract.
void test_declared_band_must_be_the_submitted_depth(void) {
  auto queue = makeQueue();
  addFace(*queue, 0x11u, 10, 0xD0u, 40, 0, 0.0100f, 0.0100f);
  addFace(*queue, 0x22u, 11, 0xD0u, 12, 0, 0.0400f, 0.0399f); // declared a band, submitted another
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(!result.ok());
  CHECK_EQ((int)result.fault, (int)PainterBandDepthResult::Fault::DepthNotBanded);
  CHECK_EQ((int)result.fault_bin, 12);
}

// One vertex out of four is enough: a face is banded or it is not.
void test_one_stray_vertex_depth_is_refused(void) {
  auto queue = makeQueue();
  addCleanDomain(*queue);
  queue->items[queue->n - 1].depth[2] = 0.0399f;
  bool accepted = false;
  const PainterBandDepthResult result = check(*queue, accepted);
  CHECK(accepted);
  CHECK(!result.ok());
  CHECK_EQ((int)result.fault, (int)PainterBandDepthResult::Fault::DepthNotBanded);
}

int main(void) {
  RUN(banded_domain_passes);
  RUN(unbanded_domain_is_counted_not_passed_silently);
  RUN(one_bin_two_depths_is_refused);
  RUN(farther_bin_with_nearer_depth_is_refused);
  RUN(equal_depths_on_different_bins_are_refused);
  RUN(declared_band_must_be_the_submitted_depth);
  RUN(one_stray_vertex_depth_is_refused);
  printf("test_painter_band_depth: a clean banded domain, an unbanded domain counted rather than "
         "passed silently, and five refusals: one bin with two depths, the issue-0120 shape of a "
         "farther bin given a nearer depth, two bins merged onto one depth, a declared band the "
         "queue does not carry, and a single stray vertex\n");
  return pt_summary();
}
