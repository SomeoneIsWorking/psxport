// executor_trace.h — the differential trace of guest execution: the committed CPU state at block entries, written
// to a file so a recompiler run and an interpreter run of the same recipe can be compared block by block.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

class Core;

namespace psx::cpu {

struct ExecutorState;

// What one trace records: a checkpoint every `every` block entries, RAM hashed at every `ramEvery`-th checkpoint,
// and inside block entries [from, to) every entry is a checkpoint with RAM hashed.
struct ExecutorTraceConfig {
  std::string path;
  std::uint64_t every = 4096;
  std::uint64_t ramEvery = 64;
  std::uint64_t from = 0;
  std::uint64_t to = 0;
};

// One checkpoint, written as is (little-endian hosts). tools/exec_trace_compare.py reads the same layout.
struct ExecutorTraceRecord {
  std::uint64_t block = 0;      // block entries so far, this one included
  std::uint64_t executed = 0;   // guest instructions executed before this entry
  std::uint32_t pc = 0;         // the block entered
  std::uint32_t previousPc = 0; // the block entered before it, the one whose run a mismatch here points at
  std::uint32_t gpr[32] = {};
  std::uint32_t hi = 0;
  std::uint32_t lo = 0;
  std::uint32_t cop0[16] = {};
  std::uint32_t pendingReg = 0; // the load in flight across the entry
  std::uint32_t pendingValue = 0;
  std::uint32_t arrivingReg = 0;
  std::uint32_t arrivingValue = 0;
  std::uint64_t gteHash = 0; // GTE data and control registers and flags
  std::uint64_t ramHash = 0; // main RAM and the scratchpad; 0 when not hashed at this checkpoint
  std::uint32_t ramHashed = 0;
  std::uint32_t reserved = 0;
};
static_assert(sizeof(ExecutorTraceRecord) == 264, "tools/exec_trace_compare.py reads this layout");

class ExecutorTrace {
public:
  // Null when the file cannot be opened (logged).
  static std::unique_ptr<ExecutorTrace> open(const ExecutorTraceConfig &config);
  ~ExecutorTrace();
  ExecutorTrace(const ExecutorTrace &) = delete;
  ExecutorTrace &operator=(const ExecutorTrace &) = delete;

  // The block entry at `pc`, the `block`-th; records a checkpoint when this entry is one.
  void enter(const Core &core, const ExecutorState &state, std::uint32_t pc, std::uint64_t block) {
    if (block % config_.every == 0 || (block >= config_.from && block < config_.to)) {
      record(core, state, pc, block);
    }
    previousPc_ = pc;
  }

private:
  ExecutorTrace(ExecutorTraceConfig config, std::FILE *file);
  void record(const Core &core, const ExecutorState &state, std::uint32_t pc, std::uint64_t block);

  ExecutorTraceConfig config_;
  std::FILE *file_;
  std::uint32_t previousPc_ = 0;
  std::uint64_t checkpoints_ = 0;
};

// Arms the run-only diagnostics PSXPORT_EXEC_INTERPRET and PSXPORT_EXEC_TRACE* on `core`'s executor.
void configureExecutorDiagnostics(Core &core);

} // namespace psx::cpu
