// host_turn — the per-Core host field clock: one display field owed per field period of emulated
// guest time, taken at an executor boundary outside the guest's critical sections.
#pragma once

#include "native_function.h"

#include <cstdint>

class Core;

namespace psx::cpu {

class HostTurnClock {
public:
  // Starts the clock with the handler a due field runs; a second arm on a running clock is refused.
  void arm(Core &core, NativeFunction function, unsigned fieldRateMilliHz);
  void reset();
  // Raises PW_HOST once the guest clock reaches the deadline.
  void requestWhenDue(Core &core) const;
  // Guest ticks until the next field is owed; unbounded with no handler.
  std::uint64_t ticksUntilDue(const Core &core) const;
  // A field was delivered: the next one is owed one period from now, and a latched request is cancelled.
  void onDisplayField(Core &core);
  void service(Core &core);

private:
  void rearm(const Core &core);

  NativeFunction function_ = nullptr;
  std::uint64_t periodTicks_ = 0;
  std::uint64_t deadlineTicks_ = 0;
  // The handler runs guest code that tests the gate again.
  bool inTurn_ = false;
};

} // namespace psx::cpu
