// host_turn_plan.h — pure state transitions for the host field clock.
//
// `host_turn.cpp` owns the real steady_clock and condition variable. This file owns the decisions
// around them as integer timestamps so cancellation/restart can be tested without sleeping.
#pragma once

#include <stdint.h>

struct HostTurnClockState {
  uint64_t generation = 0;
  int64_t deadline_ns = 0;
};

enum class HostTurnWakeAction : uint8_t { Stop, Restart, Arm };

inline HostTurnClockState host_turn_clock_start(int64_t now_ns, int64_t period_ns) {
  return {0, now_ns + period_ns};
}

// An explicitly delivered field is the new clock boundary. Changing generation invalidates a waiter
// holding the old deadline; replacing the deadline starts a complete period at this boundary.
inline HostTurnClockState host_turn_clock_field_delivered(HostTurnClockState state,
                                                           int64_t now_ns, int64_t period_ns) {
  ++state.generation;
  state.deadline_ns = now_ns + period_ns;
  return state;
}

// A timer-delivered field also starts the next complete period. It does not change generation:
// generation identifies cancellation by another owner, not ordinary timer progress.
inline HostTurnClockState host_turn_clock_armed(HostTurnClockState state,
                                                int64_t now_ns, int64_t period_ns) {
  state.deadline_ns = now_ns + period_ns;
  return state;
}

inline HostTurnWakeAction host_turn_wake_action(uint64_t captured_generation,
                                                const HostTurnClockState& current,
                                                int64_t now_ns, bool stop) {
  if (stop) return HostTurnWakeAction::Stop;
  if (captured_generation != current.generation) return HostTurnWakeAction::Restart;
  // Defensive against a spurious wake (or a clock backend returning early): an unexpired deadline
  // is never a field. The condition-variable predicate normally filters this before it gets here.
  if (now_ns < current.deadline_ns) return HostTurnWakeAction::Restart;
  return HostTurnWakeAction::Arm;
}

inline int host_turn_pending_after_field(int pending, int host_bit) {
  return pending & ~host_bit;
}
