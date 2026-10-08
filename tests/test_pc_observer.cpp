#include "pc_observer.h"
#include "testutil.h"

namespace {
struct Hit {
  int calls = 0;
  Core *core = nullptr;
  uint64_t ordinals[2]{};
  uint32_t pcs[2]{};
};
void record(Core *c, uint64_t ordinal, uint32_t pc, void *user) {
  auto &h = *static_cast<Hit *>(user);
  if (h.calls < 2) {
    h.ordinals[h.calls] = ordinal;
    h.pcs[h.calls] = pc;
  }
  ++h.calls;
  h.core = c;
}
void test_unarmed_zero_callbacks() {
  PcObserver o;
  Hit h;
  o.observe(reinterpret_cast<Core *>(1), 0x80010000u);
  CHECK_EQ(o.seen(), 0u);
  CHECK_EQ(o.matched(), 0u);
  CHECK_EQ(h.calls, 0);
}
void test_two_targets_exact_core_pc_and_order() {
  PcObserver o;
  Hit h;
  auto *c = reinterpret_cast<Core *>(0xC0FFEE00u);
  const uint32_t targets[] = {0x80011000u, 0x80011008u};
  CHECK(o.arm(targets, 2, record, &h));
  o.observe(c, targets[0]);
  o.observe(c, targets[1]);
  CHECK_EQ(o.seen(), 2u);
  CHECK_EQ(o.matched(), 2u);
  CHECK_EQ(h.calls, 2);
  CHECK(h.core == c);
  CHECK_EQ(h.ordinals[0], 1u);
  CHECK_EQ(h.ordinals[1], 2u);
  CHECK_EQ(h.pcs[0], targets[0]);
  CHECK_EQ(h.pcs[1], targets[1]);
}
void test_missed_target_is_explicit_negative() {
  PcObserver o;
  Hit h;
  const uint32_t target = 0x80011000u;
  CHECK(o.arm(&target, 1, record, &h));
  o.observe(reinterpret_cast<Core *>(1), 0x80011004u);
  CHECK_EQ(o.seen(), 1u);
  CHECK_EQ(o.matched(), 0u);
  CHECK_EQ(h.calls, 0);
}
void test_interpreter_filter_does_not_count_unrelated_pc() {
  PcObserver o;
  Hit h;
  const uint32_t target = 0x80011000u;
  CHECK(o.arm(&target, 1, record, &h));
  const uint32_t unrelated = 0x80011004u;
  if (o.matches(unrelated)) {
    o.observe(reinterpret_cast<Core *>(1), unrelated);
  }
  CHECK_EQ(o.seen(), 0u);
  CHECK_EQ(o.matched(), 0u);
  CHECK_EQ(h.calls, 0);
}
void test_empty_and_overflow_refuse() {
  PcObserver o;
  Hit h;
  uint32_t targets[PcObserver::kMaxTargets + 1]{};
  CHECK(!o.arm(targets, 0, record, &h));
  CHECK(!o.armed());
  CHECK(!o.arm(targets, PcObserver::kMaxTargets + 1, record, &h));
  CHECK(!o.armed());
}
} // namespace
int main() {
  RUN(unarmed_zero_callbacks);
  RUN(two_targets_exact_core_pc_and_order);
  RUN(missed_target_is_explicit_negative);
  RUN(interpreter_filter_does_not_count_unrelated_pc);
  RUN(empty_and_overflow_refuse);
  return pt_summary();
}
