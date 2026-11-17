// recompiler.h — psxport's run-time recompiler: guest blocks translated to host code as they are first entered,
// run in place of the interpreter, and dropped when their bytes change.
#pragma once

#include "block_cache.h"
#include "block_scan.h"
#include "code_memory.h"
#include "guest_address_range.h"
#include "guest_interpreter.h"
#include "jit_context.h"

#include <array>
#include <cstdint>
#include <memory>

namespace psx::cpu::recompiler {

struct RecompilerCounters {
  std::uint64_t translated = 0;
  std::uint64_t translatedInstructions = 0;
  std::uint64_t translatedBytes = 0;
  std::uint64_t translatedRuns = 0;
  // Block runs the interpreter took instead, by reason.
  std::array<std::uint64_t, static_cast<std::size_t>(Refusal::Count)> interpreted{};
  std::uint64_t resumes = 0;
  std::uint64_t flushes = 0;
  std::uint64_t dropped = 0;
};

// One executor's recompiler. It translates from guest memory at run time, never saves a translation, and keeps the
// executor contract: a translated block runs on Core's registers, sends every store and every non-RAM load through
// the same ExecutorState paths as the interpreter, keeps branch and load delay slots, and returns at the block end
// the interpreter would reach, so block entries (native dispatch, guest return, pending work) are unchanged.
class Recompiler {
public:
  // Null on a host with no backend or no executable memory: the executor interprets there.
  static std::unique_ptr<Recompiler> create(ExecutorState &state);
  ~Recompiler();
  Recompiler(const Recompiler &) = delete;
  Recompiler &operator=(const Recompiler &) = delete;

  // Runs the block entered at `pc` to its end, translated when it can be, on the interpreter otherwise.
  BlockOutcome runBlock(std::uint32_t pc, SegmentLimits limits);
  // A physical guest range whose bytes changed (runtime/cpu/invalidation.cpp): every block read from it is dropped.
  void invalidate(GuestAddressRange physicalRange);
  void invalidateAll();

  [[nodiscard]] const RecompilerCounters &counters() const {
    return counters_;
  }
  void logReport() const;

private:
  Recompiler(ExecutorState &state, std::unique_ptr<CodeMemory> memory);
  CompiledBlock *compile(std::uint32_t pc);
  BlockOutcome interpret(std::uint32_t pc, SegmentLimits limits, Refusal reason);

  // Helpers called from translated code.
  static std::uint32_t helperLoad(JitContext *context, std::uint32_t instruction, std::uint32_t address);
  static std::uint32_t
  helperLoadUnaligned(JitContext *context, std::uint32_t instruction, std::uint32_t address, std::uint32_t base);
  static std::uint32_t
  helperStore(JitContext *context, std::uint32_t instruction, std::uint32_t pc, std::uint32_t address);
  static void helperLwc2(JitContext *context, std::uint32_t instruction, std::uint32_t address);
  static void helperGteCommand(JitContext *context, std::uint32_t instruction);
  static std::uint32_t helperMfc2(JitContext *context, std::uint32_t instruction);
  static std::uint32_t helperCfc2(JitContext *context, std::uint32_t instruction);
  static void helperMtc2(JitContext *context, std::uint32_t instruction, std::uint32_t value);
  static void helperCtc2(JitContext *context, std::uint32_t instruction, std::uint32_t value);
  static void helperMtc0(JitContext *context, std::uint32_t reg, std::uint32_t value);
  static void helperRfe(JitContext *context);
  static void helperDiv(JitContext *context, std::uint32_t numerator, std::uint32_t denominator);
  static void helperDivu(JitContext *context, std::uint32_t numerator, std::uint32_t denominator);

  ExecutorState &state_;
  std::unique_ptr<CodeMemory> memory_;
  BlockCache cache_;
  JitContext context_;
  RecompilerCounters counters_;
  // The translated block running now, whose own invalidation sends the rest of it to the interpreter.
  const CompiledBlock *running_ = nullptr;
  std::uint64_t segmentStart_ = 0;
};

} // namespace psx::cpu::recompiler
