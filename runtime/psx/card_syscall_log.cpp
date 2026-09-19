#include "card_syscall_log.h"

#include <lucent/log.h>

#include <string>

namespace psxport::card {
namespace {

constexpr int index(Vector vector) {
  return vector == Vector::A0 ? 0 : 1;
}

const char *vectorName(int slot) {
  return slot == 0 ? "A0" : "B0";
}

std::string histogram(const uint32_t (&table)[2][256]) {
  std::string out;
  for (int slot = 0; slot < 2; slot++) {
    for (uint32_t fn = 0; fn < 256; fn++) {
      if (table[slot][fn] == 0) {
        continue;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += lucent::format("{}:0x{:02X}={}", vectorName(slot), fn, table[slot][fn]);
    }
  }
  return out.empty() ? std::string("none") : out;
}

} // namespace

void SyscallLog::record(Vector vector, uint32_t function, bool handled) {
  if (function >= kFunctions) {
    return;
  }
  const int slot = index(vector);
  mByFunction[slot][function]++;
  mCalls++;
  if (!handled) {
    mUnhandledByFunction[slot][function]++;
    mUnhandled++;
  }
  lucent::debug("card",
                "{}:0x{:02X} {}",
                vectorName(slot),
                function,
                handled ? "handled" : "NOT HANDLED — the guest is being told this call was not taken");
}

void SyscallLog::report(const char *when) const {
  lucent::info("card",
               "BIOS card syscalls ({}): {} call(s), {} unhandled. called: {}. unhandled: {}",
               when,
               mCalls,
               mUnhandled,
               histogram(mByFunction),
               histogram(mUnhandledByFunction));
}

} // namespace psxport::card
