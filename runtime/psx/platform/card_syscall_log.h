// Counts every BIOS card syscall, handled or not, and reports the totals at teardown.
#pragma once

#include <cstdint>

namespace psx::platform::card {

// Which BIOS vector a call arrived on. The same function number means different things on each.
enum class Vector : uint8_t { A0, B0 };

class SyscallLog {
public:
  // Record one dispatch. `handled` is what the dispatcher is about to return: false means the
  // runtime has no implementation and the guest is about to be told the call was not taken.
  void record(Vector vector, uint32_t function, bool handled);

  // Emit the totals on the `card` channel, with the denominator. Always emits, including when
  // nothing was called at all -- that is a finding, not an absence.
  void report(const char *when) const;

  uint32_t calls() const {
    return mCalls;
  }
  uint32_t unhandled() const {
    return mUnhandled;
  }

private:
  // Function numbers are a byte, so a flat table per vector is exact and needs no map.
  static constexpr uint32_t kFunctions = 256;
  uint32_t mByFunction[2][kFunctions] = {};
  uint32_t mUnhandledByFunction[2][kFunctions] = {};
  uint32_t mCalls = 0;
  uint32_t mUnhandled = 0;
};

} // namespace psx::platform::card
