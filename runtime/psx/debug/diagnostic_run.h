#pragma once

#include <string>

namespace psx::config {

// A comparison run is a diagnostic role, not a CPU-engine selection. Both roles execute the
// product's dynarec/native path; the role only keeps deliberate enhancements out of fidelity
// evidence and identifies which side of a comparison produced the result.
enum class DiagnosticRunMode {
  Product,
  CompareCandidate,
  CompareReference,
  Invalid,
};

DiagnosticRunMode diagnostic_run_mode();
const char *diagnostic_run_mode_name(DiagnosticRunMode mode);
bool diagnostic_run_is_comparison(DiagnosticRunMode mode);

// Typed, nestable runtime-layer override for harnesses and tests. Player code should use the
// compiled/configured diagnostic_run_mode() value and must not expose this as a gameplay selector.
class ScopedDiagnosticRun final {
public:
  explicit ScopedDiagnosticRun(DiagnosticRunMode mode);
  ~ScopedDiagnosticRun();
  ScopedDiagnosticRun(const ScopedDiagnosticRun &) = delete;
  ScopedDiagnosticRun &operator=(const ScopedDiagnosticRun &) = delete;

private:
  bool hadRuntimeValue_ = false;
  std::string previousRuntimeValue_;
};

} // namespace psx::config
