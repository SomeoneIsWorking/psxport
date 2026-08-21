#pragma once

#include <cstdint>

#include "cdc_state.h"

struct CdcTestClock {
  uint64_t ticks = 0;
};

inline uint64_t cdc_test_now(void *context) {
  return static_cast<CdcTestClock *>(context)->ticks;
}

inline void cdc_test_bind(CdcState *cdc, CdcTestClock *clock) {
  cdc_bind_tick_source(cdc, clock, cdc_test_now);
}

inline int cdc_test_service_deadline(CdcState *cdc, CdcTestClock *clock) {
  clock->ticks = cdc->drive_deadline_ticks;
  return cdc_drive_service(cdc);
}
