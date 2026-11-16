#include "guest_executor.h"

#include "core.h"
#include "cpu_divide.h"
#include "execution_control.h"
#include "execution_ledger.h"
#include "execution_services.h"
#include "function_reach.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "guest_call_site.h"
#include "guest_memory_map.h"
#include "native_dispatch.h"
#include "segment_clock.h"
#include "side_effect_journal.h"
#include "store_observe.h"
#include "watchdog.h"

#include <lucent/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace psx::cpu {
namespace {

// Only the hardware-register window reaches io_read/io_write, the one place a guest observes elapsed time.
bool isDeviceAddress(std::uint32_t address) {
  const std::uint32_t phys = physical(address);
  return phys >= kHardwareBase && phys < kHardwareBase + kHardwareBytes;
}

constexpr std::uint32_t rsField(std::uint32_t instruction) {
  return (instruction >> 21) & 0x1Fu;
}
constexpr std::uint32_t functField(std::uint32_t instruction) {
  return instruction & 0x3Fu;
}
constexpr std::uint32_t immediate(std::uint32_t instruction) {
  return instruction & 0xFFFFu;
}
constexpr std::uint32_t signedImmediate(std::uint32_t instruction) {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(instruction & 0xFFFFu)));
}

// Primary opcodes.
enum : std::uint32_t {
  kSpecial = 0x00,
  kRegimm = 0x01,
  kJ = 0x02,
  kJal = 0x03,
  kBeq = 0x04,
  kBne = 0x05,
  kBlez = 0x06,
  kBgtz = 0x07,
  kAddi = 0x08,
  kAddiu = 0x09,
  kSlti = 0x0A,
  kSltiu = 0x0B,
  kAndi = 0x0C,
  kOri = 0x0D,
  kXori = 0x0E,
  kLui = 0x0F,
  kCop0 = 0x10,
  kCop2 = 0x12,
  kLb = 0x20,
  kLh = 0x21,
  kLwl = 0x22,
  kLw = 0x23,
  kLbu = 0x24,
  kLhu = 0x25,
  kLwr = 0x26,
  kSb = 0x28,
  kSh = 0x29,
  kSwl = 0x2A,
  kSw = 0x2B,
  kSwr = 0x2E,
  kLwc2 = 0x32,
  kSwc2 = 0x3A,
};

// SPECIAL function codes.
enum : std::uint32_t {
  kSll = 0x00,
  kSrl = 0x02,
  kSra = 0x03,
  kSllv = 0x04,
  kSrlv = 0x06,
  kSrav = 0x07,
  kJr = 0x08,
  kJalr = 0x09,
  kSyscall = 0x0C,
  kBreak = 0x0D,
  kMfhi = 0x10,
  kMthi = 0x11,
  kMflo = 0x12,
  kMtlo = 0x13,
  kMult = 0x18,
  kMultu = 0x19,
  kDiv = 0x1A,
  kDivu = 0x1B,
  kAdd = 0x20,
  kAddu = 0x21,
  kSub = 0x22,
  kSubu = 0x23,
  kAnd = 0x24,
  kOr = 0x25,
  kXor = 0x26,
  kNor = 0x27,
  kSlt = 0x2A,
  kSltu = 0x2B,
};

// Coprocessor rs fields.
enum : std::uint32_t {
  kMfc = 0x00,
  kCfc = 0x02,
  kMtc = 0x04,
  kCtc = 0x06,
};

// A jump the block cannot run past: J, JAL, JR, JALR, and the always-taken branch forms.
bool isUnconditionalJump(std::uint32_t instruction) {
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    return functField(instruction) == kJr || functField(instruction) == kJalr;
  case kJ:
  case kJal:
    return true;
  case kBeq:
  case kBlez:
    return rsField(instruction) == rtField(instruction);
  case kRegimm: {
    const std::uint32_t rt = rtField(instruction);
    return (rt == 0x01u || rt == 0x11u) && rsField(instruction) == 0;
  }
  default:
    return false;
  }
}

// An instruction that ends its block on its own: SYSCALL, or a write to Status or Cause.
bool endsBlock(std::uint32_t instruction) {
  if (primaryOpcode(instruction) == kSpecial) {
    return functField(instruction) == kSyscall;
  }
  if (primaryOpcode(instruction) == kCop0) {
    const std::uint32_t move = rsField(instruction);
    return (move == kMtc || move == kCtc) && (rdField(instruction) == kCop0Status || rdField(instruction) == kCop0Cause);
  }
  return false;
}

bool isStore(std::uint32_t op) {
  return op == kSb || op == kSh || op == kSwl || op == kSw || op == kSwr || op == kSwc2;
}

} // namespace

struct GuestExecutor::Impl {
  explicit Impl(Core &owner) : core(owner) {
    psx::platform::Watchdog::watchGuest(&enteredPc, core.r);
  }

  ~Impl() {
    psx::platform::Watchdog::forgetGuest(&enteredPc);
  }

  enum class SegmentExit : std::uint8_t {
    Budget,
    Boundary,
    CheckInterrupt,
    Syscall,
    Break,
    Fault,
  };

  struct Segment {
    SegmentExit exit = SegmentExit::Budget;
    std::uint32_t pc = 0;
    const char *fault = nullptr;
  };

  enum class BoundaryReason : std::uint8_t {
    None,
    GuestReturn,
    HostDispatch,
    PendingWork,
  };

  struct BoundaryContext {
    std::optional<std::uint32_t> returnAddress;
    bool dispatchHostServices = false;
    bool skipPendingBoundaryOnce = false;
    BoundaryReason reason = BoundaryReason::None;
    std::uint32_t pc = 0;
  };

  // A load whose value reaches its register after the next instruction (the R3000 load delay slot).
  struct DelayedLoad {
    std::uint32_t reg = 0;
    std::uint32_t value = 0;
  };

  // ---- Registers -------------------------------------------------------------------------------

  // An instruction's own register write; it overrides a delayed load of the same register.
  void setGpr(std::uint32_t reg, std::uint32_t value) {
    if (reg == 0) {
      return;
    }
    core.r[reg] = value;
    if (arriving.reg == reg) {
      arriving.reg = 0;
    }
  }

  // A load's result, visible after the next instruction. A second load of the register cancels the first.
  void delayLoad(std::uint32_t reg, std::uint32_t value) {
    if (reg == 0) {
      return;
    }
    if (arriving.reg == reg) {
      arriving.reg = 0;
    }
    pending = {reg, value};
  }

  // The register as LWL/LWR merge into it: a load still in flight to it is forwarded.
  std::uint32_t mergeBase(std::uint32_t reg) const {
    return arriving.reg == reg && reg != 0 ? arriving.value : core.r[reg];
  }

  // Lands every load still in flight, so Core holds the architectural registers at an exit.
  void settleLoads() {
    if (arriving.reg != 0) {
      core.r[arriving.reg] = arriving.value;
      arriving.reg = 0;
    }
    if (pending.reg != 0) {
      core.r[pending.reg] = pending.value;
      pending.reg = 0;
    }
  }

  void writeCop0(std::uint32_t reg, std::uint32_t value) {
    switch (reg) {
    case 1:
    case 4:
    case 8:
    case 14:
    case 15:
      return; // read-only
    default:
      break;
    }
    if (reg >= 16) {
      return;
    }
    const std::uint32_t oldStatus = core.cop0[kCop0Status];
    if (reg == kCop0Cause) {
      core.cop0[kCop0Cause] = (core.cop0[kCop0Cause] & ~0x300u) | (value & 0x300u);
    } else {
      core.cop0[reg] = value;
    }
    if (reg != kCop0Status && reg != kCop0Cause) {
      return;
    }
    const std::uint32_t status = core.cop0[kCop0Status];
    const std::uint32_t cause = core.cop0[kCop0Cause];
    // A pending software interrupt now enabled, or hardware interrupts just enabled: deliver at the block end.
    if ((status & cause & 0x300u) != 0 && (status & 1u) != 0) {
      checkInterrupt = true;
    }
    if (reg == kCop0Status && (~status & 0x401u) == 0 && (~oldStatus & 0x401u) != 0) {
      checkInterrupt = true;
    }
  }

  // ---- GTE -------------------------------------------------------------------------------------

  // A data register as MFC2/SWC2 read it; the screen FIFO is read through its projected vertex.
  std::uint32_t readGteData(std::uint32_t reg) {
    if (reg >= gte::kSxy0 && reg <= gte::kSxyP) {
      return core.gte().projected(std::min(reg, gte::kSxy2) - gte::kSxy0).word();
    }
    return core.gte().readData(reg);
  }

  // The place of `value` read from GTE data register `reg`, when that is an SXY entry still holding it.
  std::optional<NotedPlace> screenPlace(std::uint32_t reg, std::uint32_t value) {
    if (reg < gte::kSxy0 || reg > gte::kSxyP) {
      return std::nullopt;
    }
    const gte::ProjectedVertex vertex = core.gte().projected(std::min(reg, gte::kSxy2) - gte::kSxy0);
    if (vertex.word() != value) {
      return std::nullopt;
    }
    return NotedPlace{.place = vertex.place(), .site = core.writerLatch.function()};
  }

  // MFC2 of an SXY entry carries its place in the GPR it loads; any other transfer into a GPR ends what it carried.
  // CTC2 of TRZ hands the GTE the translation depth its GPR carries.
  void cop2Transfer(std::uint32_t instruction, std::uint32_t value) {
    VertexPlaces &places = core.vertexPlaces;
    const std::uint32_t reg = rtField(instruction);
    const std::uint32_t move = rsField(instruction);
    // Every TRZ write replaces the carried translation depth, tracked or not.
    if (move == kCop2Ctc2 && rdField(instruction) == gte::kTranslationZ) {
      const std::optional<NotedPlace> noted = places.tracking() ? places.carried(reg, value) : std::nullopt;
      core.gte().carryTranslation(noted ? noted->translation : GteTranslationDepth{});
      places.endFace();
      return;
    }
    if (!places.tracking()) {
      return;
    }
    if (move == kCop2Mfc2) {
      if (const std::optional<NotedPlace> noted = screenPlace(rdField(instruction), value)) {
        places.carry(reg, value, *noted);
        return;
      }
    }
    if (move == kCop2Mfc2 || move == kCop2Cfc2) {
      places.overwrote(reg);
    }
  }

  // ---- Memory ----------------------------------------------------------------------------------

  // A store's vertex place: a SWC2 of an SXY entry takes the GTE's, a SW the one its register carries.
  void storedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value) {
    VertexPlaces &places = core.vertexPlaces;
    if (!places.tracking()) {
      return;
    }
    const std::uint32_t reg = rtField(instruction);
    if (primaryOpcode(instruction) == kOpSw) {
      places.stored(reg, address, value);
      return;
    }
    if (primaryOpcode(instruction) != kOpSwc2) {
      return;
    }
    if (const std::optional<NotedPlace> noted = screenPlace(reg, value)) {
      places.note(address, value, *noted);
    }
  }

  // A load into a GPR: a whole word carries the place stored with it, anything else ends what the GPR carried.
  void loadedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value) {
    VertexPlaces &places = core.vertexPlaces;
    if (!places.tracking()) {
      return;
    }
    const std::uint32_t op = primaryOpcode(instruction);
    if (op == kOpLw) {
      if (places.mayLoadFace(instruction)) {
        places.faceLoaded({.function = core.writerLatch.function(), .opcode = instruction, .word = value});
      }
      places.loaded(rtField(instruction), address, value);
    } else if (op >= kOpLb && op <= kOpLwr) {
      places.overwrote(rtField(instruction));
    }
  }

  // Commits the guest time since the last device access so the device about to be read sees it move.
  // It never re-enters a memory access, and leaves the host turn and spin sampling to the segment end.
  void beforeAccess(std::uint32_t address) {
    ++counters.memoryAccesses;
    if (core.game == nullptr || !isDeviceAddress(address)) {
      return;
    }
    // The override differential's shadow path holds guest time; its device reads are replayed.
    if (core.sideEffectJournal != nullptr && core.sideEffectJournal->withholdsGuestTime()) {
      return;
    }
    const std::uint32_t instructions = segmentClock.commitThrough(executed);
    if (instructions == 0) {
      return;
    }
    ++counters.deviceClockCommits;
    counters.deviceClockCommitInstructions += instructions;
    core.game->timing.advanceGuestInstructionTicks(instructions);
  }

  std::uint32_t load(std::uint32_t instruction, std::uint32_t address) {
    beforeAccess(address);
    std::uint32_t value = 0;
    switch (primaryOpcode(instruction)) {
    case kLb:
    case kLbu:
      value = core.mem_r8(address);
      break;
    case kLh:
    case kLhu:
      value = core.mem_r16(address);
      break;
    default:
      value = core.mem_r32(address);
      break;
    }
    loadedPlace(instruction, address, value);
    return value;
  }

  void store(std::uint32_t instruction, std::uint32_t address, std::uint32_t value, std::uint32_t width) {
    beforeAccess(address);
    switch (width) {
    case 1:
      core.mem_w8(address, static_cast<std::uint8_t>(value), ExecutableWriteSource::Cpu);
      break;
    case 2:
      core.mem_w16(address, static_cast<std::uint16_t>(value), ExecutableWriteSource::Cpu);
      break;
    default:
      core.mem_w32(address, value, ExecutableWriteSource::Cpu);
      storedPlace(instruction, address, value);
      break;
    }
  }

  // The instruction word at `pc`: main RAM (and its mirrors) or the scratchpad, never a device or the BIOS.
  bool fetch(std::uint32_t pc, std::uint32_t &instruction) const {
    const std::uint32_t phys = physical(pc) & ~3u;
    if (phys < kRamMirrorSpan) {
      std::memcpy(&instruction, core.ram + (phys & (kMainRamBytes - 1u)), 4);
      return true;
    }
    if (phys >= kScratchpadPhysicalBase && phys < kScratchpadPhysicalBase + kScratchpadBytes) {
      std::memcpy(&instruction, core.scratch + (phys - kScratchpadPhysicalBase), 4);
      return true;
    }
    return false;
  }

  // ---- Blocks ----------------------------------------------------------------------------------

  // Whether a taken branch at `branchPc` to `target` stays inside the block that began at `blockStart`.
  // The block runs to the delay slot of its first unconditional jump, or to its first block-ending
  // instruction; everything from its start to the branch's delay slot has already run inside it.
  bool staysInBlock(std::uint32_t blockStart, std::uint32_t branchPc, std::uint32_t target) const {
    if (target < blockStart) {
      return false;
    }
    if (target <= branchPc + 4u) {
      return true;
    }
    for (std::uint32_t at = branchPc + 8u; at < target; at += 4u) {
      std::uint32_t word = 0;
      if (!fetch(at, word)) {
        return false;
      }
      if (isUnconditionalJump(word)) {
        return target <= at + 4u;
      }
      if (endsBlock(word)) {
        return target <= at;
      }
    }
    return true;
  }

  // The block entry at `guestPc`: true when the segment stops here for the boundary's reason.
  bool enterBlock(std::uint32_t guestPc) {
    ++counters.executedBlocks;
    enteredPc = guestPc;
    latchWriter(guestPc);
    if (reach) {
      reach->observe(guestPc);
    }
    BoundaryContext &boundary = *running;
    if (boundary.returnAddress && guestPc == *boundary.returnAddress) {
      boundary.reason = BoundaryReason::GuestReturn;
    } else if (boundary.skipPendingBoundaryOnce) {
      boundary.skipPendingBoundaryOnce = false;
    } else if (pendingWorkDeliverable()) {
      boundary.reason = BoundaryReason::PendingWork;
    }
    if (boundary.reason == BoundaryReason::None && boundary.dispatchHostServices &&
        core.nativeDispatcher().classify(guestPc) != GuestHostDispatchKind::ExecuteGuest) {
      boundary.reason = BoundaryReason::HostDispatch;
    }
    if (boundary.reason == BoundaryReason::None) {
      return false;
    }
    boundary.pc = guestPc;
    return true;
  }

  // Every block entry passes here, so a store is attributed to its block and enclosing call.
  void latchWriter(std::uint32_t guestPc) {
    const std::uint32_t ra = core.r[kRa];
    GuestWriterLatch &latch = core.writerLatch;
    const std::uint32_t callWord =
        latch.needsCallWord(ra) && mainRamOffset(ra - kJalReturnOffset) ? core.mem_r32(ra - kJalReturnOffset) : 0u;
    latch.enterBlock(guestPc, ra, core.r, callWord);
  }

  // ---- Store observer --------------------------------------------------------------------------

  StoreObserverTargetCounts *observedStore(std::uint32_t pc) {
    for (std::size_t i = 0; i < storeReport.targetCount; ++i) {
      if (storeReport.targets[i].guestPc == pc) {
        return &storeReport.targets[i];
      }
    }
    return nullptr;
  }

  void observeStore(StoreObserverTargetCounts &target,
                    std::uint32_t pc,
                    std::uint32_t instruction,
                    StoreObservationPhase phase,
                    std::uint32_t segmentInstructions) {
    std::array<std::uint32_t, 34> gpr{};
    std::copy_n(core.r, 32, gpr.begin());
    gpr[32] = core.lo;
    gpr[33] = core.hi;
    std::array<std::uint32_t, 32> cp0{};
    std::copy_n(core.cop0, 16, cp0.begin());
    std::array<std::uint32_t, 32> cp2Data{};
    std::array<std::uint32_t, 32> cp2Control{};
    exportGteRegisters(core.gte().registers(), cp2Data, cp2Control);
    ResolvedStoreTarget resolved;
    resolved.baseRegister = rsField(instruction);
    resolved.sourceRegister = rtField(instruction);
    resolved.displacement = static_cast<std::int32_t>(signedImmediate(instruction));
    resolved.address = gpr[resolved.baseRegister] + static_cast<std::uint32_t>(resolved.displacement);
    resolved.value = gpr[resolved.sourceRegister];
    resolved.valid = true;
    if (phase == StoreObservationPhase::Before) {
      ++target.before;
      if (target.lastTarget.valid && target.lastTarget.address != resolved.address) {
        ++target.distinctTargets;
      } else if (!target.lastTarget.valid) {
        target.firstTarget = resolved;
      }
      target.previousTarget = target.lastTarget;
      target.lastTarget = resolved;
    } else {
      ++target.after;
    }
    const StoreObservation observation{
        pc, phase, segmentInstructions, instruction, resolved, gpr, cp0, cp2Data, cp2Control};
    ++storeReport.observations;
    storeCallback(observation, storeContext);
  }

  // ---- The interpreter -------------------------------------------------------------------------

  // Runs from the block entry at `pc` until a block entry finds the segment budget spent, a stop
  // requested or a boundary reason, or an instruction leaves the interpreter (syscall, break, fault).
  Segment runSegment(std::uint32_t pc, std::uint64_t budget) {
    stopRequested.store(false, std::memory_order_relaxed);
    checkInterrupt = false;
    const std::uint64_t segmentStart = executed;
    auto leave = [&](SegmentExit exit, std::uint32_t at, const char *fault = nullptr) {
      settleLoads();
      return Segment{exit, at, fault};
    };
    if (enterBlock(pc)) {
      return leave(SegmentExit::Boundary, pc);
    }
    std::uint32_t blockStart = pc;
    std::uint32_t next = pc + 4u;
    bool inDelaySlot = false;
    // Instructions left until the block ends: 2 after a block-leaving jump (it and its delay slot), 1 after
    // an instruction that ends the block on its own.
    unsigned blockEndsIn = 0;
    for (;;) {
      std::uint32_t instruction = 0;
      if (!fetch(pc, instruction)) {
        ++counters.faults;
        return leave(SegmentExit::Fault, pc, "instruction fetch outside main RAM and the scratchpad");
      }
      ++executed;
      const bool delaySlot = std::exchange(inDelaySlot, false);
      arriving = std::exchange(pending, DelayedLoad{});
      std::uint32_t after = next + 4u;
      const std::uint32_t op = primaryOpcode(instruction);
      const std::uint32_t rs = rsField(instruction);
      const std::uint32_t rt = rtField(instruction);
      std::uint32_t *const r = core.r;

      // A branch: `taken` jumps to `target` after the delay slot; the block ends there unless it stays inside.
      auto branch = [&](bool taken, std::uint32_t target, bool unconditional) {
        inDelaySlot = true;
        if (taken) {
          after = target;
        }
        if (unconditional) {
          blockEndsIn = 2;
        } else if (taken &&
                   (executed - segmentStart >= budget || stopRequested.load(std::memory_order_relaxed) ||
                    !staysInBlock(blockStart, pc, target))) {
          blockEndsIn = 2;
        }
      };
      const std::uint32_t branchTarget = next + (signedImmediate(instruction) << 2);

      switch (op) {
      case kSpecial: {
        const std::uint32_t rd = rdField(instruction);
        const std::uint32_t shift = (instruction >> 6) & 0x1Fu;
        switch (functField(instruction)) {
        case kSll:
          setGpr(rd, r[rt] << shift);
          break;
        case kSrl:
          setGpr(rd, r[rt] >> shift);
          break;
        case kSra:
          setGpr(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> shift));
          break;
        case kSllv:
          setGpr(rd, r[rt] << (r[rs] & 0x1Fu));
          break;
        case kSrlv:
          setGpr(rd, r[rt] >> (r[rs] & 0x1Fu));
          break;
        case kSrav:
          setGpr(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 0x1Fu)));
          break;
        case kJr:
          branch(true, r[rs], true);
          break;
        case kJalr: {
          const std::uint32_t target = r[rs];
          setGpr(rd, next + 4u);
          branch(true, target, true);
          break;
        }
        case kSyscall:
          if (delaySlot) {
            ++counters.faults;
            return leave(SegmentExit::Fault, pc, "syscall in a delay slot is unsupported");
          }
          return leave(SegmentExit::Syscall, pc);
        case kBreak:
          if (delaySlot) {
            ++counters.faults;
            return leave(SegmentExit::Fault, pc, "break in a delay slot is unsupported");
          }
          return leave(SegmentExit::Break, pc);
        case kMfhi:
          setGpr(rd, core.hi);
          break;
        case kMthi:
          core.hi = r[rs];
          break;
        case kMflo:
          setGpr(rd, core.lo);
          break;
        case kMtlo:
          core.lo = r[rs];
          break;
        case kMult: {
          const std::int64_t product =
              static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) * static_cast<std::int32_t>(r[rt]);
          core.lo = static_cast<std::uint32_t>(product);
          core.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32);
          break;
        }
        case kMultu: {
          const std::uint64_t product = static_cast<std::uint64_t>(r[rs]) * r[rt];
          core.lo = static_cast<std::uint32_t>(product);
          core.hi = static_cast<std::uint32_t>(product >> 32);
          break;
        }
        case kDiv: {
          const Division operands{r[rs], r[rt]};
          core.lo = quotient(operands);
          core.hi = remainder(operands);
          break;
        }
        case kDivu: {
          const std::uint32_t n = r[rs];
          const std::uint32_t d = r[rt];
          core.lo = d == 0 ? 0xFFFFFFFFu : n / d;
          core.hi = d == 0 ? n : n % d;
          break;
        }
        // No overflow trap, as the recompiler before this interpreter did not raise one either.
        case kAdd:
        case kAddu:
          setGpr(rd, r[rs] + r[rt]);
          break;
        case kSub:
        case kSubu:
          setGpr(rd, r[rs] - r[rt]);
          break;
        case kAnd:
          setGpr(rd, r[rs] & r[rt]);
          break;
        case kOr:
          setGpr(rd, r[rs] | r[rt]);
          break;
        case kXor:
          setGpr(rd, r[rs] ^ r[rt]);
          break;
        case kNor:
          setGpr(rd, ~(r[rs] | r[rt]));
          break;
        case kSlt:
          setGpr(rd, static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]) ? 1u : 0u);
          break;
        case kSltu:
          setGpr(rd, r[rs] < r[rt] ? 1u : 0u);
          break;
        default:
          ++counters.faults;
          return leave(SegmentExit::Fault, pc, "unknown SPECIAL instruction");
        }
        break;
      }
      case kRegimm: {
        const bool greaterOrEqual = (rt & 1u) != 0;
        const bool taken = (static_cast<std::int32_t>(r[rs]) >= 0) == greaterOrEqual;
        // BLTZAL/BGEZAL link whether or not they branch.
        if ((rt & 0x1Eu) == 0x10u) {
          setGpr(kRa, next + 4u);
        }
        branch(taken, branchTarget, isUnconditionalJump(instruction));
        break;
      }
      case kJ:
        branch(true, (next & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2), true);
        break;
      case kJal:
        setGpr(kRa, next + 4u);
        branch(true, (next & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2), true);
        break;
      case kBeq:
        branch(r[rs] == r[rt], branchTarget, rs == rt);
        break;
      case kBne:
        branch(r[rs] != r[rt], branchTarget, false);
        break;
      case kBlez:
        branch(static_cast<std::int32_t>(r[rs]) <= 0, branchTarget, rs == rt);
        break;
      case kBgtz:
        branch(static_cast<std::int32_t>(r[rs]) > 0, branchTarget, false);
        break;
      case kAddi:
      case kAddiu:
        setGpr(rt, r[rs] + signedImmediate(instruction));
        break;
      case kSlti:
        setGpr(rt,
               static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(signedImmediate(instruction)) ? 1u : 0u);
        break;
      case kSltiu:
        setGpr(rt, r[rs] < signedImmediate(instruction) ? 1u : 0u);
        break;
      case kAndi:
        setGpr(rt, r[rs] & immediate(instruction));
        break;
      case kOri:
        setGpr(rt, r[rs] | immediate(instruction));
        break;
      case kXori:
        setGpr(rt, r[rs] ^ immediate(instruction));
        break;
      case kLui:
        setGpr(rt, immediate(instruction) << 16);
        break;
      case kCop0:
        switch (rs) {
        case kMfc:
        case kCfc: {
          const std::uint32_t rd = rdField(instruction);
          delayLoad(rt, rd < 16 ? core.cop0[rd] : 0u);
          break;
        }
        case kMtc:
        case kCtc:
          writeCop0(rdField(instruction), r[rt]);
          if (endsBlock(instruction)) {
            blockEndsIn = 1;
          }
          break;
        default:
          if ((instruction & 0x0200003Fu) == 0x02000010u) { // RFE
            const std::uint32_t status = core.cop0[kCop0Status];
            writeCop0(kCop0Status, ((status & 0x3Cu) >> 2) | (status & ~0xFu));
            break;
          }
          ++counters.faults;
          return leave(SegmentExit::Fault, pc, "unknown COP0 instruction");
        }
        break;
      case kCop2:
        if ((instruction & (1u << 25)) != 0) {
          core.gte().execute(instruction);
          break;
        }
        switch (rs) {
        case kMfc: {
          const std::uint32_t value = readGteData(rdField(instruction));
          delayLoad(rt, value);
          cop2Transfer(instruction, value);
          break;
        }
        case kCfc: {
          const std::uint32_t value = core.gte().readControl(rdField(instruction));
          delayLoad(rt, value);
          cop2Transfer(instruction, value);
          break;
        }
        case kMtc:
          core.gte().writeData(rdField(instruction), r[rt]);
          break;
        case kCtc:
          core.gte().writeControl(rdField(instruction), r[rt]);
          cop2Transfer(instruction, r[rt]);
          break;
        default:
          ++counters.faults;
          return leave(SegmentExit::Fault, pc, "unknown COP2 instruction");
        }
        break;
      case kLb: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        const std::uint32_t value = load(instruction, address);
        delayLoad(rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value))));
        break;
      }
      case kLh: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        const std::uint32_t value = load(instruction, address);
        delayLoad(rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value))));
        break;
      }
      case kLw:
      case kLbu:
      case kLhu: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        delayLoad(rt, load(instruction, address));
        break;
      }
      case kLwl:
      case kLwr: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        const std::uint32_t word = load(instruction, address & ~3u);
        const std::uint32_t shift = (address & 3u) * 8u;
        const std::uint32_t base = mergeBase(rt);
        const std::uint32_t merged = op == kLwl ? (base & (0x00FFFFFFu >> shift)) | (word << (24u - shift))
                                                : (base & (0xFFFFFF00u << (24u - shift))) | (word >> shift);
        delayLoad(rt, merged);
        break;
      }
      case kSb:
      case kSh:
      case kSw:
      case kSwl:
      case kSwr:
      case kSwc2: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        StoreObserverTargetCounts *const observed = storeReport.armed ? observedStore(pc) : nullptr;
        const auto segmentInstructions = static_cast<std::uint32_t>(executed - segmentStart);
        if (observed != nullptr) {
          observeStore(*observed, pc, instruction, StoreObservationPhase::Before, segmentInstructions);
        }
        switch (op) {
        case kSb:
          store(instruction, address, r[rt], 1);
          break;
        case kSh:
          store(instruction, address, r[rt], 2);
          break;
        case kSw:
          store(instruction, address, r[rt], 4);
          break;
        case kSwc2:
          store(instruction, address, readGteData(rt), 4);
          break;
        default: {
          const std::uint32_t aligned = address & ~3u;
          beforeAccess(aligned);
          const std::uint32_t word = core.mem_r32(aligned);
          const std::uint32_t shift = (address & 3u) * 8u;
          const std::uint32_t merged = op == kSwl ? (word & (0xFFFFFF00u << shift)) | (r[rt] >> (24u - shift))
                                                  : (word & (0x00FFFFFFu >> (24u - shift))) | (r[rt] << shift);
          store(instruction, aligned, merged, 4);
          break;
        }
        }
        if (observed != nullptr) {
          observeStore(*observed, pc, instruction, StoreObservationPhase::After, segmentInstructions);
        }
        break;
      }
      case kLwc2: {
        const std::uint32_t address = r[rs] + signedImmediate(instruction);
        core.gte().writeData(rt, load(instruction, address));
        break;
      }
      default:
        ++counters.faults;
        return leave(SegmentExit::Fault, pc, "unknown instruction");
      }

      // The load from the previous instruction arrives now, unless this instruction wrote its register.
      if (arriving.reg != 0) {
        r[arriving.reg] = arriving.value;
        arriving.reg = 0;
      }
      pc = next;
      next = after;
      if (blockEndsIn == 0 || --blockEndsIn != 0) {
        continue;
      }
      // A block entry at `pc`.
      blockStart = pc;
      if (checkInterrupt || stopRequested.load(std::memory_order_relaxed)) {
        return leave(SegmentExit::CheckInterrupt, pc);
      }
      if (executed - segmentStart >= budget) {
        return leave(SegmentExit::Budget, pc);
      }
      // A stop lands every load still in flight (leave), so host code at a dispatch reads its arguments from Core.
      if (enterBlock(pc)) {
        return leave(SegmentExit::Boundary, pc);
      }
      if (next != pc + 4u) {
        // The entry is a jump target reached in a delay slot (a branch in a branch's delay slot).
        ++counters.faults;
        return leave(SegmentExit::Fault, pc, "a branch in a delay slot is unsupported");
      }
    }
  }

  [[nodiscard]] bool pendingWorkDeliverable() const {
    return core.game && core.active_native_address == 0 && core.pending_guest_redirect == 0 &&
           __atomic_load_n(&core.pending_work, __ATOMIC_RELAXED) != 0;
  }

  // Pending work served where the guest stopped; a result when the caller, not this executor, resumes the guest.
  std::optional<ExecutionResult> servicePendingWorkAt(BoundaryContext &boundary, std::uint64_t consumedCycles) {
    servicePendingWork(core);
    if (auto requested = core.executionControl().consume(core.pc, consumedCycles)) {
      return requested;
    }
    if (!boundary.dispatchHostServices) {
      return ExecutionResult{ExecutionExitReason::HostService, core.pc, consumedCycles, "pending work"};
    }
    boundary.skipPendingBoundaryOnce = __atomic_load_n(&core.pending_work, __ATOMIC_RELAXED) != 0;
    return std::nullopt;
  }

  // Classifies the resume pc a budget exit hands back; an unresolvable one is reported at once.
  ExecutionExitReason recordBudgetExit(std::uint32_t resumePc) {
    ++counters.budgetExits;
    if (core.imageCatalog().resolve(resumePc).has_value()) {
      ++counters.budgetExitPcInCodeImage;
    } else {
      ++counters.budgetExitPcOutsideCodeImage;
      lucent::error("executor",
                    "budget exit reported resume pc 0x{:08X}, which is NOT in any loaded code "
                    "image; guest execution will resume there. {} of {} budget exit(s) so far "
                    "reported a pc outside every code image",
                    resumePc,
                    counters.budgetExitPcOutsideCodeImage,
                    counters.budgetExits);
    }
    return ExecutionExitReason::BudgetExhausted;
  }

  Core &core;
  // The block last entered, for the watchdog's report.
  volatile std::uint32_t enteredPc = 0;
  BoundaryContext *running = nullptr;
  ExecutorCounters counters;
  SegmentClockLedger segmentClock;
  // Guest instructions executed by this executor, the clock the segment ledger commits from.
  std::uint64_t executed = 0;
  DelayedLoad pending;
  DelayedLoad arriving;
  std::atomic<bool> stopRequested{false};
  bool checkInterrupt = false;
  StoreObserverCallback storeCallback = nullptr;
  void *storeContext = nullptr;
  StoreObserverReport storeReport{};
  std::unique_ptr<FunctionReach> reach;
};

GuestExecutor::GuestExecutor(Core &core) : impl_(std::make_unique<Impl>(core)) {}

GuestExecutor::~GuestExecutor() {
  logLedger(impl_->counters, "shutdown", lucent::Level::Info);
  psx::cpu::reportStoreObserver(impl_->storeReport);
}

void GuestExecutor::attachFunctionReach(std::unique_ptr<FunctionReach> reach) {
  impl_->reach = std::move(reach);
}

namespace {

void accountExecutedInstructions(Core &core, std::uint64_t instructions) {
  while (instructions != 0) {
    const auto chunk =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(instructions, std::numeric_limits<std::uint32_t>::max()));
    accountGuestInstructions(core, chunk);
    instructions -= chunk;
  }
}

} // namespace

ExecutionResult GuestExecutor::executeWithBoundary(std::uint32_t guestAddress,
                                                   std::optional<std::uint32_t> returnAddress,
                                                   bool dispatchHostServices,
                                                   ExecutionBudget budget) {
  Impl &impl = *impl_;
  ++impl.counters.calls;
  Impl::BoundaryContext boundary{.returnAddress = returnAddress, .dispatchHostServices = dispatchHostServices};
  std::uint64_t consumedCycles = 0;
  std::uint64_t hostDispatches = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    boundary.reason = Impl::BoundaryReason::None;
    boundary.pc = 0;
    impl.core.gte().bind();
    const std::uint64_t before = impl.executed;
    impl.segmentClock.beginSegment(before);
    // A segment also ends at the host field deadline and the next device deadline, which are only
    // folded into the clock between segments.
    // At most 32 bits of instructions, so every commit of the segment clock fits its delta.
    std::uint64_t segmentCycleBudget =
        std::min<std::uint64_t>(budget.cycles - consumedCycles, std::numeric_limits<std::uint32_t>::max());
    if (impl.core.game) {
      const std::uint64_t untilFieldDue = impl.core.hostTurn.ticksUntilDue(impl.core);
      if (untilFieldDue != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, untilFieldDue);
      }
      const std::optional<std::uint64_t> untilDeviceEvent = impl.core.game->timing.ticksUntilDeviceEvent();
      if (untilDeviceEvent && *untilDeviceEvent != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, *untilDeviceEvent);
      }
    }
    // The start names the block behind a wild target.
    const std::uint32_t blockStart = nextPc;
    Impl::BoundaryContext *const enclosing = std::exchange(impl.running, &boundary);
    Impl::Segment segment;
    {
      // Guest frames are on the stack only in here; the journal must not raise through them.
      const SideEffectJournal::GuestExecutionScope guest(impl.core);
      segment = impl.runSegment(nextPc, segmentCycleBudget);
    }
    impl.running = enclosing;
    nextPc = segment.pc;
    impl.core.pc = nextPc;
    impl.core.r[0] = 0;
    if (impl.core.sideEffectJournal != nullptr) {
      impl.core.sideEffectJournal->stopDeferredDiscardedRun();
    }
    const std::uint64_t segmentInstructions = impl.executed - before;
    consumedCycles += segmentInstructions;
    impl.counters.executedInstructions = impl.executed;
    if (impl.storeReport.armed) {
      impl.storeReport.executedInstructions += segmentInstructions;
    }
    if (impl.core.game) {
      // The exact instruction count, less what device-access commits already charged.
      accountExecutedInstructions(impl.core, impl.segmentClock.uncommitted(segmentInstructions));
      impl.core.game->display.showIfDue();
    }

    if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
      return *requested;
    }
    switch (segment.exit) {
    case Impl::SegmentExit::Fault:
      lucent::error("executor", "guest fault at 0x{:08X}: {}", nextPc, segment.fault);
      return {ExecutionExitReason::Fault, nextPc, consumedCycles, segment.fault};
    case Impl::SegmentExit::Syscall: {
      const auto syscall = handleSyscall(impl.core, nextPc);
      if (syscall != SyscallResult::Handled) {
        ++impl.counters.faults;
        return {ExecutionExitReason::Fault,
                nextPc,
                consumedCycles,
                syscall == SyscallResult::UnsupportedSelector ? "unsupported syscall selector"
                                                              : "syscall without game context"};
      }
      impl.core.pc = nextPc + 4u;
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::InterruptOrException, impl.core.pc, consumedCycles, "syscall"};
    }
    case Impl::SegmentExit::Break: {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      handleBreak(impl.core, (instruction >> 6u) & 0xfffffu);
      impl.core.pc = nextPc + 4u;
      return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "break"};
    }
    case Impl::SegmentExit::Boundary:
      switch (boundary.reason) {
      case Impl::BoundaryReason::GuestReturn:
        return {ExecutionExitReason::GuestReturn, boundary.pc, consumedCycles};
      case Impl::BoundaryReason::HostDispatch: {
        if (hostDispatches >= budget.maxHostDispatches) {
          return {impl.recordBudgetExit(boundary.pc), boundary.pc, consumedCycles, "host dispatch budget exhausted"};
        }
        ++hostDispatches;
        ++impl.counters.hostDispatches;
        ExecutionResult result = dispatchGuestHostService(impl.core, boundary.pc);
        result.cycles += consumedCycles;
        if (!result.returned()) {
          if (result.reason == ExecutionExitReason::Fault) {
            lucent::error("executor",
                          "host dispatch to 0x{:08X} FAILED ({}); the segment that produced this target began at "
                          "0x{:08X}",
                          boundary.pc,
                          result.detail,
                          blockStart);
          }
          return result;
        }
        // Nested native calls restore their C++ caller's scoped PC; the result owns the guest continuation.
        nextPc = result.guestPc;
        continue;
      }
      case Impl::BoundaryReason::PendingWork:
        if (auto handedBack = impl.servicePendingWorkAt(boundary, consumedCycles)) {
          return *handedBack;
        }
        nextPc = impl.core.pc;
        continue;
      case Impl::BoundaryReason::None:
        ++impl.counters.faults;
        return {ExecutionExitReason::Fault, nextPc, consumedCycles, "unclassified block boundary stop"};
      }
      break;
    case Impl::SegmentExit::CheckInterrupt:
      if (impl.core.game) {
        servicePendingWork(impl.core);
      }
      if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
        return *requested;
      }
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::HostService, nextPc, consumedCycles, "pending work"};
    case Impl::SegmentExit::Budget:
      break;
    }
    if (consumedCycles < budget.cycles) {
      // Ended at the field or device deadline, not the caller's budget: what the deadline raised is taken before the
      // guest resumes, since a loop inside one block reaches no block entry until the next deadline.
      if (impl.pendingWorkDeliverable()) {
        if (auto handedBack = impl.servicePendingWorkAt(boundary, consumedCycles)) {
          return *handedBack;
        }
        nextPc = impl.core.pc;
      }
      continue;
    }
    return {impl.recordBudgetExit(nextPc), nextPc, consumedCycles, "cycle budget exhausted"};
  }
  return {ExecutionExitReason::BudgetExhausted, nextPc, consumedCycles, "cycle budget exhausted"};
}

ExecutionResult GuestExecutor::executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, std::nullopt, true, budget);
}

ExecutionResult
GuestExecutor::executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, returnAddress, true, budget);
}

void GuestExecutor::requestStop() {
  impl_->stopRequested.store(true, std::memory_order_relaxed);
}

void GuestExecutor::invalidate(GuestAddressRange, ExecutableWriteSource source) {
  ++impl_->counters.invalidations;
  ++impl_->counters.invalidationsBySource[static_cast<std::size_t>(source)];
}

void GuestExecutor::invalidateAll(ExecutableWriteSource source) {
  ++impl_->counters.invalidations;
  ++impl_->counters.invalidationsBySource[static_cast<std::size_t>(source)];
}

StoreObserverStatus GuestExecutor::configureStoreObserver(std::span<const std::uint32_t> targets,
                                                          StoreObserverCallback callback,
                                                          void *context) {
  Impl &impl = *impl_;
  if ((targets.empty() && (callback || context)) || targets.size() > kMaxObservedStoreTargets ||
      (!targets.empty() && callback == nullptr)) {
    return StoreObserverStatus::InvalidConfiguration;
  }
  if (targets.empty()) {
    impl.storeReport.armed = false;
    impl.storeCallback = nullptr;
    impl.storeContext = nullptr;
    return StoreObserverStatus::Configured;
  }
  impl.storeReport = {};
  impl.storeReport.targetCount = targets.size();
  impl.storeReport.armed = true;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    impl.storeReport.targets[i].guestPc = targets[i];
  }
  impl.storeCallback = callback;
  impl.storeContext = context;
  return StoreObserverStatus::Configured;
}

StoreObserverReport GuestExecutor::storeObserverReport() const {
  return impl_->storeReport;
}

const ExecutorCounters &GuestExecutor::counters() const {
  return impl_->counters;
}

} // namespace psx::cpu
