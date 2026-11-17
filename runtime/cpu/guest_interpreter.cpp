#include "guest_interpreter.h"

#include "core.h"
#include "cpu_divide.h"
#include "r3000_decode.h"

#include <utility>

namespace psx::cpu {
namespace {

// Whether a taken branch at `branchPc` to `target` stays inside the block that began at `blockStart`.
// The block runs to the delay slot of its first unconditional jump, or to its first block-ending
// instruction; everything from its start to the branch's delay slot has already run inside it.
bool staysInBlock(const ExecutorState &state, std::uint32_t blockStart, std::uint32_t branchPc, std::uint32_t target) {
  if (target < blockStart) {
    return false;
  }
  if (target <= branchPc + 4u) {
    return true;
  }
  for (std::uint32_t at = branchPc + 8u; at < target; at += 4u) {
    std::uint32_t word = 0;
    if (!state.fetch(at, word)) {
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

} // namespace

BlockOutcome interpretBlock(ExecutorState &state, std::uint32_t pc, std::uint32_t blockStart, SegmentLimits limits) {
  Core &core = state.core;
  std::uint32_t next = pc + 4u;
  bool inDelaySlot = false;
  // Instructions left until the block ends: 2 after a block-leaving jump (it and its delay slot), 1 after
  // an instruction that ends the block on its own.
  unsigned blockEndsIn = 0;
  auto leave = [&](BlockEnd end, const char *fault = nullptr) {
    if (end == BlockEnd::Fault) {
      ++state.counters.faults;
    }
    return BlockOutcome{end, pc, next, fault};
  };
  for (;;) {
    std::uint32_t instruction = 0;
    if (!state.fetch(pc, instruction)) {
      return leave(BlockEnd::Fault, "instruction fetch outside main RAM and the scratchpad");
    }
    ++state.executed;
    const bool delaySlot = std::exchange(inDelaySlot, false);
    state.arriving = std::exchange(state.pending, DelayedLoad{});
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
      } else if (taken && (state.executed - limits.start >= limits.budget ||
                           state.stopRequested.load(std::memory_order_relaxed) ||
                           !staysInBlock(state, blockStart, pc, target))) {
        blockEndsIn = 2;
      }
    };
    const std::uint32_t branchTarget = next + (signedImmediate(instruction) << 2);

    switch (op) {
    case kSpecial: {
      const std::uint32_t rd = rdField(instruction);
      const std::uint32_t shift = shamtField(instruction);
      switch (functField(instruction)) {
      case kSll:
        state.setGpr(rd, r[rt] << shift);
        break;
      case kSrl:
        state.setGpr(rd, r[rt] >> shift);
        break;
      case kSra:
        state.setGpr(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> shift));
        break;
      case kSllv:
        state.setGpr(rd, r[rt] << (r[rs] & 0x1Fu));
        break;
      case kSrlv:
        state.setGpr(rd, r[rt] >> (r[rs] & 0x1Fu));
        break;
      case kSrav:
        state.setGpr(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 0x1Fu)));
        break;
      case kJr:
        branch(true, r[rs], true);
        break;
      case kJalr: {
        const std::uint32_t target = r[rs];
        state.setGpr(rd, next + 4u);
        branch(true, target, true);
        break;
      }
      case kSyscall:
        if (delaySlot) {
          return leave(BlockEnd::Fault, "syscall in a delay slot is unsupported");
        }
        return leave(BlockEnd::Syscall);
      case kBreak:
        if (delaySlot) {
          return leave(BlockEnd::Fault, "break in a delay slot is unsupported");
        }
        return leave(BlockEnd::Break);
      case kMfhi:
        state.setGpr(rd, core.hi);
        break;
      case kMthi:
        core.hi = r[rs];
        break;
      case kMflo:
        state.setGpr(rd, core.lo);
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
        state.setGpr(rd, r[rs] + r[rt]);
        break;
      case kSub:
      case kSubu:
        state.setGpr(rd, r[rs] - r[rt]);
        break;
      case kAnd:
        state.setGpr(rd, r[rs] & r[rt]);
        break;
      case kOr:
        state.setGpr(rd, r[rs] | r[rt]);
        break;
      case kXor:
        state.setGpr(rd, r[rs] ^ r[rt]);
        break;
      case kNor:
        state.setGpr(rd, ~(r[rs] | r[rt]));
        break;
      case kSlt:
        state.setGpr(rd, static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]) ? 1u : 0u);
        break;
      case kSltu:
        state.setGpr(rd, r[rs] < r[rt] ? 1u : 0u);
        break;
      default:
        return leave(BlockEnd::Fault, "unknown SPECIAL instruction");
      }
      break;
    }
    case kRegimm: {
      const bool greaterOrEqual = (rt & 1u) != 0;
      const bool taken = (static_cast<std::int32_t>(r[rs]) >= 0) == greaterOrEqual;
      // BLTZAL/BGEZAL link whether or not they branch.
      if ((rt & 0x1Eu) == 0x10u) {
        state.setGpr(kRa, next + 4u);
      }
      branch(taken, branchTarget, isUnconditionalJump(instruction));
      break;
    }
    case kJ:
      branch(true, (next & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2), true);
      break;
    case kJal:
      state.setGpr(kRa, next + 4u);
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
      state.setGpr(rt, r[rs] + signedImmediate(instruction));
      break;
    case kSlti:
      state.setGpr(
          rt, static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(signedImmediate(instruction)) ? 1u : 0u);
      break;
    case kSltiu:
      state.setGpr(rt, r[rs] < signedImmediate(instruction) ? 1u : 0u);
      break;
    case kAndi:
      state.setGpr(rt, r[rs] & immediate(instruction));
      break;
    case kOri:
      state.setGpr(rt, r[rs] | immediate(instruction));
      break;
    case kXori:
      state.setGpr(rt, r[rs] ^ immediate(instruction));
      break;
    case kLui:
      state.setGpr(rt, immediate(instruction) << 16);
      break;
    case kCop0:
      switch (rs) {
      case kMfc:
      case kCfc: {
        const std::uint32_t rd = rdField(instruction);
        state.delayLoad(rt, rd < 16 ? core.cop0[rd] : 0u);
        break;
      }
      case kMtc:
      case kCtc:
        state.writeCop0(rdField(instruction), r[rt]);
        if (endsBlock(instruction)) {
          blockEndsIn = 1;
        }
        break;
      default:
        if (isRfe(instruction)) {
          state.returnFromException();
          break;
        }
        return leave(BlockEnd::Fault, "unknown COP0 instruction");
      }
      break;
    case kCop2:
      if ((instruction & (1u << 25)) != 0) {
        core.gte().execute(instruction);
        break;
      }
      switch (rs) {
      case kMfc: {
        const std::uint32_t value = state.readGteData(rdField(instruction));
        state.delayLoad(rt, value);
        state.cop2Transfer(instruction, value);
        break;
      }
      case kCfc: {
        const std::uint32_t value = core.gte().readControl(rdField(instruction));
        state.delayLoad(rt, value);
        state.cop2Transfer(instruction, value);
        break;
      }
      case kMtc:
        core.gte().writeData(rdField(instruction), r[rt]);
        break;
      case kCtc:
        core.gte().writeControl(rdField(instruction), r[rt]);
        state.cop2Transfer(instruction, r[rt]);
        break;
      default:
        return leave(BlockEnd::Fault, "unknown COP2 instruction");
      }
      break;
    case kLb:
    case kLh:
    case kLw:
    case kLbu:
    case kLhu:
      state.delayLoad(rt, state.loadValue(instruction, r[rs] + signedImmediate(instruction)));
      break;
    case kLwl:
    case kLwr:
      state.delayLoad(rt, state.loadUnaligned(instruction, r[rs] + signedImmediate(instruction), state.mergeBase(rt)));
      break;
    case kSb:
    case kSh:
    case kSw:
    case kSwl:
    case kSwr:
    case kSwc2:
      state.storeInstruction(instruction,
                             pc,
                             r[rs] + signedImmediate(instruction),
                             static_cast<std::uint32_t>(state.executed - limits.start));
      break;
    case kLwc2: {
      const std::uint32_t address = r[rs] + signedImmediate(instruction);
      core.gte().writeData(rt, state.load(instruction, address));
      break;
    }
    default:
      return leave(BlockEnd::Fault, "unknown instruction");
    }

    // The load from the previous instruction arrives now, unless this instruction wrote its register.
    if (state.arriving.reg != 0) {
      r[state.arriving.reg] = state.arriving.value;
      state.arriving.reg = 0;
    }
    pc = next;
    next = after;
    if (blockEndsIn == 0 || --blockEndsIn != 0) {
      continue;
    }
    return BlockOutcome{BlockEnd::Entry, pc, next, nullptr};
  }
}

} // namespace psx::cpu
