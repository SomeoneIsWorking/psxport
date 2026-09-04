#include "config.h"
#include "config_vars.h"
#include "diagnostic_run.h"

#include "testutil.h"

#include <lucent/log.h>

#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<std::pair<lucent::Level, std::string>> lines;

class LogCapture final {
public:
  LogCapture() {
    lines.clear();
    lucent::set_sink([](lucent::Level level, std::string_view line) {
      lines.emplace_back(level, line);
    });
  }
  ~LogCapture() {
    lucent::set_sink(nullptr);
  }
};

bool logged(lucent::Level level, std::string_view needle) {
  for (const auto &[actualLevel, line] : lines) {
    if (actualLevel == level && line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

static void test_product_role_admits_requested_enhancement_through_typed_api() {
  LogCapture capture;
  psx::config::BoolVar enhancement("PSXPORT_TEST_PRODUCT_ENH", true, "test-only enhancement");
  psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::Product);

  CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::Product);
  CHECK(!psx::config::diagnostic_run_is_comparison(psx::config::diagnostic_run_mode()));
  CHECK(psx::config::enh(enhancement));
  CHECK(logged(lucent::Level::Info, "PSXPORT_TEST_PRODUCT_ENH enhancement active in product run"));
}

static void test_both_compare_roles_suppress_requested_enhancement_at_production_gate() {
  LogCapture capture;
  psx::config::BoolVar enhancement("PSXPORT_TEST_COMPARE_ENH", true, "test-only enhancement");
  {
    psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::CompareCandidate);
    CHECK(psx::config::diagnostic_run_is_comparison(psx::config::diagnostic_run_mode()));
    CHECK(!psx::config::enh(enhancement));
    CHECK(logged(lucent::Level::Warn, "compare-candidate"));
  }
  psx::config::reset_for_test();
  {
    psx::config::ScopedDiagnosticRun run(psx::config::DiagnosticRunMode::CompareReference);
    CHECK(psx::config::diagnostic_run_is_comparison(psx::config::diagnostic_run_mode()));
    CHECK(!psx::config::enh(enhancement));
    CHECK(logged(lucent::Level::Warn, "compare-reference"));
  }
}

static void test_typed_scope_is_nested_and_restores_the_previous_role() {
  psx::config::ScopedDiagnosticRun product(psx::config::DiagnosticRunMode::Product);
  CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::Product);
  {
    psx::config::ScopedDiagnosticRun reference(psx::config::DiagnosticRunMode::CompareReference);
    CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::CompareReference);
    {
      psx::config::ScopedDiagnosticRun candidate(psx::config::DiagnosticRunMode::CompareCandidate);
      CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::CompareCandidate);
    }
    CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::CompareReference);
  }
  CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::Product);
}

static void test_invalid_config_is_typed_and_fails_closed_at_enhancement_gate() {
  LogCapture capture;
  psx::config::BoolVar enhancement("PSXPORT_TEST_INVALID_RUN_ENH", true, "test-only enhancement");
  CHECK(psx::config::set_runtime("PSXPORT_DIAGNOSTIC_RUN", "typo"));
  CHECK_EQ(psx::config::diagnostic_run_mode(), psx::config::DiagnosticRunMode::Invalid);
  CHECK(!psx::config::enh(enhancement));
  CHECK(logged(lucent::Level::Error, "PSXPORT_DIAGNOSTIC_RUN='typo' is invalid"));
  CHECK(psx::config::clear_runtime("PSXPORT_DIAGNOSTIC_RUN"));
}

int main() {
  RUN(product_role_admits_requested_enhancement_through_typed_api);
  RUN(both_compare_roles_suppress_requested_enhancement_at_production_gate);
  RUN(typed_scope_is_nested_and_restores_the_previous_role);
  RUN(invalid_config_is_typed_and_fails_closed_at_enhancement_gate);
  return pt_summary();
}
