#pragma once

#include <cstdint>

#include "cdc_state.h"

struct CdcTestClock {
  uint64_t ticks = 0;
};

namespace psx::test {

inline uint64_t cdcClockNow(void *context) {
  return static_cast<CdcTestClock *>(context)->ticks;
}

inline void bindCdcClock(psx::cd::CdController *cdc, CdcTestClock *clock) {
  cdc->bindTickSource(clock, cdcClockNow);
}

inline int serviceCdcDeadline(psx::cd::CdController *cdc, CdcTestClock *clock) {
  clock->ticks = cdc->drive_deadline_ticks;
  return cdc->service();
}

} // namespace psx::test
