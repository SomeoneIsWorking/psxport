// The typed, immutable configuration of the per-function override differential. The configuration
// owner (`runtime/psx/config.cpp`, `psx::config::override_differential_config`) reads the knobs once and
// converts them through `overrideDifferentialConfigFrom`; nothing else reads the environment.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psx::cpu {

inline constexpr std::uint32_t kDefaultDifferentialFirstCalls = 16;
inline constexpr std::uint32_t kDefaultDifferentialEveryKth = 64;
// Bytes below the entry stack pointer whose differences are CALLEE FRAME residue, not a result. The
// O32 contract gives a caller nothing below its own `sp` after a return, and an original that built a
// frame leaves its locals there while a native override that needs no frame does not. The window is
// bounded because main RAM below the stack is also ordinary data; differing bytes inside it are still
// counted and reported, only not judged.
inline constexpr std::uint32_t kDefaultDifferentialDeadStackBytes = 0x2000;
inline constexpr std::string_view kDefaultDifferentialReportPath = "scratch/override_differential.json";

// One requested override: a registered override NAME, or a guest entry ADDRESS written `0x...`.
struct DifferentialSelector {
  std::string text;
  std::optional<std::uint32_t> address;
};

struct OverrideDifferentialConfig {
  std::vector<DifferentialSelector> selectors;
  std::uint32_t firstCalls = kDefaultDifferentialFirstCalls;
  std::uint32_t everyKth = kDefaultDifferentialEveryKth; // 0 = only the first `firstCalls`
  std::uint32_t deadStackBytes = kDefaultDifferentialDeadStackBytes;
  std::string reportPath{kDefaultDifferentialReportPath};
  // Set when the configured text is refused; the differential is then not armed at all.
  std::optional<std::string> error;

  bool enabled() const {
    return !selectors.empty() && !error;
  }
};

// Converts the raw knob values. `selectors` is a comma- or space-separated list; an address token must
// carry the `0x` prefix, so an override named in hex digits is never mistaken for an address. Negative
// counts, an empty report path, or an unparsable address token produce a config with `error` set.
OverrideDifferentialConfig overrideDifferentialConfigFrom(std::string_view selectors,
                                                          std::int64_t firstCalls,
                                                          std::int64_t everyKth,
                                                          std::int64_t deadStackBytes,
                                                          std::string_view reportPath);

} // namespace psx::cpu
