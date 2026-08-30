// The generated-substrate identity must cross the real game/framework install seam and be announced
// unconditionally. App/framework git commits cannot identify ignored generated code, so a diagnostic
// run without this value is explicitly UNKNOWN rather than silently borrowing the working tree's
// current generated files.
#include "recomp_iface.h"
#include "testutil.h"

#include <lucent/log.h>

#include <string>
#include <vector>

namespace {

constexpr char kIdentity[] = "recomp-2026-08-30.2-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void test_install_announces_and_preserves_generated_identity() {
  const RecompRegistry registry = {
      .substrate_id = kIdentity,
  };
  std::vector<std::string> lines;
  lucent::set_sink([&](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  psxport_install_recomp(&registry);
  lucent::set_sink(nullptr);

  CHECK(psxport_recomp() == &registry);
  CHECK(psxport_recomp()->substrate_id == kIdentity);
  CHECK(lines.size() == 1);
  CHECK(lines[0].find("generated substrate identity: ") != std::string::npos);
  CHECK(lines[0].find(kIdentity) != std::string::npos);
}

void test_missing_identity_is_reported_as_unknown() {
  const RecompRegistry registry{};
  std::vector<std::string> lines;
  lucent::set_sink([&](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  psxport_install_recomp(&registry);
  lucent::set_sink(nullptr);

  CHECK(lines.size() == 1);
  CHECK(lines[0].find("UNKNOWN") != std::string::npos);
  CHECK(lines[0].find("regenerate the substrate") != std::string::npos);
}

} // namespace

int main() {
  RUN(install_announces_and_preserves_generated_identity);
  RUN(missing_identity_is_reported_as_unknown);
  return pt_summary();
}
