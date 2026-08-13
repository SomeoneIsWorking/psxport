#include "testutil.h"

#include "gte_preop_observer.h"

namespace {

struct Hit {
  int calls = 0;
  Core* core = nullptr;
  uint64_t ordinal = 0;
  uint32_t pc = 0;
  uint32_t insn = 0;
};

void record(Core* core, uint64_t ordinal, uint32_t pc, uint32_t insn, void* user) {
  Hit* hit = static_cast<Hit*>(user);
  hit->calls++;
  hit->core = core;
  hit->ordinal = ordinal;
  hit->pc = pc;
  hit->insn = insn;
}

void test_unarmed_is_a_real_negative_not_a_silent_counter() {
  GtePreOpObserver observer;
  Hit hit;
  Core* fake = reinterpret_cast<Core*>(0xC0FFEE00u);
  observer.observe(fake, 0x80012340u, 0x4A280030u);
  CHECK(!observer.armed());
  CHECK_EQ(observer.seen(), 0u);
  CHECK_EQ(hit.calls, 0);
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
  return pt_summary();
}
