// jit_context.h — what a translated block reads and writes besides guest registers: the run's pointers, the exit it
// reports, and the C++ helpers it calls for every access that has host-side bookkeeping.
#pragma once

#include <cstddef>
#include <cstdint>

namespace psx::cpu::recompiler {

// How a translated block ended. Entry, Syscall, Break and Fault are the interpreter's BlockEnd; Resume asks the
// interpreter to continue the same block at `exitPc` (a store rewrote the block's own code).
enum class JitExit : std::uint32_t {
  Entry = 0,
  Syscall = 1,
  Break = 2,
  Fault = 3,
  Resume = 4,
};

enum Helper : std::uint32_t {
  kHelperLoad,          // (ctx, instruction, address) -> register value of LB/LH/LW/LBU/LHU
  kHelperLoadUnaligned, // (ctx, instruction, address, base) -> LWL/LWR merge
  kHelperStore,         // (ctx, instruction, pc, address) -> 1 when the running block was invalidated
  kHelperLwc2,          // (ctx, instruction, address)
  kHelperGteCommand,    // (ctx, instruction)
  kHelperMfc2,          // (ctx, instruction) -> value
  kHelperCfc2,          // (ctx, instruction) -> value
  kHelperMtc2,          // (ctx, instruction, value)
  kHelperCtc2,          // (ctx, instruction, value)
  kHelperMtc0,          // (ctx, register, value)
  kHelperRfe,           // (ctx)
  kHelperDiv,           // (ctx, numerator, denominator)
  kHelperDivu,          // (ctx, numerator, denominator)
  kHelperCount,
};

// Standard layout: the backends address every field by offsetof.
struct JitContext {
  std::uint32_t *gpr = nullptr;            // Core::r; hi and lo follow r31 (R3000 layout)
  std::uint8_t *ram = nullptr;             // Core::ram, 2 MiB
  std::uint32_t *cop0 = nullptr;           // Core::cop0
  std::uint64_t *executed = nullptr;       // ExecutorState::executed
  std::uint64_t *memoryAccesses = nullptr; // ExecutorCounters::memoryAccesses
  const std::uint8_t *stop = nullptr;      // ExecutorState::stopRequested
  std::uint8_t *scratch = nullptr;         // Core::scratch, 1 KiB
  // VertexPlaces::LoadGate: a load whose place bookkeeping would do something takes the helper.
  const std::uint8_t *tracking = nullptr;
  const std::uint32_t *carriedMask = nullptr;
  const std::uint32_t *faceLoadMask = nullptr;
  std::uint64_t *const *placedWords = nullptr;
  std::uint64_t segmentEnd = 0; // executed count at which the segment's budget is spent
  std::uint32_t exitPc = 0;
  // A load still in flight at the exit (its register, 0 for none, and value): the next block's delay slot.
  std::uint32_t pendingReg = 0;
  std::uint32_t pendingValue = 0;
  // Set by the store helper when a store dropped the running block.
  std::uint32_t selfModified = 0;
  void *owner = nullptr; // the Recompiler
  void *helpers[kHelperCount] = {};
};

inline constexpr std::uint32_t kHiOffset = 32u * 4u;
inline constexpr std::uint32_t kLoOffset = 33u * 4u;

} // namespace psx::cpu::recompiler
