#include "guest_redraw.h"

#include "emit_memory.h"
#include "emulated_time.h"
#include "execution_control.h"
#include "execution_exit.h"
#include "game.h"
#include "native_dispatch.h"

#include <lucent/log.h>

#include <algorithm>

namespace psx::present {

GuestRedraw::GuestRedraw(Core &core)
    : core_(core), gte_(core.gte()), hi_(core.hi), lo_(core.lo), pc_(core.pc), writerLatch_(core.writerLatch),
      journalScope_(core, journal_) {
  std::copy_n(core.r, gpr_.size(), gpr_.begin());
  std::copy_n(core.cop0, cop0_.size(), cop0_.begin());
  core_.writeRollback.arm();
}

GuestRedraw::~GuestRedraw() {
  core_.writeRollback.rollBack();
  std::copy(gpr_.begin(), gpr_.end(), core_.r);
  std::copy(cop0_.begin(), cop0_.end(), core_.cop0);
  core_.hi = hi_;
  core_.lo = lo_;
  core_.pc = pc_;
  core_.writerLatch = writerLatch_;
}

void GuestRedraw::keep(std::uint32_t address, std::uint32_t bytes) {
  core_.keepStores({address, bytes});
}

void GuestRedraw::keepNothing() {
  core_.writeRollback.keepNothing();
}

void GuestRedraw::writeWord(std::uint32_t address, std::uint32_t word) {
  core_.mem_w32(address, word);
}

void GuestRedraw::writeByte(std::uint32_t address, std::uint8_t byte) {
  core_.mem_w8(address, byte);
}

void GuestRedraw::writeHalf(std::uint32_t address, std::uint16_t half) {
  core_.mem_w16(address, half);
}

void GuestRedraw::writeVertex(std::uint32_t address, const gte::ProjectedVertex &vertex) {
  EmitMemory(core_).storeVertex(address, vertex);
}

void GuestRedraw::writeCopy(std::uint32_t command, const VramCopy &copy) {
  const auto field = [](int x, float subX, int y, float subY) {
    const std::uint32_t word =
        (static_cast<std::uint32_t>(y) & 0xFFFFu) << 16 | (static_cast<std::uint32_t>(x) & 0xFFFFu);
    return gte::ProjectedVertex(word, GtePlace{.x = static_cast<float>(x) + subX, .y = static_cast<float>(y) + subY});
  };
  const CopyPlace &place = copy.place;
  EmitMemory memory(core_);
  memory.storeVertex(command + 4u, field(copy.srcX, place.srcX, copy.srcY, place.srcY));
  memory.storeVertex(command + 8u, field(copy.dstX, place.dstX, copy.dstY, place.dstY));
  memory.storeVertex(command + 12u, field(copy.width, place.width, copy.height, place.height));
}

void GuestRedraw::applied(std::span<const std::uint32_t> callees) {
  for (const std::uint32_t callee : callees) {
    if (core_.nativeDispatcher().classify(callee) == cpu::GuestHostDispatchKind::ExecuteGuest) {
      lucent::error("redraw", "applied callee 0x{:08X} is guest code, not a host service", callee);
      std::abort();
    }
  }
  journal_.withhold(callees);
}

bool GuestRedraw::run(std::uint32_t entry, std::span<const std::uint32_t> arguments) {
  return call(entry, arguments, Call::Original);
}

bool GuestRedraw::dispatch(std::uint32_t entry, std::span<const std::uint32_t> arguments) {
  return call(entry, arguments, Call::Dispatched);
}

bool GuestRedraw::dispatchTo(std::uint32_t entry,
                             std::uint32_t end,
                             std::span<const std::uint32_t> arguments,
                             std::span<const Skip> skips) {
  const std::uint32_t sp = core_.r[cpu::kSp];
  const std::uint32_t ra = core_.r[cpu::kRa];
  bool reached = call(entry, arguments, Call::ToEnd, skips.empty() ? end : skips.front().at);
  for (std::size_t next = 1; reached && next <= skips.size(); ++next) {
    const Skip &skip = skips[next - 1];
    // Read before the host, whose own calls move ra.
    const std::uint32_t resume = skip.resume != 0 ? skip.resume : core_.r[cpu::kRa];
    if (skip.host) {
      skip.host();
    }
    const std::uint32_t stop = next < skips.size() ? skips[next].at : end;
    reached = call(entry, {}, Call::ToEnd, stop, resume);
  }
  core_.r[cpu::kSp] = sp;
  core_.r[cpu::kRa] = ra;
  return reached;
}

bool GuestRedraw::resumeTo(std::uint32_t entry, std::uint32_t from, std::uint32_t end) {
  const std::uint32_t sp = core_.r[cpu::kSp];
  const std::uint32_t ra = core_.r[cpu::kRa];
  const bool reached = call(entry, {}, Call::ToEnd, end, from);
  core_.r[cpu::kSp] = sp;
  core_.r[cpu::kRa] = ra;
  return reached;
}

bool GuestRedraw::call(
    std::uint32_t entry, std::span<const std::uint32_t> arguments, Call kind, std::uint32_t end, std::uint32_t from) {
  std::copy(arguments.begin(), arguments.end(), core_.r + cpu::kA0);
  // No call inside it can suspend at a turn's end; a second of guest time only stops a runaway.
  const cpu::ExecutionBudget runaway = cpu::ExecutionBudget::fromCycles(frame::kNominalPsxCpuHz);
  const cpu::ExecutionControl::Unsuspendable unsuspendable(core_.executionControl(), runaway);
  try {
    const cpu::ExecutionResult result =
        kind == Call::Dispatched ? cpu::dispatchGuest(core_, entry, runaway, "redraw")
        : kind == Call::ToEnd
            ? cpu::resumeGuestToReturn(core_, cpu::GuestCallSite{entry, end}, from != 0 ? from : entry, runaway)
            : cpu::callOriginal(core_, entry, runaway);
    if (!result.returned()) {
      lucent::warn("redraw",
                   "the redraw of 0x{:08X} stopped: {} at 0x{:08X}",
                   entry,
                   cpu::executionExitName(result.reason),
                   result.guestPc);
      core_.game->ownerJournal.record(
          core_, {.reason = OwnerReason::ReplayRefused, .entry = entry, .sampleAddress = result.guestPc}, false);
      return false;
    }
  } catch (const cpu::DiscardedRunTouchedDevice &) {
    // A host body reached a device; the destructor puts the run's state back.
    return reachedNoDevice(entry);
  }
  return reachedNoDevice(entry);
}

bool GuestRedraw::reachedNoDevice(std::uint32_t entry) {
  const std::optional<cpu::SideEffect> &touched = journal_.discardStop();
  if (!touched) {
    return true;
  }
  lucent::warn("redraw",
               "the redraw of 0x{:08X} reached {} (stopped at pc 0x{:08X}, ra 0x{:08X})",
               entry,
               cpu::describeSideEffect(*touched),
               core_.pc,
               core_.r[31]);
  core_.game->ownerJournal.record(
      core_, {.reason = OwnerReason::ReplayRefused, .entry = entry, .sampleAddress = touched->address}, false);
  return false;
}

} // namespace psx::present
