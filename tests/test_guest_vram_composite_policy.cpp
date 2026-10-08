#include "guest_vram_composite_policy.h"
#include "testutil.h"

namespace {

void test_cold_and_stable_native_ownership() {
  GuestVramCompositePolicy policy;

  const GuestVramCompositePlan cold = policy.plan(false);
  CHECK(cold.rebuildForOwnership);
  CHECK(!cold.uploadWholeVram);

  policy.didBuild(false);
  const GuestVramCompositePlan stable = policy.plan(false);
  CHECK(!stable.rebuildForOwnership);
  CHECK(!stable.uploadWholeVram);
}

void test_both_ownership_transitions_rebuild_the_composite() {
  GuestVramCompositePolicy policy;
  policy.didBuild(false);

  const GuestVramCompositePlan toGuest = policy.plan(true);
  CHECK(toGuest.rebuildForOwnership);
  CHECK(toGuest.uploadWholeVram);

  policy.didBuild(true);
  const GuestVramCompositePlan stableGuest = policy.plan(true);
  CHECK(!stableGuest.rebuildForOwnership);
  CHECK(!stableGuest.uploadWholeVram);

  const GuestVramCompositePlan toNative = policy.plan(false);
  CHECK(toNative.rebuildForOwnership);
  CHECK(!toNative.uploadWholeVram);
}

void test_policy_state_is_per_game_instance() {
  GuestVramCompositePolicy first;
  GuestVramCompositePolicy second;
  first.didBuild(true);
  second.didBuild(false);

  CHECK(!first.plan(true).rebuildForOwnership);
  CHECK(first.plan(false).rebuildForOwnership);
  CHECK(!second.plan(false).rebuildForOwnership);
  CHECK(second.plan(true).rebuildForOwnership);
}

} // namespace

int main() {
  RUN(cold_and_stable_native_ownership);
  RUN(both_ownership_transitions_rebuild_the_composite);
  RUN(policy_state_is_per_game_instance);
  return pt_summary();
}
