#include "testutil.h"

#include "gte_preop_observer.h"

namespace {

struct Hit {
  int calls = 0;
  Core* core = nullptr;
  uint64_t ordinal = 0;
  uint32_t pc = 0;
  uint32_t insn = 0;
  int value = 0;
  int order[2] = {};
};

void record(Core* core, uint64_t ordinal, uint32_t pc, uint32_t insn, void* user) {
  Hit* hit = static_cast<Hit*>(user);
  hit->calls++;
  hit->core = core;
  hit->ordinal = ordinal;
  hit->pc = pc;
  hit->insn = insn;
}

void record_pre(Core* core, uint64_t ordinal, uint32_t pc, uint32_t insn, void* user) {
  Hit* hit = static_cast<Hit*>(user);
  hit->core = core;
  hit->ordinal = ordinal;
  hit->pc = pc;
  hit->insn = insn;
  hit->order[0] = ++hit->calls;
  hit->value = 10;
}

void record_post(Core* core, uint64_t ordinal, uint32_t pc, uint32_t insn, void* user) {
  Hit* hit = static_cast<Hit*>(user);
  hit->core = core;
  hit->ordinal = ordinal;
  hit->pc = pc;
  hit->insn = insn;
  hit->order[1] = ++hit->calls;
  hit->value += 100;
}

void test_unarmed_is_a_real_negative_not_a_silent_counter() {
  GtePreOpObserver observer;
  Hit hit;
  Core* fake = reinterpret_cast<Core*>(0xC0FFEE00u);
  observer.observe(fake, 0x80012340u, 0x4A280030u);
  observer.observePost(fake, 0, 0x80012340u, 0x4A280030u);
  CHECK(!observer.armed());
  CHECK_EQ(observer.seen(), 0u);
  CHECK_EQ(hit.calls, 0);
}

void test_paired_observer_preserves_identity_and_pre_post_order() {
  GtePreOpObserver observer;
  Hit hit;
  Core* fake = reinterpret_cast<Core*>(0xC0FFEE00u);
  observer.arm(record_pre, record_post, &hit);
  observer.observeAround(fake, 0x80024528u, 0x4A180001u, [&hit] {
    CHECK_EQ(hit.value, 10);
    hit.value += 1;
  });
  CHECK_EQ(observer.seen(), 1u);
  CHECK_EQ(hit.order[0], 1);
  CHECK_EQ(hit.order[1], 2);
  CHECK_EQ(hit.value, 111);
  CHECK(hit.core == fake);
  CHECK_EQ(hit.ordinal, 1u);
  CHECK_EQ(hit.pc, 0x80024528u);
  CHECK_EQ(hit.insn, 0x4A180001u);
}

void test_armed_observer_gets_preop_identity_and_a_denominator() {
  GtePreOpObserver observer;
  Hit hit;
  Core* fake = reinterpret_cast<Core*>(0xC0FFEE00u);
  observer.arm(record, &hit);
  observer.observe(fake, 0x80024510u, 0x4A280030u);
  observer.observe(fake, 0x80024528u, 0x4A180001u);
  CHECK(observer.armed());
  CHECK_EQ(observer.seen(), 2u);
  CHECK_EQ(hit.calls, 2);
  CHECK(hit.core == fake);
  CHECK_EQ(hit.ordinal, 2u);
  CHECK_EQ(hit.pc, 0x80024528u);
  CHECK_EQ(hit.insn, 0x4A180001u);
  CHECK_EQ(observer.disarm(), 2u);
  observer.observe(fake, 0xDEADBEEFu, 0u);
  CHECK_EQ(observer.seen(), 2u);
  CHECK_EQ(hit.calls, 2);
}

}  // namespace

int main() {
  RUN(unarmed_is_a_real_negative_not_a_silent_counter);
  RUN(armed_observer_gets_preop_identity_and_a_denominator);
  RUN(paired_observer_preserves_identity_and_pre_post_order);
  return pt_summary();
}
