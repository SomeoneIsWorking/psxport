// guest_interpreter.h — the R3000 interpreter: one guest block at a time, the executor's reference engine.
#pragma once

#include "executor_state.h"

#include <cstdint>

namespace psx::cpu {

// How a block run ended: at the next block entry, or at an instruction that leaves the guest engine.
enum class BlockEnd : std::uint8_t {
  Entry,
  Syscall,
  Break,
  Fault,
};

struct BlockOutcome {
  BlockEnd end = BlockEnd::Entry;
  // The next block entry, or the instruction that left the engine.
  std::uint32_t pc = 0;
  // The instruction after `pc`: pc + 4 unless `pc` was reached in a delay slot (a branch in a branch's delay slot).
  std::uint32_t next = 0;
  const char *fault = nullptr;
  // The block entry at `pc` already ran and met a boundary (a recompiler linked entry): the segment stops there.
  bool entered = false;
};

// Runs from `pc` until the block that began at `blockStart` ends: after an unconditional jump's delay slot, after a
// Status/Cause write, or after the delay slot of a taken branch that leaves it (or once `limits` is spent or a stop is
// requested). `pc` is `blockStart` for a block entry, or an instruction inside the block to resume at with no load in
// flight. Loads still in flight at the end stay in `state` for the next block.
BlockOutcome interpretBlock(ExecutorState &state, std::uint32_t pc, std::uint32_t blockStart, SegmentLimits limits);

} // namespace psx::cpu
