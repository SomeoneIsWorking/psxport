// PSXPORT_ENH on the CVar ladder, and the ORACLE/SBS suppression as a resolve-time hook.
//
// WHY THIS EXISTS. `cfg_enh()` read `lucent::config::text("PSXPORT_ENH")` straight into a
// function-local seeded static. Three consequences, and this file is one case per consequence:
//
//   1. NO LADDER. No Value layer (the settings file could not select an enhancement), no Runtime
//      layer (the REPL could not), no row in the CVar dump, no row in the environment audit. The
//      USER's standing ruling is "use cvars not cfg_str", which that shape cannot satisfy.
//   2. THE SEEDED STATIC IS A ONE-SHOT. Once the first call had parsed the environment, nothing
//      could change the answer for the rest of the process — which is also why a test could not
//      exercise more than ONE configuration of it per process.
//   3. ONE GLOBAL WARNING. The suppression notice fired once for the whole run, naming the raw
//      PSXPORT_ENH string. A run with two enhancements asked for could not name both, so the second
//      one read as never having been asked for. Mega Man X4's game/core/enhancements.cpp had to
//      REPRODUCE the suppression rule to get per-knob warnings — i.e. keep a second copy of the
//      definition of "what a byte-compare run IS". If those two copies ever diverge, one of them
//      fails to recognise an SBS variant and a CONTAMINATED byte-compare still looks clean.
//
// WHICH CASE MATTERS MOST — CORRECTED 2026-08-12 AFTER SABOTAGE, because the first answer was wrong and
// a confidently-wrong note sends the next session down the same path. This file used to say it was
// `test_compare_run_is_the_single_definition_of_a_byte_compare_run` — the eight-combination check of the
// framework's verdict against the hand-written three-input expression. That case is real (it is the gate
// against a consumer's copy drifting) but it exercises the PURE PREDICATE, and a verifier deleted the
// suppression from `enh_gate()` outright — the function the game actually calls — while this whole file
// and the shipping `selftest()` stayed GREEN. An `PSXPORT_ORACLE` run would have enabled every
// enhancement: the byte-compare oracle contaminated, with two gates reporting no disagreement.
//
// So the two cases that matter most are the ones that drive the SHIPPING path and can see it move:
//   * `test_the_oracle_moves_mid_process_with_no_caches_cleared` (5b) — every other case here resets
//     the caches between configurations, so none of them can observe a gate answering from a one-time
//     binding. That is what let "cv_oracle is re-read every call so the REPL can move it" survive as a
//     comment with nothing behind it.
//   * `test_the_shipping_selftest_passes` (9), which now runs a `selftest()` that drives `enh_gate()`
//     itself rather than the predicate beside it.
// A test of the code beside the shipping code is not a test of the shipping code.
//
// WHAT A NEGATIVE PRINTS: every case prints what it fed in and what came back, and the log-capture
// cases print the captured line count with the lines themselves — so "no suppression warning" can
// never be confused with "the gate was never called". A case that scanned nothing fails.
#include "testutil.h"

#include "cfg.h"
#include "config.h"
#include "config_var.h"
#include "config_vars.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <stdlib.h>

#include <string>
#include <vector>

using psx::config::Layer;

// The three environment inputs that define a byte-compare run, plus the knob under test. Every case
// starts from all four unset, so no case can inherit another's configuration.
static const char* const kAllInputs[] = {"PSXPORT_ENH", "PSXPORT_ORACLE", "PSXPORT_SBS",
                                         "PSXPORT_SBS_MODE"};

static void forget_caches(void) {
  // Both caches must forget: lucent memoises per name, and a CVar binds its environment Override
  // once. psx::config::reset_for_test() also clears the enhancement gate's warn-once registers, so a
  // case that re-configures the environment gets the warning again rather than measuring the
  // previous case's silence.
  lucent::config::reset_cache();
  psx::config::reset_for_test();
}

static void clear_all(void) {
  for (const char* n : kAllInputs) unsetenv(n);
  // A Value or Runtime layer left behind by an earlier case would outrank the environment in the
  // next one, which is exactly the bug this file is about.
  if (psx::config::CVarBase* v = psx::config::find("PSXPORT_ENH")) {
    v->clear(Layer::Value);
    v->clear(Layer::Runtime);
  }
  forget_caches();
}

static void set_env(const char* name, const char* value) {
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
  forget_caches();
}

// The pre-migration parse, transcribed from the cfg_enh body being replaced. It is the ORACLE for
// case 4: a migrated knob must resolve IDENTICALLY to what the old code returned, and the reference
// for that is the old code, not a remembered constant.
//
// ONE DIVERGENCE, and it is asserted separately in case 4b rather than smuggled in here: the old body
// returned 1 for a call with an EMPTY name whenever PSXPORT_ENH=all, because `all` short-circuited
// before it looked at the name at all. `kNamelessIsRefused` is that difference, spelled out.
static const bool kNamelessIsRefused = true;
static int pre_migration_enh(const std::string& e, const char* name) {
  if (e.empty()) return 0;
  if (kNamelessIsRefused && (!name || !*name)) return 0;
  if (e == "all") return 1;
  size_t start = 0;
  while (start <= e.size()) {
    const size_t sep = e.find_first_of(",: ", start);
    const size_t end = (sep == std::string::npos) ? e.size() : sep;
    if (end > start && e.substr(start, end - start) == name) return 1;
    if (sep == std::string::npos) break;
    start = sep + 1;
  }
  return 0;
}

// ── 1. THE LADDER: PSXPORT_ENH is a declared CVar at all ─────────────────────────────────────────
static void test_enh_is_a_declared_cvar(void) {
  clear_all();
  psx::config::CVarBase* v = psx::config::find("PSXPORT_ENH");
  fprintf(stderr, "  find(\"PSXPORT_ENH\") -> %s\n", v ? "declared" : "NOT DECLARED (off the ladder)");
  CHECK(v != nullptr);
  if (!v) return;
  fprintf(stderr, "  kind=%s persistable=%d external=%d\n", psx::config::kind_name(v->kind()),
          (int)v->persistable(), (int)v->external());
  CHECK(v->kind() == psx::config::Kind::Text);
  CHECK(v->persistable());        // an enhancement selection is a user preference — the Value layer's class
  CHECK(!v->external());          // this registry resolves it; nothing else does

  // ...and it must show up in the environment audit as DECLARED, not UNKNOWN. A knob "on the ladder"
  // that the audit still calls unknown is not on the ladder.
  set_env("PSXPORT_ENH", "faster-transitions");
  const psx::config::EnvAudit a = psx::config::audit_environment();
  int declared = 0, unknown = 0;
  for (const std::string& s : a.declared) declared += (s == "PSXPORT_ENH");
  for (const std::string& s : a.unknown) unknown += (s == "PSXPORT_ENH");
  fprintf(stderr, "  audit: %zu PSXPORT_* set -> %zu declared, %zu legacy, %zu UNKNOWN; "
                  "PSXPORT_ENH declared=%d unknown=%d\n",
          a.set_in_env.size(), a.declared.size(), a.legacy.size(), a.unknown.size(), declared, unknown);
  CHECK(a.set_in_env.size() >= 1);   // the denominator: the audit actually looked at something
  CHECK_EQ(declared, 1);
  CHECK_EQ(unknown, 0);
  clear_all();
}

// ── 2. THE VALUE LAYER: the settings file can select an enhancement ──────────────────────────────
static void test_value_layer_reaches_the_gate(void) {
  clear_all();
  const bool ok = psx::config::set_from_settings_file("PSXPORT_ENH", "faster-transitions");
  const int hit = cfg_enh("faster-transitions");
  const int miss = cfg_enh("expanded-load-range");
  fprintf(stderr, "  settings-file Value=\"faster-transitions\" accepted=%d -> asked-for=%d other=%d\n",
          (int)ok, hit, miss);
  CHECK(ok);
  CHECK_EQ(hit, 1);
  CHECK_EQ(miss, 0);
  clear_all();
}

// ── 3. THE RUNTIME LAYER: the REPL outranks the environment ──────────────────────────────────────
static void test_runtime_layer_outranks_the_environment(void) {
  clear_all();
  set_env("PSXPORT_ENH", "from-env");
  CHECK_EQ(cfg_enh("from-env"), 1);
  CHECK_EQ(cfg_enh("from-repl"), 0);

  const bool ok = psx::config::set_runtime("PSXPORT_ENH", "from-repl");
  const int env_now = cfg_enh("from-env");
  const int repl_now = cfg_enh("from-repl");
  fprintf(stderr, "  env=\"from-env\" then REPL=\"from-repl\" accepted=%d -> env-name=%d repl-name=%d\n",
          (int)ok, env_now, repl_now);
  CHECK(ok);
  CHECK_EQ(repl_now, 1);
  CHECK_EQ(env_now, 0);      // the Runtime layer REPLACES the selection, it does not union with it

  // ...and clearing the Runtime layer must fall back to the environment rather than to nothing.
  CHECK(psx::config::clear_runtime("PSXPORT_ENH"));
  fprintf(stderr, "  after clear_runtime -> env-name=%d repl-name=%d\n", cfg_enh("from-env"),
          cfg_enh("from-repl"));
  CHECK_EQ(cfg_enh("from-env"), 1);
  CHECK_EQ(cfg_enh("from-repl"), 0);
  clear_all();
}

// ── 4. COMPATIBILITY: the migrated knob parses exactly as the old code did ───────────────────────
static void test_parse_matches_the_pre_migration_implementation(void) {
  clear_all();
  static const char* const values[] = {nullptr, "", "all", "faster-transitions",
                                       "faster-transitions,expanded-load-range",
                                       "faster-transitions: expanded-load-range",
                                       "faster-transitions,,expanded-load-range",
                                       " faster-transitions", "faster-transitionsX"};
  static const char* const names[] = {"faster-transitions", "expanded-load-range", "all", ""};
  int compared = 0;
  for (const char* v : values) {
    set_env("PSXPORT_ENH", v);
    for (const char* n : names) {
      const int want = pre_migration_enh(v ? v : "", n);
      const int got = cfg_enh(n);
      if (got != want)
        fprintf(stderr, "  MISMATCH PSXPORT_ENH=%-42s cfg_enh(\"%s\") = %d, pre-migration = %d\n",
                v ? (*v ? v : "\"\"") : "<unset>", n, got, want);
      CHECK_EQ(got, want);
      ++compared;
    }
  }
  fprintf(stderr, "  compared %d (value, name) pair(s) against the pre-migration parse\n", compared);
  CHECK_EQ(compared, 36);
  clear_all();
}

// ── 4b. THE ONE DELIBERATE DIVERGENCE, asserted rather than hidden ───────────────────────────────
// Under PSXPORT_ENH=all the old code answered YES to a call with an empty name. That made an empty or
// unexpanded name at a call site read as a working enabled enhancement. The migrated gate refuses it
// and SAYS SO — and "says so" is the half that has to be asserted, because a silent refusal is the
// same class of bug in the other direction.
static void test_a_nameless_enhancement_is_refused_loudly(void) {
  clear_all();
  set_env("PSXPORT_ENH", "all");

  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) { lines.emplace_back(line); });
  const int empty_name = cfg_enh("");
  const int null_name = cfg_enh(nullptr);
  const int real_name = cfg_enh("faster-transitions");   // `all` still means all, for a REAL name
  lucent::set_sink(nullptr);

  int refusals = 0;
  for (const std::string& l : lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
    refusals += l.find("EMPTY name") != std::string::npos;
  }
  fprintf(stderr, "  PSXPORT_ENH=all -> cfg_enh(\"\")=%d cfg_enh(NULL)=%d cfg_enh(\"faster-transitions\")=%d; "
                  "%zu captured line(s), %d refusal notice(s)\n",
          empty_name, null_name, real_name, lines.size(), refusals);
  CHECK_EQ(empty_name, 0);
  CHECK_EQ(null_name, 0);
  CHECK_EQ(real_name, 1);
  CHECK_EQ(refusals, 1);   // once, not per call — and NOT zero, which is the silent-swallow failure
  clear_all();
}

// ── 5. THE SUPPRESSION, on BOTH classes and on EACH of the three inputs separately ───────────────
// The old seeded static made this case impossible to write: one process could only ever observe one
// configuration. That is not a testing inconvenience, it is the defect — a REPL that turns ORACLE on
// mid-run could not suppress anything either.
static void test_each_compare_run_input_suppresses_and_a_plain_run_does_not(void) {
  clear_all();
  struct Case { const char* name; const char* value; int want; };
  static const Case cases[] = {
      {nullptr,             nullptr,  1},   // a plain run: the enhancement is honoured
      {"PSXPORT_ORACLE",    "1",      0},
      {"PSXPORT_SBS",       "1",      0},
      {"PSXPORT_SBS_MODE",  "panes",  0},
      {"PSXPORT_ORACLE",    "0",      1},   // present-but-off is not a compare run
      {"PSXPORT_SBS_MODE",  "",       1},   // empty is not an SBS mode
  };
  // DERIVED from the results, not written down beside the table: with the counts as literal 3s, adding
  // a suppressing case reported "3 suppressing, 3 honouring" over 7 cases and the printed denominator
  // stopped describing the run. A summary line that cannot disagree with what happened is decoration.
  int checked = 0, suppressed = 0, honoured = 0, want_suppressed = 0, want_honoured = 0;
  for (const Case& c : cases) {
    clear_all();
    set_env("PSXPORT_ENH", "all");
    if (c.name) set_env(c.name, c.value);
    const int got = cfg_enh("faster-transitions");
    fprintf(stderr, "  PSXPORT_ENH=all %-16s=%-6s -> cfg_enh = %d (want %d)\n", c.name ? c.name : "(nothing else)",
            c.value ? (*c.value ? c.value : "\"\"") : "-", got, c.want);
    CHECK_EQ(got, c.want);
    (got ? honoured : suppressed)++;
    (c.want ? want_honoured : want_suppressed)++;
    ++checked;
  }
  fprintf(stderr, "  exercised %d configuration(s) in ONE process: %d suppressing, %d honouring "
                  "(the table wants %d / %d)\n",
          checked, suppressed, honoured, want_suppressed, want_honoured);
  CHECK_EQ(checked, (int)(sizeof(cases) / sizeof(cases[0])));
  // Both classes were actually reached. A run that suppressed everything, or honoured everything, must
  // not be able to report a pass here.
  CHECK_EQ(suppressed, want_suppressed);
  CHECK_EQ(honoured, want_honoured);
  CHECK(suppressed > 0);
  CHECK(honoured > 0);
  clear_all();
}

// ── 5b. THE ORACLE MOVES MID-PROCESS, WITH NOTHING RESET ─────────────────────────────────────────
// Every other case in this file calls clear_all()/forget_caches() between configurations, so NO case
// observes the oracle moving mid-run — which is why "cv_oracle is re-read on every call so the REPL can
// move it" survived as a comment with nothing gating it. Proven by sabotage: moving cv_oracle.get() into
// compare_run()'s one-time SBS-binding block left this whole file green.
//
// This case is the missing one. The lever is set_runtime() — the REPL's own entry point, and the only
// one that can move the oracle after a CVar has bound its environment Override — and NOTHING is reset
// between the three reads, so a cached verdict cannot pass.
static void test_the_oracle_moves_mid_process_with_no_caches_cleared(void) {
  clear_all();
  set_env("PSXPORT_ENH", "faster-transitions");

  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view l) { lines.emplace_back(l); });
  const int before = cfg_enh("faster-transitions");          // plain run: honoured, binds the caches
  const bool moved_on = psx::config::set_runtime("PSXPORT_ORACLE", "1");
  const int during = cfg_enh("faster-transitions");          // NO forget_caches() — this is the point
  const bool moved_off = psx::config::set_runtime("PSXPORT_ORACLE", "0");
  const int after = cfg_enh("faster-transitions");           // and back, so a one-way latch fails too
  lucent::set_sink(nullptr);

  int suppressed_notices = 0;
  for (const std::string& l : lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
    suppressed_notices += (l.find("SUPPRESSED") != std::string::npos &&
                           l.find("faster-transitions") != std::string::npos);
  }
  fprintf(stderr, "  one process, no caches cleared: plain=%d -> REPL oracle=1 (accepted %d) -> %d -> "
                  "REPL oracle=0 (accepted %d) -> %d; %zu line(s), %d SUPPRESSED notice(s)\n",
          before, (int)moved_on, during, (int)moved_off, after, lines.size(), suppressed_notices);
  CHECK(moved_on);
  CHECK(moved_off);
  CHECK_EQ(before, 1);
  CHECK_EQ(during, 0);   // the gate must NOTICE the move, not answer from a binding made at `before`
  CHECK_EQ(after, 1);    // ...in both directions
  CHECK_EQ(suppressed_notices, 1);
  CHECK(psx::config::clear_runtime("PSXPORT_ORACLE"));
  clear_all();
}

// ── 5c. THE TRIP-WIRE ON THE UN-MIGRATED HALF OF THE SUPPRESSION ─────────────────────────────────
// compare_run() reads PSXPORT_SBS / PSXPORT_SBS_MODE through detail::env_bool / detail::env_text and
// CACHES them once, which is correct for an env-only knob and WRONG the moment either becomes a CVar:
// a settings-file or REPL value would then be invisible to the gate — the ladder bypassed for exactly
// the two inputs that decide whether a byte-compare is protected — and the one-time cache would freeze
// a value that can now move. docs/config-migration.md Group 4a lists both as remaining work, so this is
// scheduled work, not a hypothetical.
//
// A doc note cannot fail. This can: it goes RED on the commit that declares either name as a CVar, in
// the same repo, next to the code that has to change with it.
static void test_migrating_the_sbs_knobs_must_update_compare_run(void) {
  clear_all();
  static const char* const kSbs[] = {"PSXPORT_SBS", "PSXPORT_SBS_MODE"};
  int declared = 0;
  for (const char* n : kSbs) {
    const bool is_cvar = psx::config::find(n) != nullptr;
    fprintf(stderr, "  %s -> %s\n", n,
            is_cvar ? "DECLARED as a CVar — compare_run() must now READ THE CVAR, not detail::env_*, "
                      "and must drop its one-time cache (a CVar can move mid-run)"
                    : "env-only, as compare_run() assumes");
    declared += (int)is_cvar;
  }
  fprintf(stderr, "  checked %d suppression input(s) for CVar declaration -> %d declared\n",
          (int)(sizeof(kSbs) / sizeof(kSbs[0])), declared);
  CHECK_EQ(declared, 0);
  clear_all();
}

// ── 6. THE WARNING IS PER KNOB, ONCE, AND NAMES THE KNOB ─────────────────────────────────────────
// A run with two enhancements asked for must NAME BOTH. The old single seeded static named the raw
// PSXPORT_ENH string once; the second enhancement read as never having been asked for.
static void test_suppression_warns_once_per_knob_naming_each(void) {
  clear_all();
  set_env("PSXPORT_ENH", "faster-transitions,expanded-load-range");
  set_env("PSXPORT_ORACLE", "1");

  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) { lines.emplace_back(line); });
  // Twice each, interleaved: the register must be keyed on the knob, not on call order.
  for (int i = 0; i < 2; ++i) {
    (void)cfg_enh("faster-transitions");
    (void)cfg_enh("expanded-load-range");
  }
  lucent::set_sink(nullptr);

  int suppressed_lines = 0, named_a = 0, named_b = 0;
  for (const std::string& l : lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
    if (l.find("SUPPRESSED") == std::string::npos) continue;
    ++suppressed_lines;
    named_a += l.find("faster-transitions") != std::string::npos;
    named_b += l.find("expanded-load-range") != std::string::npos;
  }
  fprintf(stderr, "  4 gate calls over 2 knobs -> %zu captured line(s), %d SUPPRESSED, "
                  "naming faster-transitions %d time(s) / expanded-load-range %d time(s)\n",
          lines.size(), suppressed_lines, named_a, named_b);
  CHECK_EQ(suppressed_lines, 2);   // once per knob, not once per call and not once per run
  CHECK_EQ(named_a, 1);
  CHECK_EQ(named_b, 1);
  clear_all();
}

// ── 7. THE GAME-OWNED-CVAR SHAPE: a consumer must not need a second copy of the rule ─────────────
// This is the case that closes kanban #92. megamanx4 declares its three enhancements as its OWN
// BoolVars (they are per-title, so the framework cannot declare them) and needs the framework's
// suppression rule applied to them. Before psx::config::enh(), the only way to get that was to
// reproduce the three-input expression in the game repo.
static psx::config::BoolVar cv_test_knob(
    "PSXPORT_TEST_ENH_KNOB", false, "test-only: the game-declared enhancement shape (case 7)");

static void test_a_game_declared_bool_cvar_reaches_the_same_hook(void) {
  clear_all();
  unsetenv("PSXPORT_TEST_ENH_KNOB");
  forget_caches();

  // Off by default: the gate must not invent an enhancement nobody asked for.
  fprintf(stderr, "  default (unset) -> enh() = %d\n", (int)psx::config::enh(cv_test_knob));
  CHECK(!psx::config::enh(cv_test_knob));

  // Asked for, plain run: honoured, and ANNOUNCED — once, naming the knob.
  setenv("PSXPORT_TEST_ENH_KNOB", "1", 1);
  forget_caches();
  std::vector<std::string> on_lines;
  lucent::set_sink([&on_lines](lucent::Level, std::string_view l) { on_lines.emplace_back(l); });
  const bool on1 = psx::config::enh(cv_test_knob);
  const bool on2 = psx::config::enh(cv_test_knob);
  lucent::set_sink(nullptr);
  int active_named = 0;
  for (const std::string& l : on_lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
    active_named += (l.find("ENHANCEMENT ACTIVE") != std::string::npos &&
                     l.find("PSXPORT_TEST_ENH_KNOB") != std::string::npos);
  }
  fprintf(stderr, "  asked-for, plain run -> enh() = %d,%d; %zu line(s), %d ACTIVE notice(s) naming the knob\n",
          (int)on1, (int)on2, on_lines.size(), active_named);
  CHECK(on1);
  CHECK(on2);
  CHECK_EQ(active_named, 1);

  // Asked for, compare run: suppressed, and the notice names THIS knob — not "PSXPORT_ENH", which the
  // knob has nothing to do with.
  setenv("PSXPORT_SBS_MODE", "panes", 1);
  forget_caches();
  std::vector<std::string> off_lines;
  lucent::set_sink([&off_lines](lucent::Level, std::string_view l) { off_lines.emplace_back(l); });
  const bool off1 = psx::config::enh(cv_test_knob);
  const bool off2 = psx::config::enh(cv_test_knob);
  lucent::set_sink(nullptr);
  int suppressed_named = 0;
  for (const std::string& l : off_lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
    suppressed_named += (l.find("SUPPRESSED") != std::string::npos &&
                         l.find("PSXPORT_TEST_ENH_KNOB") != std::string::npos);
  }
  fprintf(stderr, "  asked-for, SBS_MODE=panes -> enh() = %d,%d; %zu line(s), %d SUPPRESSED notice(s) "
                  "naming the knob\n", (int)off1, (int)off2, off_lines.size(), suppressed_named);
  CHECK(!off1);
  CHECK(!off2);
  CHECK_EQ(suppressed_named, 1);

  unsetenv("PSXPORT_TEST_ENH_KNOB");
  clear_all();
}

// ── 8. THE ANTI-DIVERGENCE GATE: one definition of "what a byte-compare run IS" ───────────────────
// The reason kanban #92 is a framework bug and not a tidiness complaint. The literal expression below
// is the one megamanx4/game/core/enhancements.cpp keeps a copy of; compare_run_from() must agree with
// it on every one of the eight input combinations, or a game and the framework disagree about whether a
// run is a byte-compare — and the failure mode of that disagreement is a CONTAMINATED compare that
// still looks clean.
static void test_compare_run_is_the_single_definition_of_a_byte_compare_run(void) {
  int compared = 0, suppressing = 0;
  for (int bits = 0; bits < 8; ++bits) {
    const bool oracle = (bits & 1) != 0, sbs = (bits & 2) != 0, has_mode = (bits & 4) != 0;
    const char* mode = has_mode ? "panes" : "";
    // The expression, written out exactly as a call site would have to write it.
    const bool by_hand = oracle || sbs || (mode && *mode);
    std::string why;
    const bool by_framework = psx::config::compare_run_from(oracle, sbs, mode, &why);
    fprintf(stderr, "  oracle=%d sbs=%d sbs_mode=%-6s -> framework=%d hand-written=%d why=\"%s\"\n",
            (int)oracle, (int)sbs, has_mode ? "panes" : "\"\"", (int)by_framework, (int)by_hand,
            why.c_str());
    CHECK_EQ((int)by_framework, (int)by_hand);
    // A verdict of "yes" that cannot say WHICH input made it yes is a diagnostic that explains nothing.
    if (by_framework) {
      CHECK(why.find("PSXPORT_") != std::string::npos);
      ++suppressing;
    } else {
      CHECK(why.empty());
    }
    ++compared;
  }
  fprintf(stderr, "  compared %d of 8 input combination(s); %d suppressing, %d honouring\n", compared,
          suppressing, compared - suppressing);
  CHECK_EQ(compared, 8);
  CHECK_EQ(suppressing, 7);   // exactly one combination — nothing set — is not a compare run

  // ...and the environment-reading form must agree with the pure one on the configuration this process
  // is actually in, or the pure function is gating a decision nothing uses.
  clear_all();
  std::string why;
  const bool plain = psx::config::compare_run(&why);
  fprintf(stderr, "  with all three inputs unset: compare_run() = %d why=\"%s\"\n", (int)plain,
          why.c_str());
  CHECK(!plain);
  set_env("PSXPORT_ORACLE", "1");
  const bool oracle_run = psx::config::compare_run(&why);
  fprintf(stderr, "  with PSXPORT_ORACLE=1: compare_run() = %d why=\"%s\"\n", (int)oracle_run,
          why.c_str());
  CHECK(oracle_run);
  CHECK(why.find("PSXPORT_ORACLE") != std::string::npos);
  clear_all();
}

// ── 9. THE SHIPPING SELF-TEST ────────────────────────────────────────────────────────────────────
// psx::config::selftest() proves IN THE SHIPPING LIBRARY that the audit and the enhancement gate each
// produce their positive AND their negative. Running it from here is what stops it being a self-test
// nobody runs.
static void test_the_shipping_selftest_passes(void) {
  clear_all();
  const bool ok = psx::config::selftest();
  fprintf(stderr, "  psx::config::selftest() -> %s\n",
          ok ? "true (audit AND enhancement gate produced both classes)" : "FALSE");
  CHECK(ok);
  clear_all();
}

int main(void) {
  RUN(enh_is_a_declared_cvar);
  RUN(value_layer_reaches_the_gate);
  RUN(runtime_layer_outranks_the_environment);
  RUN(parse_matches_the_pre_migration_implementation);
  RUN(a_nameless_enhancement_is_refused_loudly);
  RUN(each_compare_run_input_suppresses_and_a_plain_run_does_not);
  RUN(the_oracle_moves_mid_process_with_no_caches_cleared);
  RUN(migrating_the_sbs_knobs_must_update_compare_run);
  RUN(suppression_warns_once_per_knob_naming_each);
  RUN(a_game_declared_bool_cvar_reaches_the_same_hook);
  RUN(compare_run_is_the_single_definition_of_a_byte_compare_run);
  RUN(the_shipping_selftest_passes);
  return pt_summary();
}
