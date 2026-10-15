#include "cdc_command_phase.h"
#include "cdc_state.h"

#include <algorithm>
#include <cassert>

namespace psx::cd {

uint8_t CdController::popParameter() {
  const uint8_t value = param[0];
  std::move(param + 1, param + param_n, param);
  --param_n;
  return value;
}

void CdController::scheduleCommand(uint8_t command) {
  assert(tick_now != nullptr && "CDC command scheduling requires the bound guest clock");
  pending_command = command;
  command_phase = -1;
  command_arg_n = 0;
  command_event_armed = 1;
  command_deadline_ticks = tick_now(tick_context) + kCdcCommandWritePhaseCpuTicks;
}

CdcCommandEvent CdController::serviceCommand(bool completion_can_raise_irq) {
  if (!command_event_armed || !tick_now) {
    return CdcCommandEvent::None;
  }

  const uint64_t now_ticks = tick_now(tick_context);
  while (now_ticks >= command_deadline_ticks) {
    switch (command_phase) {
    case -1:
      if (param_n > 0) {
        command_arg_latch = popParameter();
        command_phase = 0;
        command_deadline_ticks += kCdcCommandArgumentPhaseCpuTicks;
      } else {
        command_phase = 1;
        command_deadline_ticks += kCdcCommandExecutionPhaseCpuTicks;
      }
      break;
    case 0:
      if (command_arg_n < sizeof command_args) {
        command_args[command_arg_n++] = command_arg_latch;
      }
      if (param_n > 0) {
        command_arg_latch = popParameter();
        command_deadline_ticks += kCdcCommandArgumentPhaseCpuTicks;
      } else {
        command_phase = 1;
        command_deadline_ticks += kCdcCommandExecutionPhaseCpuTicks;
      }
      break;
    case 1:
      return CdcCommandEvent::Execute;
    case 2:
      if (!completion_can_raise_irq) {
        return CdcCommandEvent::None;
      }
      command_event_armed = 0;
      command_deadline_ticks = 0;
      return CdcCommandEvent::Complete;
    default:
      command_event_armed = 0;
      command_deadline_ticks = 0;
      return CdcCommandEvent::None;
    }
  }
  return CdcCommandEvent::None;
}

void CdController::finishCommandExecution(uint64_t completion_delay_ticks) {
  if (completion_delay_ticks == 0) {
    command_event_armed = 0;
    command_deadline_ticks = 0;
    return;
  }
  command_phase = 2;
  command_deadline_ticks += completion_delay_ticks;
}

} // namespace psx::cd
