// PSXPORT_DIAGNOSTIC_RUN: which side of a comparison this run is; both sides run the product runtime.
#pragma once

#include <cstdint>

namespace psx::config {

enum class DiagnosticRunMode : std::uint8_t {
  Product,
  CompareCandidate,
  CompareReference,
  Invalid,
};

DiagnosticRunMode diagnostic_run_mode();
const char *diagnostic_run_mode_name(DiagnosticRunMode mode);
bool diagnostic_run_is_comparison(DiagnosticRunMode mode);

} // namespace psx::config
