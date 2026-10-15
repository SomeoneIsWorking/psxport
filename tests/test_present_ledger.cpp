// test_present_ledger — a producer the composed present drew nothing for is reported; a matched one is not.
#include "present_ledger.h"
#include "testutil.h"

namespace {

using psx::present::DrawPrimitive;
using psx::present::FrameRecord;
using psx::present::PresentLedger;
using psx::present::RecordKey;

// `count` keyed triangles drawn by `producer`.
struct Keyed {
  std::uint32_t producer = 0;
  int count = 0;
};

void addKeyed(FrameRecord &record, Keyed keyed) {
  for (int i = 0; i < keyed.count; ++i) {
    DrawPrimitive primitive;
    primitive.vertexCount = 3;
    primitive.key = RecordKey{.producer = keyed.producer, .object = static_cast<std::uint32_t>(i)};
    record.append(primitive);
  }
}

void test_a_recorded_producer_the_composer_dropped_is_reported(void) {
  FrameRecord shown(1, true);
  addKeyed(shown, {0x10, 642});
  addKeyed(shown, {0x20, 21});
  FrameRecord composed(1, true);
  addKeyed(composed, {0x10, 642});
  PresentLedger ledger;
  const std::vector<std::uint32_t> dropped = ledger.reconcile(shown, composed, 930, false);
  CHECK_EQ(static_cast<int>(dropped.size()), 1);
  CHECK_EQ(dropped.front(), 0x20u);
  CHECK_EQ(static_cast<int>(ledger.framesDropped()), 1);
  CHECK_EQ(static_cast<int>(ledger.framesReconciled()), 1);
}

void test_a_matched_present_does_not_cry_wolf(void) {
  FrameRecord shown(2, true);
  addKeyed(shown, {0x10, 5});
  addKeyed(shown, {0x30, 7});
  FrameRecord composed(2, true);
  addKeyed(composed, {0x10, 1});
  addKeyed(composed, {0x30, 9});
  PresentLedger ledger;
  CHECK(ledger.reconcile(shown, composed, 2, false).empty());
  CHECK_EQ(static_cast<int>(ledger.framesDropped()), 0);
}

// The composer may add a producer's state the shown record lacked; that is not a drop.
void test_composed_without_recorded_is_not_a_drop(void) {
  FrameRecord shown(3, true);
  FrameRecord composed(3, true);
  addKeyed(composed, {0x40, 4});
  PresentLedger ledger;
  CHECK(ledger.reconcile(shown, composed, 3, false).empty());
}

void test_unkeyed_primitives_are_not_counted(void) {
  FrameRecord shown(4, true);
  shown.append(DrawPrimitive{});
  FrameRecord composed(4, true);
  PresentLedger ledger;
  CHECK(ledger.reconcile(shown, composed, 4, false).empty());
}

} // namespace

int main(void) {
  RUN(a_recorded_producer_the_composer_dropped_is_reported);
  RUN(a_matched_present_does_not_cry_wolf);
  RUN(composed_without_recorded_is_not_a_drop);
  RUN(unkeyed_primitives_are_not_counted);
  return psx::test::summary();
}
