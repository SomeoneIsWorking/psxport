#pragma once

#include "dynarec_capabilities.h"
#include "execution_exit.h"
#include "guest_program_image.h"

#include <cstdint>
#include <memory>

class Core;

namespace psx::cpu {

enum class InterpreterFallbackReason : std::uint8_t {
  SelfModifyingCode,
  UnsupportedBlock,
  CompilationFailed,
  LoadDelayHazard,
  UnsafeInstructionFetch,
};

struct InterpreterFallbackCounters {
  std::uint64_t calls = 0;
  std::uint64_t instructions = 0;
  std::uint64_t compilationFailed = 0;
  std::uint64_t selfModifyingCode = 0;
  std::uint64_t unsupportedBlock = 0;
  std::uint64_t loadDelayHazard = 0;
  std::uint64_t unsafeInstructionFetch = 0;
};

struct ExecutorCounters {
  std::uint64_t calls = 0;
  std::uint64_t translatedBlocks = 0;
  std::uint64_t executedBlocks = 0;
  std::uint64_t cacheHits = 0;
  std::uint64_t cacheMisses = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t faults = 0;
  InterpreterFallbackCounters fallback;
};

class LightrecExecutor {
public:
  explicit LightrecExecutor(Core &core);
  ~LightrecExecutor();
  LightrecExecutor(const LightrecExecutor &) = delete;
  LightrecExecutor &operator=(const LightrecExecutor &) = delete;

  ExecutionResult execute(std::uint32_t guestAddress, ExecutionBudget budget);
  ExecutionResult executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget);
  void requestStop();
  void invalidate(GuestAddressRange range);
  void invalidateAll();
  const ExecutorCounters &counters() const;
  bool available() const;
  static constexpr DynarecBackendCapabilities backendCapabilities() {
    return kLightrecBackendCapabilities;
  }

private:
  ExecutionResult executeWithBoundary(std::uint32_t guestAddress,
                                      std::uint32_t returnAddress,
                                      bool stopAtReturn,
                                      ExecutionBudget budget);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace psx::cpu
