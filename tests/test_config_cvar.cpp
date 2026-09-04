// The layered CVar configuration system: precedence, compatibility, and the env audit.
//
// WHY THIS EXISTS. psxport has 201 distinct PSXPORT_* knobs read through three mechanisms that had
// NO documented precedence between them: env vars (cfg_on/cfg_int/cfg_str), psxport_settings.ini
// (mods.cpp), and REPL/debug-server `debug ...` commands. "The REPL still overrides the environment
// for the rest of the run" was the only precedence sentence anywhere in the repo.
//
// And one knob, PSXPORT_FPS60, is documented in docs/config.md and read by NOTHING. It has been set
// on real runs. Nothing said so. A run measured with a flag that never applied is a measurement of
// the wrong thing that looks exactly like a measurement of the right thing.
//
// So the three cases below, in order of how much they matter:
//
//   1. COMPATIBILITY (the important one). A knob that has been migrated onto a CVar must resolve
//      IDENTICALLY to what the pre-migration code returned. The oracle is not a remembered constant
//      — it is the pre-migration implementation itself, `lucent::config::flag/number/text`, called
//      side by side with cfg_*. Same input, same environment, same answer, or red.
//   2. PRECEDENCE. Default < Value < Override < Runtime, resolving and UN-resolving in that order,
//      and an Override never contaminating what gets persisted.
//   3. THE AUDIT. A knob set in the environment that resolves to nothing must be REPORTED. Run
//      against BOTH classes: a bogus name must land in `unknown`, and a real registered knob must
//      NOT. A discriminator only checked on one class is not a discriminator.
//
// WHAT A NEGATIVE PRINTS: every audit assertion first prints the full denominator — how many
// PSXPORT_* variables were seen in the environment and how they were classified — so "0 unknown"
// can never be confused with "the audit never looked". An audit that scanned nothing is a failure,
// not a pass.
#include "testutil.h"

#include "config.h"
#include "config_var.h"
#include "config_vars.h"

#include "cfg.h"

#include <lucent/config.h>

#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

using psx::config::CVarBase;
using psx::config::Kind;
using psx::config::Layer;

// Every test knob name lives under this prefix so it is impossible to collide with a real knob, and
// so the audit's "unknown" class has something deterministic to find.
static const char *kBogus = "PSXPORT_TEST_NOT_A_KNOB_AT_ALL";

static void set_env(const char *name, const char *value) {
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

static bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static void print_audit(const psx::config::EnvAudit &a) {
  fprintf(stderr,
          "  audit: %zu PSXPORT_* var(s) in the environment -> %zu declared, %zu legacy, %zu UNKNOWN\n",
          a.set_in_env.size(),
          a.declared.size(),
          a.legacy.size(),
          a.unknown.size());
  for (const std::string &s : a.unknown) {
    fprintf(stderr, "  audit:   UNKNOWN %s\n", s.c_str());
  }
}

// ── 1. COMPATIBILITY: a migrated knob resolves exactly as it did before migration ───────────────
// The oracle is the old implementation, run side by side. cfg_on() used to BE
// `lucent::config::flag(name) ? 1 : 0`; if routing it through a CVar changes any answer for any
// input, that is a knob whose meaning silently moved.
static void test_migrated_bool_knob_matches_pre_migration_env_behaviour(void) {
  const char *const values[] = {nullptr, "1", "0", "yes", "off", "", "banana"};
  int compared = 0;
  for (const char *v : values) {
    set_env("PSXPORT_NOAUDIO", v);
    const int want = lucent::config::flag("PSXPORT_NOAUDIO") ? 1 : 0; // the pre-migration body
    const int got = cfg_on("PSXPORT_NOAUDIO");
    fprintf(
        stderr, "  PSXPORT_NOAUDIO=%-8s cfg_on=%d  pre-migration=%d\n", v ? (*v ? v : "\"\"") : "<unset>", got, want);
    CHECK_EQ(got, want);
    ++compared;
  }
  fprintf(stderr, "  compared %d value(s) against the pre-migration implementation\n", compared);
  CHECK_EQ(compared, 7);
  set_env("PSXPORT_NOAUDIO", nullptr);
}

static void test_migrated_int_and_text_knobs_match_pre_migration_env_behaviour(void) {
  const char *const ints[] = {nullptr, "0", "3", "45", "notanumber"};
  int compared = 0;
  for (const char *v : ints) {
    set_env("PSXPORT_WATCHDOG", v);
    const int want = (int)lucent::config::number("PSXPORT_WATCHDOG", 3);
    const int got = cfg_int("PSXPORT_WATCHDOG", 3);
    fprintf(stderr, "  PSXPORT_WATCHDOG=%-11s cfg_int=%d  pre-migration=%d\n", v ? v : "<unset>", got, want);
    CHECK_EQ(got, want);
    ++compared;
  }
  const char *const texts[] = {nullptr, "", "external/psxport", "/some/where"};
  for (const char *v : texts) {
    set_env("PSXPORT_ASSET_DIR", v);
    const std::string &ref = lucent::config::text("PSXPORT_ASSET_DIR");
    const char *want = ref.empty() ? nullptr : ref.c_str(); // the pre-migration body, verbatim
    const char *got = cfg_str("PSXPORT_ASSET_DIR");
    fprintf(stderr,
            "  PSXPORT_ASSET_DIR=%-18s cfg_str=%s  pre-migration=%s\n",
            v ? (*v ? v : "\"\"") : "<unset>",
            got ? got : "(null)",
            want ? want : "(null)");
    CHECK_EQ(got == nullptr, want == nullptr);
    if (got && want) {
      CHECK_STREQ(got, want);
    }
    ++compared;
  }
  fprintf(stderr, "  compared %d value(s) against the pre-migration implementation\n", compared);
  CHECK_EQ(compared, 9);
  set_env("PSXPORT_WATCHDOG", nullptr);
  set_env("PSXPORT_ASSET_DIR", nullptr);
}

// The producer-DB knobs, migrated 2026-08-12. PSXPORT_PRODUCERS_DIR is the migration that can bite: its
// pre-migration call sites read cfg_str and applied `if (!dir || !*dir) dir = "scratch/producers"` at each
// site, so the CVar must carry that SAME default — a CVar defaulting to empty would silently write the
// per-run JSONL and the claim set to "/run-...jsonl" at the filesystem root. Asserted on the resolved
// value, both unset and set, because that is the behaviour the two call sites depend on.
static void test_producer_db_knobs_resolve_with_their_pre_migration_defaults(void) {
  set_env("PSXPORT_PRODUCERS_DIR", nullptr);
  CHECK_STREQ(psx::config::cv_producers_dir.get().c_str(), "scratch/producers");
  set_env("PSXPORT_PRODUCERS_DIR", "scratch/other");
  CHECK_STREQ(psx::config::cv_producers_dir.get().c_str(), "scratch/other");
  set_env("PSXPORT_PRODUCERS_DIR", nullptr);

  // PSXPORT_PRODUCERS_DB defaults EMPTY on purpose — empty means "derive <DIR>/claims.txt", and a
  // non-empty default would make that derivation unreachable.
  set_env("PSXPORT_PRODUCERS_DB", nullptr);
  CHECK(psx::config::cv_producers_db.get().empty());
  set_env("PSXPORT_PRODUCERS_DB", "scratch/producers/run-x.jsonl");
  CHECK_STREQ(psx::config::cv_producers_db.get().c_str(), "scratch/producers/run-x.jsonl");
  set_env("PSXPORT_PRODUCERS_DB", nullptr);
  fprintf(stderr, "  producer-DB knobs: 4 resolution(s) checked incl. both defaults\n");
}

// An UNMIGRATED knob — the compatibility path that carries the other ~190 — must be untouched too.
static void test_unmigrated_knob_still_resolves_through_the_env(void) {
  set_env("PSXPORT_TEST_UNMIGRATED", "1");
  CHECK_EQ(cfg_on("PSXPORT_TEST_UNMIGRATED"), 1);
  CHECK(psx::config::find("PSXPORT_TEST_UNMIGRATED") == nullptr);
  set_env("PSXPORT_TEST_UNMIGRATED", "0");
  CHECK_EQ(cfg_on("PSXPORT_TEST_UNMIGRATED"), 0);
  set_env("PSXPORT_TEST_UNMIGRATED", nullptr);
  CHECK_EQ(cfg_on("PSXPORT_TEST_UNMIGRATED"), 0);
}

// ── 2. PRECEDENCE: Default < Value < Override < Runtime ─────────────────────────────────────────
static void test_layer_ladder_resolves_and_unresolves_in_the_documented_order(void) {
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
static void test_runtime_layer_outranks_the_environment_override(void) {
  psx::config::TextVar v("PSXPORT_TEST_RUNTIME_WINS", "", "hermetic test knob");
  v.set(Layer::Override, "from-env");
  CHECK_STREQ(v.get().c_str(), "from-env");
  v.set(Layer::Runtime, "from-repl");
  CHECK_STREQ(v.get().c_str(), "from-repl");
  CHECK_EQ((int)v.layer(), (int)Layer::Runtime);
}

// An Override is a launch argument: it must never be written back to the settings file. This is the
// bug the ladder prevents by construction — today mods.cpp saves whatever the field currently holds.
static void test_override_and_runtime_layers_are_never_persisted(void) {
  psx::config::BoolVar v("PSXPORT_TEST_NOPERSIST", false, "hermetic test knob");
  v.set(Layer::Value, true);
  v.set(Layer::Override, false);
  v.set(Layer::Runtime, false);
  CHECK_EQ(v.get(), false);           // what the run sees
  CHECK_EQ(v.value_for_save(), true); // what the settings file must keep
}

// ── 3. THE AUDIT, run against BOTH classes ──────────────────────────────────────────────────────
static void test_a_knob_set_in_the_environment_that_nothing_reads_is_reported(void) {
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
static void test_a_registered_knob_set_in_the_environment_is_not_reported_as_unknown(void) {
  set_env("PSXPORT_NOAUDIO", "1");
  const psx::config::EnvAudit a = psx::config::audit_environment();
  print_audit(a);
  CHECK(contains(a.declared, "PSXPORT_NOAUDIO"));
  CHECK(!contains(a.unknown, "PSXPORT_NOAUDIO"));
  set_env("PSXPORT_NOAUDIO", nullptr);
}

// ...and the third class: an UNMIGRATED knob is only observable once something reads it. Before the
// read it is indistinguishable from a typo — which is honest, and is exactly why the audit reports
// the legacy count separately instead of folding it into `declared`.
static void test_an_unmigrated_knob_is_classified_legacy_once_it_has_been_read(void) {
  set_env("PSXPORT_TEST_LEGACY", "1");
  const psx::config::EnvAudit before = psx::config::audit_environment();
  print_audit(before);
  CHECK(contains(before.unknown, "PSXPORT_TEST_LEGACY"));
  (void)cfg_on("PSXPORT_TEST_LEGACY");
  const psx::config::EnvAudit after = psx::config::audit_environment();
  print_audit(after);
  CHECK(contains(after.legacy, "PSXPORT_TEST_LEGACY"));
  CHECK(!contains(after.unknown, "PSXPORT_TEST_LEGACY"));
  set_env("PSXPORT_TEST_LEGACY", nullptr);
}

// The audit ships with its own proof that it can produce a positive. A `selftest()` that lives in
// the library (not only here) is what stops the audit from quietly degrading into a function that
// returns an empty list forever.
static void test_the_audit_selftest_fires_in_the_shipping_library(void) {
  const bool ok = psx::config::selftest();
  fprintf(stderr, "  psx::config::selftest() -> %s\n", ok ? "true (audit produced its positive)" : "FALSE");
  CHECK(ok);
}

// The inventory must actually contain the knobs claimed as migrated. A registry that registered
// nothing would pass every other case in this file.
static void test_the_declared_inventory_is_not_empty(void) {
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
  CHECK(psx::config::find("PSXPORT_FPS60") != nullptr); // documented since forever, read by NOTHING
}

int main(void) {
  RUN(migrated_bool_knob_matches_pre_migration_env_behaviour);
  RUN(migrated_int_and_text_knobs_match_pre_migration_env_behaviour);
  RUN(producer_db_knobs_resolve_with_their_pre_migration_defaults);
  RUN(unmigrated_knob_still_resolves_through_the_env);
  RUN(layer_ladder_resolves_and_unresolves_in_the_documented_order);
  RUN(runtime_layer_outranks_the_environment_override);
  RUN(override_and_runtime_layers_are_never_persisted);
  RUN(a_knob_set_in_the_environment_that_nothing_reads_is_reported);
  RUN(a_registered_knob_set_in_the_environment_is_not_reported_as_unknown);
  RUN(an_unmigrated_knob_is_classified_legacy_once_it_has_been_read);
  RUN(the_audit_selftest_fires_in_the_shipping_library);
  RUN(the_declared_inventory_is_not_empty);
  return pt_summary();
}
