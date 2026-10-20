// The typed configuration of the override differential, built once by the config owner.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psx::cpu {

inline constexpr std::uint32_t kDefaultDifferentialFirstCalls = 16;
inline constexpr std::uint32_t kDefaultDifferentialEveryKth = 64;
// Callee-frame residue below the entry sp: counted, not judged, since O32 promises nothing below sp.
inline constexpr std::uint32_t kDefaultDifferentialDeadStackBytes = 0x2000;
inline constexpr std::string_view kDefaultDifferentialReportPath = "scratch/override_differential.json";

// One requested override: a registered override NAME, or a guest entry ADDRESS written `0x...`.
struct DifferentialSelector {
  std::string text;
  std::optional<std::uint32_t> address;

  bool matches(std::uint32_t entry, std::string_view name) const {
    return address ? entry == *address : name == text;
  }
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
