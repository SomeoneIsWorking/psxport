#include "block_scan.h"

#include "executor_state.h"
#include "guest_memory_map.h"
#include "r3000_decode.h"

namespace psx::cpu::recompiler {
namespace {

std::uint32_t registerBit(std::uint32_t reg) {
  return reg == 0 ? 0u : 1u << reg;
}

// The static target of a conditional branch at `pc`.
std::uint32_t branchTarget(std::uint32_t pc, std::uint32_t word) {
  return pc + 4u + (signedImmediate(word) << 2);
}

bool isConditionalBranch(std::uint32_t word) {
  const std::uint32_t op = primaryOpcode(word);
  return isBranchOrJump(word) && op != kSpecial && op != kJ && op != kJal;
}

// The interpreter's rule for a taken branch staying inside the block (guest_interpreter.cpp staysInBlock).
bool interpreterStays(const ExecutorState &state,
                      std::uint32_t blockStart,
                      std::uint32_t branchPc,
                      std::uint32_t target) {
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

const char *refusalName(Refusal refusal) {
  switch (refusal) {
  case Refusal::None:
    return "none";
  case Refusal::NotMainRam:
    return "not_main_ram";
  case Refusal::CrossesRamEnd:
    return "crosses_ram_end";
  case Refusal::TooLong:
    return "too_long";
  case Refusal::BranchInDelaySlot:
    return "branch_in_delay_slot";
  case Refusal::ExceptionInDelaySlot:
    return "exception_in_delay_slot";
  case Refusal::TargetIsDelaySlot:
    return "target_is_delay_slot";
  case Refusal::TargetBeyondBlock:
    return "target_beyond_block";
  case Refusal::LoadDelayAtTarget:
    return "load_delay_at_target";
  case Refusal::PendingLoadAtEntry:
    return "pending_load_at_entry";
  case Refusal::StoreObserverArmed:
    return "store_observer_armed";
  case Refusal::CodeMemoryBusy:
    return "code_memory_busy";
  case Refusal::Count:
    break;
  }
  return "?";
}

long ScannedBlock::indexOf(std::uint32_t guestPc) const {
  if (guestPc < pc || ((guestPc - pc) & 3u) != 0) {
    return -1;
  }
  const std::uint32_t index = (guestPc - pc) >> 2;
  return index < code.size() ? static_cast<long>(index) : -1;
}

std::uint32_t loadDelayConflicts(std::uint32_t instruction) {
  return gprReads(instruction) | registerBit(immediateGprWrite(instruction)) |
         registerBit(delayedLoadTarget(instruction));
}

ScannedBlock scanBlock(const ExecutorState &state, std::uint32_t pc, std::size_t maxInstructions) {
  ScannedBlock block;
  block.pc = pc;
  const std::uint32_t phys = physical(pc);
  if ((pc & 3u) != 0 || phys >= kRamMirrorSpan) {
    block.refusal = Refusal::NotMainRam;
    return block;
  }
  block.ramBegin = phys & (kMainRamBytes - 1u);
  bool inDelaySlot = false;
  bool endAfterNext = false;
  for (std::uint32_t at = pc;; at += 4u) {
    if (block.code.size() >= maxInstructions) {
      block.refusal = Refusal::TooLong;
      return block;
    }
    const std::uint32_t offset = block.ramBegin + static_cast<std::uint32_t>(block.code.size() * 4u);
    if (offset >= kMainRamBytes || physical(at) >= kRamMirrorSpan) {
      block.refusal = Refusal::CrossesRamEnd;
      return block;
    }
    std::uint32_t word = 0;
    state.fetch(at, word);
    block.code.push_back({at, word, inDelaySlot, false});
    if (inDelaySlot) {
      if (isBranchOrJump(word)) {
        block.refusal = Refusal::BranchInDelaySlot;
        return block;
      }
      if (primaryOpcode(word) == kSpecial && (functField(word) == kSyscall || functField(word) == kBreak)) {
        block.refusal = Refusal::ExceptionInDelaySlot;
        return block;
      }
    }
    if (endAfterNext) {
      break;
    }
    if (isUnconditionalJump(word)) {
      endAfterNext = true;
    } else if (endsBlock(word) || unknownInstructionFault(word) != nullptr ||
               (primaryOpcode(word) == kSpecial && functField(word) == kBreak)) {
      break;
    }
    inDelaySlot = isBranchOrJump(word);
  }
  block.ramEnd = block.ramBegin + static_cast<std::uint32_t>(block.code.size() * 4u);

  // In-block branch targets, checked against the interpreter's own rule.
  const std::uint32_t lastPc = block.code.back().pc;
  for (std::size_t i = 0; i + 1 < block.code.size(); ++i) {
    const ScannedInstruction &branch = block.code[i];
    if (!isConditionalBranch(branch.word) || isUnconditionalJump(branch.word)) {
      continue;
    }
    const std::uint32_t target = branchTarget(branch.pc, branch.word);
    if (!interpreterStays(state, pc, branch.pc, target)) {
      continue;
    }
    if (target > lastPc) {
      block.refusal = Refusal::TargetBeyondBlock;
      return block;
    }
    ScannedInstruction &landing = block.code[static_cast<std::size_t>(block.indexOf(target))];
    if (landing.delaySlot) {
      block.refusal = Refusal::TargetIsDelaySlot;
      return block;
    }
    landing.target = true;
    // The load the branch's delay slot leaves in flight arrives at the target.
    const std::uint32_t inFlight = delayedLoadTarget(block.code[i + 1].word);
    if ((loadDelayConflicts(landing.word) & registerBit(inFlight)) != 0) {
      block.refusal = Refusal::LoadDelayAtTarget;
      return block;
    }
  }
  // A target reached by falling through from a load: the load must land before the target's label.
  for (std::size_t t = 1; t < block.code.size(); ++t) {
    const std::uint32_t inFlight = delayedLoadTarget(block.code[t - 1].word);
    if (block.code[t].target && (loadDelayConflicts(block.code[t].word) & registerBit(inFlight)) != 0) {
      block.refusal = Refusal::LoadDelayAtTarget;
      return block;
    }
  }
  return block;
}

} // namespace psx::cpu::recompiler
