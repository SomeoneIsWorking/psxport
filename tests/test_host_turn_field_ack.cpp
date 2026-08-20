// test_host_turn_field_ack.cpp — deterministic field acknowledgement / timer restart gate.
//
// The shipping condition-variable timer takes its decisions from host_turn_plan.h. These tests feed
// that unit integer timestamps directly: no clock, thread, sleep, or scheduler tolerance.
#include "host_turn_plan.h"
#include "testutil.h"

static constexpr int64_t PERIOD = 500;

static void test_delivery_clears_only_the_host_bit(void) {
  constexpr int IRQ = 1, HOST = 2, OTHER = 4;
  CHECK_EQ(host_turn_pending_after_field(IRQ | HOST | OTHER, HOST), IRQ | OTHER);
  CHECK_EQ(host_turn_pending_after_field(IRQ | OTHER, HOST), IRQ | OTHER);
}

static void test_delivery_restarts_generation_and_deadline(void) {
  const HostTurnClockState before = host_turn_clock_start(/*now=*/100, PERIOD);
  CHECK_EQ(before.generation, 0);
  CHECK_EQ(before.deadline_ns, 600);
  const HostTurnClockState after = host_turn_clock_field_delivered(before, /*explicit field=*/500, PERIOD);
  CHECK_EQ(after.generation, 1);
  CHECK_EQ(after.deadline_ns, 1000); // complete new period, not the old deadline at 600
}

static void test_stale_waiter_restarts_instead_of_arming(void) {
  HostTurnClockState state = host_turn_clock_start(100, PERIOD);
  const uint64_t captured = state.generation;
  state = host_turn_clock_field_delivered(state, 500, PERIOD);
  // Even after the stale deadline (600), generation mismatch cancels this wake.
  CHECK_EQ((int)host_turn_wake_action(captured, state, 650, false), (int)HostTurnWakeAction::Restart);
}

static void test_new_deadline_arms_and_starts_the_following_period(void) {
  HostTurnClockState state = host_turn_clock_start(100, PERIOD);
  state = host_turn_clock_field_delivered(state, 500, PERIOD);
  CHECK_EQ((int)host_turn_wake_action(state.generation, state, 999, false),
           (int)HostTurnWakeAction::Restart); // early/spurious wake is not a field
  CHECK_EQ((int)host_turn_wake_action(state.generation, state, 1000, false), (int)HostTurnWakeAction::Arm);
  state = host_turn_clock_armed(state, 1000, PERIOD);
  CHECK_EQ(state.generation, 1);
  CHECK_EQ(state.deadline_ns, 1500);
}

static void test_stop_beats_every_other_wake_reason(void) {
  const HostTurnClockState state = host_turn_clock_start(100, PERIOD);
  CHECK_EQ((int)host_turn_wake_action(/*stale=*/99, state, 9999, true), (int)HostTurnWakeAction::Stop);
}

// Suite-owned negative discriminator: this is the complete old acknowledgement behavior—identity.
// It must preserve both defects, and therefore contradict both fixed postconditions.
static void test_old_behavior_fails_clear_and_restart_properties(void) {
  constexpr int IRQ = 1, HOST = 2;
  const int old_pending_after_delivery = IRQ | HOST;
  const HostTurnClockState old_clock_after_delivery = host_turn_clock_start(100, PERIOD);
  CHECK(old_pending_after_delivery != host_turn_pending_after_field(IRQ | HOST, HOST));
  const HostTurnClockState fixed = host_turn_clock_field_delivered(old_clock_after_delivery, 500, PERIOD);
  CHECK(old_clock_after_delivery.generation != fixed.generation);
  CHECK(old_clock_after_delivery.deadline_ns != fixed.deadline_ns);
  CHECK_EQ(old_clock_after_delivery.deadline_ns, 600); // stale deadline the defect retained
  CHECK_EQ(fixed.deadline_ns, 1000);
}

int main() {
  RUN(delivery_clears_only_the_host_bit);
  RUN(delivery_restarts_generation_and_deadline);
  RUN(stale_waiter_restarts_instead_of_arming);
  RUN(new_deadline_arms_and_starts_the_following_period);
  RUN(stop_beats_every_other_wake_reason);
  RUN(old_behavior_fails_clear_and_restart_properties);
  return pt_summary();
}
