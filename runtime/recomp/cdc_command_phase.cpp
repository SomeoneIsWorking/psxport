#include "cdc_command_phase.h"

#include <algorithm>
#include <cassert>

namespace {

uint8_t pop_parameter(CdcState *state) {
  const uint8_t value = state->param[0];
  std::move(state->param + 1, state->param + state->param_n, state->param);
  --state->param_n;
  return value;
}

} // namespace

void cdc_command_schedule(CdcState *state, uint8_t command) {
  assert(state->tick_now != nullptr && "CDC command scheduling requires the bound guest clock");
  state->pending_command = command;
  state->command_phase = -1;
  state->command_arg_n = 0;
  state->command_event_armed = 1;
  state->command_deadline_ticks = state->tick_now(state->tick_context) + kCdcCommandWritePhaseCpuTicks;
}

CdcCommandEvent cdc_command_service(CdcState *state, bool completion_can_raise_irq) {
  if (!state->command_event_armed || !state->tick_now) {
    return CdcCommandEvent::kNone;
  }

  const uint64_t now_ticks = state->tick_now(state->tick_context);
  while (now_ticks >= state->command_deadline_ticks) {
    switch (state->command_phase) {
    case -1:
      if (state->param_n > 0) {
        state->command_arg_latch = pop_parameter(state);
        state->command_phase = 0;
        state->command_deadline_ticks += kCdcCommandArgumentPhaseCpuTicks;
      } else {
        state->command_phase = 1;
        state->command_deadline_ticks += kCdcCommandExecutionPhaseCpuTicks;
      }
      break;
    case 0:
      if (state->command_arg_n < sizeof state->command_args) {
        state->command_args[state->command_arg_n++] = state->command_arg_latch;
      }
      if (state->param_n > 0) {
        state->command_arg_latch = pop_parameter(state);
        state->command_deadline_ticks += kCdcCommandArgumentPhaseCpuTicks;
      } else {
        state->command_phase = 1;
        state->command_deadline_ticks += kCdcCommandExecutionPhaseCpuTicks;
      }
      break;
    case 1:
      return CdcCommandEvent::kExecute;
    case 2:
      if (!completion_can_raise_irq) {
        return CdcCommandEvent::kNone;
      }
      state->command_event_armed = 0;
      state->command_deadline_ticks = 0;
      return CdcCommandEvent::kComplete;
    default:
      state->command_event_armed = 0;
      state->command_deadline_ticks = 0;
      return CdcCommandEvent::kNone;
    }
  }
  return CdcCommandEvent::kNone;
}

void cdc_command_finish_execution(CdcState *state, uint64_t completion_delay_ticks) {
  if (completion_delay_ticks == 0) {
    state->command_event_armed = 0;
    state->command_deadline_ticks = 0;
    return;
  }
  state->command_phase = 2;
  state->command_deadline_ticks += completion_delay_ticks;
}
