// The layered CVar configuration: typed accessors over the environment, layer precedence, title
// launch settings and the environment audit. Every audit check prints its denominator first, so an
// audit that scanned nothing cannot pass.
#include "testutil.h"

#include "config.h"
#include "config_var.h"
#include "config_vars.h"

#include <lucent/config.h>

#include <stdlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using psx::config::CVarBase;
using psx::config::Kind;
using psx::config::Layer;

namespace {

// Every test knob name lives under this prefix so it is impossible to collide with a real knob, and
// so the audit's "unknown" class has something deterministic to find.
const char *kBogus = "PSXPORT_TEST_NOT_A_KNOB_AT_ALL";

void set_env(const char *name, const char *value) {
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
  // Both caches must forget: lucent's (it memoises per full name) and ours (a CVar binds its env
  // Override once). Without this a test that sets a variable measures the previous test's answer.
  lucent::config::reset_cache();
  psx::config::reset_for_test();
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

void print_audit(const psx::config::EnvAudit &a) {
  fprintf(stderr,
          "  audit: %zu PSXPORT_* var(s) in the environment -> %zu declared, %zu identity, %zu UNKNOWN\n",
          a.set_in_env.size(),
          a.declared.size(),
          a.identity.size(),
          a.unknown.size());
  for (const std::string &s : a.unknown) {
    fprintf(stderr, "  audit:   UNKNOWN %s\n", s.c_str());
  }
}

// ── 1. TYPED ACCESSORS parse the environment exactly as lucent::config does ─────────────────────
void test_bool_knob_parses_like_lucent_flag(void) {
  const char *const values[] = {nullptr, "1", "0", "yes", "off", "", "banana"};
  int compared = 0;
  for (const char *v : values) {
    set_env("PSXPORT_NOAUDIO", v);
    const bool want = lucent::config::flag("PSXPORT_NOAUDIO");
    const bool got = psx::config::no_audio();
    fprintf(stderr, "  PSXPORT_NOAUDIO=%-8s no_audio=%d  lucent=%d\n", v ? (*v ? v : "\"\"") : "<unset>", got, want);
    CHECK_EQ(got, want);
    ++compared;
  }
  CHECK_EQ(compared, 7);
  set_env("PSXPORT_NOAUDIO", nullptr);
}

void test_int_and_text_knobs_parse_like_lucent(void) {
  set_env("PSXPORT_WATCHDOG", nullptr);
  const long fallback = psx::config::watchdog_seconds();
  const char *const ints[] = {nullptr, "0", "3", "45", "notanumber"};
  int compared = 0;
  for (const char *v : ints) {
    set_env("PSXPORT_WATCHDOG", v);
    const long want = lucent::config::number("PSXPORT_WATCHDOG", fallback);
    const long got = psx::config::watchdog_seconds();
    fprintf(stderr, "  PSXPORT_WATCHDOG=%-11s watchdog_seconds=%ld  lucent=%ld\n", v ? v : "<unset>", got, want);
    CHECK_EQ(got, want);
    ++compared;
  }
  const char *const texts[] = {nullptr, "", "external/psxport", "/some/where"};
  for (const char *v : texts) {
    set_env("PSXPORT_ASSET_DIR", v);
    const std::string want = lucent::config::text("PSXPORT_ASSET_DIR");
    const std::string got = psx::config::asset_dir();
    fprintf(stderr,
            "  PSXPORT_ASSET_DIR=%-18s asset_dir=%s  lucent=%s\n",
            v ? (*v ? v : "\"\"") : "<unset>",
            got.c_str(),
            want.c_str());
    CHECK_STREQ(got.c_str(), want.c_str());
    ++compared;
  }
  CHECK_EQ(compared, 9);
  set_env("PSXPORT_WATCHDOG", nullptr);
  set_env("PSXPORT_ASSET_DIR", nullptr);
}

// ── 2. PRECEDENCE: Default < Value < Override < Runtime ─────────────────────────────────────────
void test_layer_ladder_resolves_and_unresolves_in_the_documented_order(void) {
  psx::config::BoolVar v("PSXPORT_TEST_LADDER", false, "hermetic test knob");
  CHECK_EQ((int)v.layer(), (int)Layer::Default);
  CHECK_EQ(v.get(), false);

  v.set(Layer::Value, true); // psxport_settings.ini
  CHECK_EQ((int)v.layer(), (int)Layer::Value);
  CHECK_EQ(v.get(), true);

  v.set(Layer::Override, false); // env var / launch arg
  CHECK_EQ((int)v.layer(), (int)Layer::Override);
  CHECK_EQ(v.get(), false);

  v.set(Layer::Runtime, true); // REPL / debug-server, this run only
  CHECK_EQ((int)v.layer(), (int)Layer::Runtime);
  CHECK_EQ(v.get(), true);

  // ...and back down, one layer at a time. A ladder that only climbs hides which layer was actually
  // holding the value.
  v.clear(Layer::Runtime);
  CHECK_EQ((int)v.layer(), (int)Layer::Override);
  CHECK_EQ(v.get(), false);
  v.clear(Layer::Override);
  CHECK_EQ((int)v.layer(), (int)Layer::Value);
  CHECK_EQ(v.get(), true);
  v.clear(Layer::Value);
  CHECK_EQ((int)v.layer(), (int)Layer::Default);
  CHECK_EQ(v.get(), false);
}

// Runtime commands intentionally outrank launch-time overrides, so this precedence gets its own case:
// (launch arg) outranks their Speedrun layer; our Runtime layer outranks Override, because
// docs/config.md has always said a REPL `debug ...` "still overrides the environment for the rest of
// the run" and that behaviour must not change. A human typing at a live console is later and more
// specific than the environment the process was launched with.
void test_runtime_layer_outranks_the_environment_override(void) {
  psx::config::TextVar v("PSXPORT_TEST_RUNTIME_WINS", "", "hermetic test knob");
  v.set(Layer::Override, "from-env");
  CHECK_STREQ(v.get().c_str(), "from-env");
  v.set(Layer::Runtime, "from-repl");
  CHECK_STREQ(v.get().c_str(), "from-repl");
  CHECK_EQ((int)v.layer(), (int)Layer::Runtime);
}

// An Override is a launch argument: it must never be written back to the settings file. This is the
// bug the ladder prevents by construction — today mods.cpp saves whatever the field currently holds.
void test_override_and_runtime_layers_are_never_persisted(void) {
  psx::config::BoolVar v("PSXPORT_TEST_NOPERSIST", false, "hermetic test knob");
  v.set(Layer::Value, true);
  v.set(Layer::Override, false);
  v.set(Layer::Runtime, false);
  CHECK_EQ(v.get(), false);           // what the run sees
  CHECK_EQ(v.value_for_save(), true); // what the settings file must keep
}

// ── 3. THE AUDIT, run against BOTH classes ──────────────────────────────────────────────────────
void test_a_knob_set_in_the_environment_that_nothing_reads_is_reported(void) {
  set_env(kBogus, "1");
  const psx::config::EnvAudit a = psx::config::audit_environment();
  print_audit(a);
  // The denominator first: an audit that scanned nothing must not be able to look like a pass.
  CHECK(a.set_in_env.size() >= 1);
  CHECK(contains(a.set_in_env, std::string(kBogus) + "=1"));
  CHECK(contains(a.unknown, kBogus));
  set_env(kBogus, nullptr);
}

// The negative half of the same discriminator: a knob that IS registered, set in the environment,
// must be classified `declared` and must NOT appear as unknown. Without this case the audit could
// pass case 1 by simply calling everything unknown.
void test_a_registered_knob_set_in_the_environment_is_not_reported_as_unknown(void) {
  set_env("PSXPORT_NOAUDIO", "1");
  const psx::config::EnvAudit a = psx::config::audit_environment();
  print_audit(a);
  CHECK(contains(a.declared, "PSXPORT_NOAUDIO"));
  CHECK(!contains(a.unknown, "PSXPORT_NOAUDIO"));
  set_env("PSXPORT_NOAUDIO", nullptr);
}

// A title's launch key is unknown until the title asks for it, then classified identity.
void test_a_launch_setting_key_is_classified_identity_once_read(void) {
  set_env("PSXPORT_TEST_IDENTITY_DISC", "disc.chd");
  const psx::config::EnvAudit before = psx::config::audit_environment();
  print_audit(before);
  CHECK(contains(before.unknown, "PSXPORT_TEST_IDENTITY_DISC"));
  CHECK_STREQ(psx::config::launch_setting({"PSXPORT_TEST_IDENTITY_DISC"}).c_str(), "disc.chd");
  const psx::config::EnvAudit after = psx::config::audit_environment();
  print_audit(after);
  CHECK(contains(after.identity, "PSXPORT_TEST_IDENTITY_DISC"));
  CHECK(!contains(after.unknown, "PSXPORT_TEST_IDENTITY_DISC"));
  set_env("PSXPORT_TEST_IDENTITY_DISC", nullptr);
}

// Every key in the environment outranks every key in ./.env; within a source the first key wins.
void test_launch_setting_prefers_environment_then_dotenv_in_key_order(void) {
  const std::filesystem::path home = std::filesystem::current_path();
  const std::filesystem::path dir = home / "test_config_cvar_dotenv";
  std::filesystem::create_directories(dir);
  std::filesystem::current_path(dir);
  {
    std::ofstream env(".env");
    env << "# comment\n  PSXPORT_TEST_LS_TITLE= title.chd \nPSXPORT_TEST_LS_SHARED=shared.chd\n";
  }
  const auto both = [] {
    return psx::config::launch_setting({"PSXPORT_TEST_LS_TITLE", "PSXPORT_TEST_LS_SHARED"});
  };
  CHECK_STREQ(both().c_str(), "title.chd");
  CHECK_STREQ(psx::config::launch_setting({"PSXPORT_TEST_LS_SHARED", "PSXPORT_TEST_LS_TITLE"}).c_str(), "shared.chd");
  set_env("PSXPORT_TEST_LS_SHARED", "env-shared.chd");
  CHECK_STREQ(both().c_str(), "env-shared.chd");
  set_env("PSXPORT_TEST_LS_TITLE", "env-title.chd");
  CHECK_STREQ(both().c_str(), "env-title.chd");
  set_env("PSXPORT_TEST_LS_TITLE", nullptr);
  set_env("PSXPORT_TEST_LS_SHARED", nullptr);
  CHECK_STREQ(psx::config::launch_setting({"PSXPORT_TEST_LS_ABSENT"}).c_str(), "");
  std::filesystem::current_path(home);
  std::filesystem::remove_all(dir);
}

// The audit ships with its own proof that it can produce a positive. A `selftest()` that lives in
// the library (not only here) is what stops the audit from quietly degrading into a function that
// returns an empty list forever.
void test_the_audit_selftest_fires_in_the_shipping_library(void) {
  const bool ok = psx::config::selftest();
  fprintf(stderr, "  psx::config::selftest() -> %s\n", ok ? "true (audit produced its positive)" : "FALSE");
  CHECK(ok);
}

// The inventory must actually contain the knobs claimed as migrated. A registry that registered
// nothing would pass every other case in this file.
void test_the_declared_inventory_is_not_empty(void) {
  int n = 0;
  psx::config::enumerate([&n](CVarBase &v) {
    fprintf(stderr,
            "  cvar %-28s %-4s = %-18s [%s]\n",
            v.name(),
            psx::config::kind_name(v.kind()),
            v.value_text().c_str(),
            psx::config::layer_name(v.layer()));
    ++n;
  });
  fprintf(stderr, "  %d CVar(s) registered\n", n);
  CHECK(n >= 7);
  CHECK(psx::config::find("PSXPORT_NOAUDIO") != nullptr);
  CHECK(psx::config::find("PSXPORT_DIAGNOSTIC_RUN") != nullptr);
  CHECK(psx::config::find("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT") != nullptr);
}

} // namespace

int main(void) {
  RUN(bool_knob_parses_like_lucent_flag);
  RUN(int_and_text_knobs_parse_like_lucent);
  RUN(layer_ladder_resolves_and_unresolves_in_the_documented_order);
  RUN(runtime_layer_outranks_the_environment_override);
  RUN(override_and_runtime_layers_are_never_persisted);
  RUN(a_knob_set_in_the_environment_that_nothing_reads_is_reported);
  RUN(a_registered_knob_set_in_the_environment_is_not_reported_as_unknown);
  RUN(a_launch_setting_key_is_classified_identity_once_read);
  RUN(launch_setting_prefers_environment_then_dotenv_in_key_order);
  RUN(the_audit_selftest_fires_in_the_shipping_library);
  RUN(the_declared_inventory_is_not_empty);
  return psx::test::summary();
}
