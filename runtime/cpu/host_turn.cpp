#include "host_turn.h"

#include "core.h"
#include "emulated_time.h"
#include "game.h"
#include "r3000.h"

#include <lucent/log.h>

#include <limits>

namespace psx::cpu {
namespace {

std::uint64_t guestNow(const Core &core) {
  return core.game->timing.emulatedCpuTicks();
}

} // namespace

void HostTurnClock::rearm(const Core &core) {
  deadlineTicks_ = guestNow(core) + periodTicks_;
}

void HostTurnClock::arm(Core &core, NativeFunction function, unsigned fieldRateMilliHz) {
  if (function == nullptr || fieldRateMilliHz == 0) {
    lucent::error("hostturn", "refusing to arm: a null handler or a zero field rate ({} mHz)", fieldRateMilliHz);
    return;
  }
  if (function_ != nullptr) {
    lucent::warn("hostturn", "core={} already has a host clock; ignoring the second arm", static_cast<void *>(&core));
    return;
  }
  function_ = function;
  periodTicks_ = psx::frame::displayFieldCpuTicks(1, 1, fieldRateMilliHz);
  rearm(core);
  lucent::info("hostturn",
               "host turn armed at {}.{:03} Hz: one field per {} guest ticks",
               fieldRateMilliHz / 1000,
               fieldRateMilliHz % 1000,
               periodTicks_);
}

void HostTurnClock::reset() {
  *this = HostTurnClock{};
}

void HostTurnClock::requestWhenDue(Core &core) const {
  if (function_ == nullptr || guestNow(core) < deadlineTicks_) {
    return;
  }
  const int previous = __atomic_fetch_or(&core.pending_work, Core::PW_HOST, __ATOMIC_RELAXED);
  if ((previous & Core::PW_HOST) == 0) {
    lucent::debug("hostturn",
                  "field due: now={} deadline={} (overrun {} ticks)",
                  guestNow(core),
                  deadlineTicks_,
                  guestNow(core) - deadlineTicks_);
  }
}

std::uint64_t HostTurnClock::ticksUntilDue(const Core &core) const {
  if (function_ == nullptr) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  const std::uint64_t now = guestNow(core);
  return now < deadlineTicks_ ? deadlineTicks_ - now : 0;
}

void HostTurnClock::onDisplayField(Core &core) {
  if (function_ == nullptr) {
    return;
  }
  rearm(core);
  __atomic_and_fetch(&core.pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);
}

void HostTurnClock::service(Core &core) {
  // PW_HOST stays latched while deferred, as hardware keeps VBlank pending until interrupts return.
  if (!core.game->hle.canDispatchInterrupt(core)) {
    return;
  }
  if (function_ != nullptr && inTurn_) {
    lucent::debug("hostturn", "turn deferred: nested inside the handler at pc=0x{:08X}", core.pc);
    return;
  }
  __atomic_and_fetch(&core.pending_work, ~Core::PW_HOST, __ATOMIC_RELAXED);
  if (function_ == nullptr) {
    return;
  }
  inTurn_ = true;
  {
    const GuestContextScope context(core);
    function_(&core);
  }
  inTurn_ = false;
  if (deadlineTicks_ <= guestNow(core)) {
    rearm(core);
  }
  lucent::debug("hostturn",
                "turn served at pc=0x{:08X}: now={} next deadline={} pending=0x{:X}",
                core.pc,
                guestNow(core),
                deadlineTicks_,
                core.pending_work);
}

} // namespace psx::cpu
