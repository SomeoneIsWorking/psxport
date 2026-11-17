// block_scan.h — reads one guest block from memory for translation: its extent, its in-block branch targets, and
// why a block is left to the interpreter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psx::cpu {
class ExecutorState;
}

namespace psx::cpu::recompiler {

// Why a block runs on the interpreter instead of translated code. Counted per reason and logged at run end.
enum class Refusal : std::uint8_t {
  None,
  NotMainRam,        // entry outside main RAM (the scratchpad, a misaligned pc)
  CrossesRamEnd,     // runs past the end of a 2 MiB RAM image
  TooLong,           // longer than the translation cap
  BranchInDelaySlot, // the interpreter faults there; it names the fault
  ExceptionInDelaySlot,
  TargetIsDelaySlot,  // an in-block branch lands on a delay slot
  TargetBeyondBlock,  // an in-block branch lands past a break or unknown instruction that ends the translation
  LoadDelayAtTarget,  // a load in flight meets an in-block branch target that reads or writes its register
  PendingLoadAtEntry, // run time: a load in flight at entry meets the first instruction's registers
  StoreObserverArmed, // run time: the diagnostic store observer reads registers per store
  CodeMemoryBusy,     // run time: the code mapping is full while translated code is running
  Count,
};

const char *refusalName(Refusal refusal);

struct ScannedInstruction {
  std::uint32_t pc = 0;
  std::uint32_t word = 0;
  bool delaySlot = false;
  // Reached by a taken branch inside the block (not only by falling through).
  bool target = false;
};

struct ScannedBlock {
  std::uint32_t pc = 0;
  // Main-RAM offsets [ramBegin, ramEnd) of the words read.
  std::uint32_t ramBegin = 0;
  std::uint32_t ramEnd = 0;
  std::vector<ScannedInstruction> code;
  Refusal refusal = Refusal::None;

  // The index of the in-block instruction at `pc`, or -1.
  [[nodiscard]] long indexOf(std::uint32_t guestPc) const;
};

// The block entered at `pc`, read the way the interpreter runs it: to the delay slot of the first unconditional
// jump, or to the first SYSCALL, Status/Cause write, BREAK or unknown instruction, at most `maxInstructions` long.
ScannedBlock scanBlock(const ExecutorState &state, std::uint32_t pc, std::size_t maxInstructions);

// The registers whose load in flight at an instruction makes its delay-slot behavior differ from landing the load
// first: the GPRs it reads, writes, or loads.
std::uint32_t loadDelayConflicts(std::uint32_t instruction);

} // namespace psx::cpu::recompiler
