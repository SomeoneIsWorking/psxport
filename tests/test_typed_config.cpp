// The typed launch config: read from the environment as a BoolVar reads it, and counted by the audit.
#include "testutil.h"

#include "config.h"
#include "config_var.h"
#include "config_vars.h"
#include "typed_config.h"

#include <lucent/config.h>

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

namespace {

using psx::config::Config;

void setEnv(const std::string &name, const char *value) {
  if (value != nullptr) {
    setenv(name.c_str(), value, 1);
  } else {
    unsetenv(name.c_str());
  }
  lucent::config::reset_cache();
  psx::config::reset_for_test();
}

void clearAll() {
  setEnv("APPDIR", nullptr);
  for (const std::string_view name : Config::environmentNames()) {
    setEnv(std::string(name), nullptr);
  }
}

void test_nothing_set_is_the_neutral_config() {
  clearAll();
  const Config config = Config::fromEnvironment();
  CHECK(!config.unpaced);
  CHECK(!config.repl);
  CHECK(!config.fps60.has_value());
  CHECK(!config.packageRoot.has_value());
}

// Every field resolves a value exactly as a BoolVar's environment layer resolves the same text.
void test_a_field_reads_a_value_as_a_boolvar_does() {
  const psx::config::BoolVar probe("PSXPORT_TEST_TYPED_PROBE", false, "hermetic test knob", false);
  const std::array<const char *, 7> values{"1", "0", "yes", "Off", "FALSE", "", "banana"};
  for (const char *value : values) {
    for (const std::string_view name : Config::environmentNames()) {
      setEnv(std::string(name), value);
    }
    setEnv("PSXPORT_TEST_TYPED_PROBE", value);
    const Config config = Config::fromEnvironment();
    CHECK_EQ(config.unpaced, probe.get());
    CHECK_EQ(config.repl, probe.get());
    CHECK(config.fps60.has_value());
    CHECK_EQ(config.fps60.value_or(!probe.get()), probe.get());
  }
  setEnv("PSXPORT_TEST_TYPED_PROBE", nullptr);
  clearAll();
}

void test_fps60_keeps_unset_apart_from_off() {
  clearAll();
  CHECK(!Config::fromEnvironment().fps60.has_value());
  setEnv("PSXPORT_FPS60", "0");
  const Config off = Config::fromEnvironment();
  CHECK(off.fps60.has_value());
  CHECK(!off.fps60.value_or(true));
  clearAll();
}

void test_describe_names_every_field() {
  const std::string text = Config{.unpaced = true, .repl = false, .fps60 = std::nullopt}.describe();
  CHECK_STREQ(text.c_str(), "nopace=on repl=off fps60=saved package=none");
}

// Only an absolute APPDIR is a package root.
void test_package_root_is_an_absolute_appdir() {
  setEnv("APPDIR", "/mnt/app");
  CHECK(Config::fromEnvironment().packageRoot == std::filesystem::path("/mnt/app"));
  setEnv("APPDIR", "relative/app");
  CHECK(!Config::fromEnvironment().packageRoot.has_value());
  setEnv("APPDIR", nullptr);
  CHECK(!Config::fromEnvironment().packageRoot.has_value());
}

// A packaged run opens a window and reads its shipped assets unless the environment says otherwise.
void test_packaged_defaults_ask_for_a_window_and_the_shipped_assets() {
  setEnv("PSXPORT_VK_WINDOW", nullptr);
  setEnv("PSXPORT_VK_HEADLESS", nullptr);
  setEnv("PSXPORT_ASSET_DIR", nullptr);
  CHECK(!psx::config::windowRequested());
  psx::config::applyPackagedDefaults("/mnt/app");
  CHECK(psx::config::windowRequested());
  CHECK(psx::config::asset_dir() == "/mnt/app/usr/share/psxport");
  setEnv("PSXPORT_VK_HEADLESS", "1");
  CHECK(!psx::config::windowRequested());
  setEnv("PSXPORT_VK_HEADLESS", nullptr);
}

// A variable the typed config reads is declared, not an unknown that did nothing.
void test_the_audit_counts_typed_names_as_declared() {
  CHECK_EQ(Config::environmentNames().size(), 3u);
  for (const std::string_view name : Config::environmentNames()) {
    const std::string key(name);
    setEnv(key, "1");
    const psx::config::EnvAudit audit = psx::config::audit_environment();
    CHECK(std::find(audit.declared.begin(), audit.declared.end(), key) != audit.declared.end());
    CHECK(std::find(audit.unknown.begin(), audit.unknown.end(), key) == audit.unknown.end());
    setEnv(key, nullptr);
  }
}

} // namespace

int main() {
  RUN(nothing_set_is_the_neutral_config);
  RUN(a_field_reads_a_value_as_a_boolvar_does);
  RUN(fps60_keeps_unset_apart_from_off);
  RUN(describe_names_every_field);
  RUN(package_root_is_an_absolute_appdir);
  RUN(packaged_defaults_ask_for_a_window_and_the_shipped_assets);
  RUN(the_audit_counts_typed_names_as_declared);
  return psx::test::summary();
}
