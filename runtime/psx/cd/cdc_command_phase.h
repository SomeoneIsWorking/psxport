// Oracle-derived CD command receive/argument/execute timing.
#pragma once

#include <cstdint>

#include "cdc_state.h"

constexpr uint64_t kCdcCommandWritePhaseCpuTicks = 10'500u + 1'815u;
constexpr uint64_t kCdcCommandArgumentPhaseCpuTicks = 1'815u;
constexpr uint64_t kCdcCommandExecutionPhaseCpuTicks = 8'500u;

constexpr uint64_t cdc_command_ack_delay_cpu_ticks(uint8_t argument_count) {
  return kCdcCommandWritePhaseCpuTicks + static_cast<uint64_t>(argument_count) * kCdcCommandArgumentPhaseCpuTicks +
         kCdcCommandExecutionPhaseCpuTicks;
}

enum class CdcCommandEvent : uint8_t {
  kNone,
  kExecute,
  kComplete,
};

void cdc_command_schedule(CdcState *state, uint8_t command);
CdcCommandEvent cdc_command_service(CdcState *state, bool completion_can_raise_irq);
void cdc_command_finish_execution(CdcState *state, uint64_t completion_delay_ticks);
