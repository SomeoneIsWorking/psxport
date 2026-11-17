#include "executor_trace.h"

#include "core.h"
#include "diagnostic_config.h"
#include "executor_state.h"
#include "guest_executor.h"

#include <lucent/log.h>

#include <cstring>

namespace psx::cpu {
namespace {

std::uint64_t hashBytes(const std::uint8_t *bytes, std::size_t size, std::uint64_t hash = 0xCBF29CE484222325ull) {
  std::size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    std::uint64_t word;
    std::memcpy(&word, bytes + i, 8);
    hash = (hash ^ word) * 0x100000001B3ull;
    hash ^= hash >> 29;
  }
  for (; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  }
  return hash;
}

} // namespace

std::unique_ptr<ExecutorTrace> ExecutorTrace::open(const ExecutorTraceConfig &config) {
  std::FILE *file = std::fopen(config.path.c_str(), "wb");
  if (file == nullptr) {
    lucent::error("exec-trace", "cannot write the executor trace to {}", config.path);
    return nullptr;
  }
  ExecutorTraceConfig armed = config;
  armed.every = armed.every == 0 ? 1 : armed.every;
  armed.ramEvery = armed.ramEvery == 0 ? 1 : armed.ramEvery;
  lucent::info("exec-trace",
               "tracing block entries to {}: a checkpoint every {} block(s), RAM every {} checkpoint(s), every "
               "entry in blocks [{}, {})",
               armed.path,
               armed.every,
               armed.ramEvery,
               armed.from,
               armed.to);
  return std::unique_ptr<ExecutorTrace>(new ExecutorTrace(armed, file));
}

ExecutorTrace::ExecutorTrace(ExecutorTraceConfig config, std::FILE *file) : config_(std::move(config)), file_(file) {
  std::setvbuf(file_, nullptr, _IOFBF, 1u << 20);
}

ExecutorTrace::~ExecutorTrace() {
  std::fclose(file_);
  lucent::info("exec-trace", "wrote {} checkpoint(s) to {}", checkpoints_, config_.path);
}

void ExecutorTrace::record(const Core &core, const ExecutorState &state, std::uint32_t pc, std::uint64_t block) {
  ExecutorTraceRecord out;
  out.block = block;
  out.executed = state.executed;
  out.pc = pc;
  out.previousPc = previousPc_;
  std::memcpy(out.gpr, core.r, sizeof(out.gpr));
  out.hi = core.hi;
  out.lo = core.lo;
  std::memcpy(out.cop0, core.cop0, sizeof(out.cop0));
  // A slot holding no register ($zero) holds no load: its value is whatever the engine left there.
  out.pendingReg = state.pending.reg;
  out.pendingValue = state.pending.reg != 0 ? state.pending.value : 0u;
  out.arrivingReg = state.arriving.reg;
  out.arrivingValue = state.arriving.reg != 0 ? state.arriving.value : 0u;
  const GteRawState gte = core.gte().saveRaw();
  out.gteHash = hashBytes(reinterpret_cast<const std::uint8_t *>(gte.reg), sizeof(gte.reg));
  out.gteHash = hashBytes(reinterpret_cast<const std::uint8_t *>(&gte.flags), sizeof(gte.flags), out.gteHash);
  const bool window = block >= config_.from && block < config_.to;
  if (window || checkpoints_ % config_.ramEvery == 0) {
    out.ramHash = hashBytes(core.ram, sizeof(core.ram));
    out.ramHash = hashBytes(core.scratch, sizeof(core.scratch), out.ramHash);
    out.ramHashed = 1;
  }
  std::fwrite(&out, sizeof(out), 1, file_);
  ++checkpoints_;
}

void configureExecutorDiagnostics(Core &core) {
  if (psx::config::exec_interpret()) {
    core.executor().interpretOnly();
  }
  if (psx::config::exec_nolink()) {
    core.executor().noLinking();
  }
  const std::string path = psx::config::exec_trace_path();
  if (path.empty()) {
    return;
  }
  auto count = [](long value) {
    return value < 0 ? 0u : static_cast<std::uint64_t>(value);
  };
  core.executor().attachTrace(ExecutorTrace::open({path,
                                                   count(psx::config::exec_trace_every()),
                                                   count(psx::config::exec_trace_ram_every()),
                                                   count(psx::config::exec_trace_from()),
                                                   count(psx::config::exec_trace_to())}));
}

} // namespace psx::cpu
