// test_producer_scope.cpp — the native render path's current-producer stack and PC-only ids.
#include "producer_scope.h"
#include "testutil.h"
#include <string.h>

// A scope restores the enclosing producer on exit rather than clearing it.
static void test_scope_sets_and_restores(void) {
  ProducerScopeState st;
  CHECK(!st.active());
  CHECK_EQ(st.currentAddr(), 0u);
  {
    ProducerScope outer(&st, 0x8002BC9Cu, "radialPlumeRender");
    CHECK(st.active());
    CHECK_EQ(st.currentAddr(), 0x8002BC9Cu);
    CHECK(strcmp(st.currentName(), "radialPlumeRender") == 0);
    {
      ProducerScope inner(&st, 0x80027768u, "meshQuadRecordsEmit");
      CHECK_EQ(st.currentAddr(), 0x80027768u);
    }
    CHECK_EQ(st.currentAddr(), 0x8002BC9Cu);
    CHECK(strcmp(st.currentName(), "radialPlumeRender") == 0);
  }
  CHECK(!st.active());
  CHECK_EQ(st.currentAddr(), 0u);
}

// PC-only ids are deterministic, never zero, and a separate space from guest addresses.
static void test_pc_producer_iids_are_stable_and_distinct(void) {
  static_assert(producer_iid("pc/margin-render") != 0u, "an iid of 0 would be a zero key");
  static_assert(producer_iid("pc/margin-render") == producer_iid("pc/margin-render"), "stable");
  static_assert(producer_iid("pc/margin-render") != producer_iid("pc/pillarbox-fill"), "distinct");
  CHECK(producer_iid("") != 0u);
  CHECK_EQ(pc_producer("pc/margin-render").iid, producer_iid("pc/margin-render"));
  CHECK(strcmp(pc_producer("pc/margin-render").name, "pc/margin-render") == 0);
  const uint32_t iid = producer_iid("pc/margin-render");
  CHECK(!(ProducerKey::native(iid) == ProducerKey::guest(iid)));
}

// A PC-only scope nested in a guest scope restores the guest key, and has no guest address.
static void test_pc_only_scope_nests_and_restores(void) {
  ProducerScopeState st;
  static constexpr PcProducer kMargin = pc_producer("pc/margin-render");
  {
    ProducerScope outer(&st, 0x8003CCA4u, "perObjRenderDispatch");
    {
      ProducerScope inner(&st, kMargin);
      CHECK(st.currentKey().isNativeOnly());
      CHECK_EQ(st.currentAddr(), 0u);
    }
    CHECK(st.currentKey() == ProducerKey::guest(0x8003CCA4u));
  }
  CHECK(!st.currentKey().valid());
}

int main(void) {
  RUN(scope_sets_and_restores);
  RUN(pc_producer_iids_are_stable_and_distinct);
  RUN(pc_only_scope_nests_and_restores);
  return pt_summary();
}
