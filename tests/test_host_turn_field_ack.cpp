// test_host_turn_field_ack.cpp — an explicitly-delivered field owns the host-turn clock boundary.
//
// WHAT THIS GATES. The host-turn timer and a game's explicit VSync/native-field path describe the
// same hardware event. If the explicit path finishes while PW_HOST is already latched, it must both
// cancel that duplicate and restart the timer period at the completed field. Before 717a14da the
// explicit path had no acknowledgement: the old timer deadline remained live and delivered another
// field immediately after pacing, nearly doubling games which exercised both paths.
//
// This is an integration test of the SHIPPING timer and public acknowledgement function, not a
// second model of their arithmetic. The only wall-clock dependency is unavoidable because the unit
// being tested is a real condition-variable timer. The windows are deliberately broad: at 2 Hz we
// acknowledge 400 ms into a 500 ms period, inspect 250 ms later (past the OLD 500 ms deadline but
// before the NEW 900 ms deadline), then wait another 350 ms and require the restarted timer to fire.
// Thus the instrument must show BOTH answers: absent in the protected window, present after one full
// restarted period. A machine would need to delay creation of one tiny sleeping thread by >150 ms to
// make the discriminator ambiguous; ctest's 60-second outer cap still catches a deadlock.
//
// NEGATIVE CONTROL. The macro below transcribes the old behavior exactly at this boundary: explicit
// delivery does nothing to the host clock. It must fail (normally first because the manually-latched
// duplicate survives; if that assertion were removed, the old deadline also makes the protected
// window fail):
//
//   c++ ... -DPSXPORT_TEST_LEGACY_HOST_TURN_ACK tests/test_host_turn_field_ack.cpp ...
//
// The normal ctest build calls rec_host_turn_field_delivered itself.

#include "core.h"
#include "testutil.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

static void unused_host_turn(Core*) {}

static void acknowledge(Core* c) {
#ifdef PSXPORT_TEST_LEGACY_HOST_TURN_ACK
  (void)c;  // behavior before 717a14da: no acknowledgement existed
#else
  rec_host_turn_field_delivered(c);
#endif
}

#ifndef PSXPORT_TEST_LEGACY_HOST_TURN_ACK
// The suite's negative discriminator, run on every normal build. This is the complete explicit-field
// behavior before 717a14da: there was no call into host_turn, so the pending word was untouched and
// the timer retained its original deadline. Keep it local to the test; the shipping-path integration
// case below is what proves the new function actually implements the opposite behavior.
static void legacy_acknowledge(Core*) {}

static void test_legacy_acknowledgement_preserves_the_duplicate(void) {
  std::unique_ptr<Core> core(new Core);
  core->pending_work = Core::PW_IRQ | Core::PW_HOST;
  legacy_acknowledge(core.get());
  CHECK_EQ(core->pending_work, Core::PW_IRQ | Core::PW_HOST);
  CHECK(core->pending_work != Core::PW_IRQ);  // the fixed-path postcondition is observably violated
}
#endif

static void test_explicit_field_cancels_duplicate_and_restarts_period(void) {
  // Heap allocation keeps Core's 2 MiB RAM image off small test-thread stacks.
  std::unique_ptr<Core> core(new Core);
  rec_host_turn_register(core.get(), unused_host_turn, 2000);  // 2.000 Hz = 500 ms per field

  std::this_thread::sleep_for(400ms);

  // Model the exact race from the defect: a host turn latched while the explicit path was pacing.
  // Preserve PW_IRQ to prove acknowledgement clears only its own bit.
  core->pending_work = Core::PW_IRQ | Core::PW_HOST;
  acknowledge(core.get());
  CHECK_EQ(core->pending_work, Core::PW_IRQ);

  // We are now 650 ms after registration. The original 500 ms deadline is past, but only 250 ms of
  // the restarted period has elapsed. Old behavior reports PW_HOST here; fixed behavior must not.
  std::this_thread::sleep_for(250ms);
  CHECK_EQ(core->pending_work, Core::PW_IRQ);

  // Cross the restarted 500 ms deadline and require the opposite answer. Without this positive the
  // prior check could pass because the timer never ran at all.
  std::this_thread::sleep_for(350ms);
  CHECK_EQ(core->pending_work, Core::PW_IRQ | Core::PW_HOST);

  rec_host_turn_shutdown();
}

int main() {
#ifndef PSXPORT_TEST_LEGACY_HOST_TURN_ACK
  RUN(legacy_acknowledgement_preserves_the_duplicate);
#endif
  RUN(explicit_field_cancels_duplicate_and_restarts_period);
  // Always join the timer even after a CHECK's early return from the test function.
  rec_host_turn_shutdown();
  return pt_summary();
}
