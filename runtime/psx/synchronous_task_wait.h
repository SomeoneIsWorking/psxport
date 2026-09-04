#pragma once

#include <stdint.h>

class PcScheduler;

struct SyncWaitCompletion {
  bool stamped = false;
  uint16_t stamp = 0;
  uint16_t waitTicks = 0;
  uint16_t loadingServices = 0;
};

// Owns the native synchronous form of FUN_80044BD4. The generated/oracle body
// remains the asynchronous reference; native callers always complete the
// spawned task before returning.
class SynchronousTaskWait {
public:
  static SyncWaitCompletion finish(PcScheduler &scheduler, uint32_t taskBase, uint32_t flag);
  static void run(PcScheduler &scheduler, uint32_t fn, uint32_t p2, uint32_t p3, uint32_t flag);

private:
  static void runSlot(PcScheduler &scheduler, int slot);
};
