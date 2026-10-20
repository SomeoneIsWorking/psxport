#include "diagnostic_run.h"

#include "config_var.h"

#include <array>
#include <string>

namespace psx::config {
namespace {

Var<std::string> cv_diagnostic_run("PSXPORT_DIAGNOSTIC_RUN", "product", kRunOnly);
const Registration kRegistration{cv_diagnostic_run};

inline constexpr std::array kModes{DiagnosticRunMode::Product,
                                   DiagnosticRunMode::CompareCandidate,
                                   DiagnosticRunMode::CompareReference};

} // namespace

const char *diagnostic_run_mode_name(DiagnosticRunMode mode) {
  switch (mode) {
  case DiagnosticRunMode::Product:
    return "product";
  case DiagnosticRunMode::CompareCandidate:
    return "compare-candidate";
  case DiagnosticRunMode::CompareReference:
    return "compare-reference";
  case DiagnosticRunMode::Invalid:
    return "invalid";
  }
  return "invalid";
}

DiagnosticRunMode diagnostic_run_mode() {
  for (const DiagnosticRunMode mode : kModes) {
    if (cv_diagnostic_run.get() == diagnostic_run_mode_name(mode)) {
      return mode;
    }
  }
  return DiagnosticRunMode::Invalid;
}

bool diagnostic_run_is_comparison(DiagnosticRunMode mode) {
  return mode == DiagnosticRunMode::CompareCandidate || mode == DiagnosticRunMode::CompareReference;
}

} // namespace psx::config
