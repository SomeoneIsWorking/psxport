#pragma once

#include <cstdint>

namespace psx::cpu {

inline constexpr std::uint64_t kDefaultMaxFallbackBlocksPerExecution = 1;

struct FallbackPolicy {
  bool valid = true;
  std::uint64_t maxBlocksPerExecution = kDefaultMaxFallbackBlocksPerExecution;
  std::int64_t configuredMaxBlocks = static_cast<std::int64_t>(kDefaultMaxFallbackBlocksPerExecution);
};

inline constexpr FallbackPolicy fallbackPolicyFromConfigured(std::int64_t configuredMaxBlocks) {
  if (configuredMaxBlocks < 0) {
    return {false, 0, configuredMaxBlocks};
  }
  return {true, static_cast<std::uint64_t>(configuredMaxBlocks), configuredMaxBlocks};
}

inline constexpr FallbackPolicy defaultFallbackPolicy() {
  return fallbackPolicyFromConfigured(static_cast<std::int64_t>(kDefaultMaxFallbackBlocksPerExecution));
}

using FallbackPolicyProvider = FallbackPolicy (*)();

} // namespace psx::cpu
