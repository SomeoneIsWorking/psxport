#ifndef PSXPORT_CONFIG_H
#define PSXPORT_CONFIG_H
// config.h — the CVar REGISTRY: lookup, mutation from the outside world, and introspection.
//
// Include config_var.h if you only need to READ a knob. This header is the mutation and reporting
// surface, and it is deliberately small so that "what can change a knob" stays answerable.
// (Dusklight's src/dusk/config.hpp plays the same role; see the header comment in config_var.h for
// what was taken and where this deviates.)
//
// THE PART THAT MATTERS MOST: audit_environment() / report().
//
// PSXPORT_FPS60 is documented in docs/config.md and is read by NOTHING. It has been set on real
// runs, including on both legs of an A/B, and the result looked plausible. Nothing in the port said
// a word. Every knob in this system can therefore be asked three questions — does it exist, what is
// it resolving to, and which layer did that come from — and every PSXPORT_* variable found in the
// environment is classified into one of exactly three buckets, one of which is "this did nothing".
//
// The audit reports its DENOMINATOR and its BLIND SPOT even when it finds nothing, because "0
// unknown" and "the audit never ran" must not print the same way.
#include "config_var.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace psx::config {

// --- lookup ------------------------------------------------------------------------------------
CVarBase *find(std::string_view name);                     // null if not declared
void enumerate(const std::function<void(CVarBase &)> &fn); // every declared CVar, name order
std::size_t registered_count();

// --- the legacy compatibility path ---------------------------------------------------------------
// The ~190 knobs that have NOT been migrated still resolve exactly as they always did: cfg_on /
// cfg_int / cfg_str fall through to lucent::config, i.e. to the environment. They call this on the
// way past so the registry knows the knob was READ, which is what lets the audit tell a real
// un-migrated knob apart from a typo. It is an observation, not a declaration — the distinction is
// reported, because a knob on a code path this run never entered is invisible to it.
void note_legacy_read(const char *name, Kind kind, std::string_view resolved);
std::size_t legacy_read_count();

// --- the environment audit -----------------------------------------------------------------------
struct EnvAudit {
  // "NAME=value" for every PSXPORT_* variable present in the environment. The denominator.
  std::vector<std::string> set_in_env;
  std::vector<std::string> declared; // names matching a registered CVar
  std::vector<std::string> legacy;   // names some cfg_* call has read this run
  std::vector<std::string> unknown;  // names matching NEITHER — these did nothing
};
EnvAudit audit_environment();

// Log the whole configuration state: every declared CVar with its value and resolving layer, every
// legacy-observed knob, then the audit with its denominator and its blind spot. `unknown` entries
// come out at WARN. Safe to call more than once (the boot call is one-shot via report_once()).
void report();
// The boot call. Also arms report_exit_audit() via std::atexit, so a cleanly-exiting run gets the
// audit again with nothing left unread. A run killed by a signal does not — see the comment on
// report_exit_audit in config.cpp, and use `cvars` over the debug server for a live answer.
void report_once();
void report_exit_audit();

// Proof, in the shipping library, that two instruments can each produce a positive AND a negative.
//
//  1. THE ENVIRONMENT AUDIT, run against a name that MUST come back unknown and one that MUST NOT. An
//     audit only ever checked against the class it is supposed to stay silent about is not an
//     instrument. See PSXPORT_CFG_SELFTEST_DECLARED in config_vars.h.
//  2. THE ENHANCEMENT GATE — and specifically enh_gate() / enh() / enh_named(), the functions a game
//     reaches, not the pure compare_run_from() predicate beside them. That distinction is the whole
//     point: an earlier version looped only the predicate, so deleting the ORACLE/SBS suppression from
//     enh_gate() outright still printed "0 disagreement(s)" while an PSXPORT_ORACLE run would have
//     enabled every enhancement — a contaminated byte-compare oracle, certified clean.
//
// TWO THINGS A CALLER MUST KNOW, because this is not a read-only diagnostic:
//  * It MOVES PSXPORT_ORACLE's Runtime layer (via set_runtime) and restores whatever was there. That
//    is the only lever that can change the gate's input mid-process, and driving the gate through it is
//    also what gates "cv_oracle is re-read on every call so the REPL can move it".
//  * It REFUSES — returns false, saying what it did not exercise — when the process ALREADY is a
//    byte-compare run. Reaching the honour class would mean forcing the oracle off in a run that IS
//    one. So do not call this from an oracle/SBS boot path and read the result as a verdict on the
//    configuration; it is a verdict on the code, and it wants a plain process.
// The gate's own log lines during the run are marked as the selftest's, since it drives the real gate.
bool selftest();

// --- mutation from outside -------------------------------------------------------------------------
// Returns false if the name is not a declared CVar, or the text does not parse for its Kind. Both
// are reported; neither is swallowed.
bool set_from_settings_file(std::string_view name, std::string_view text); // -> Layer::Value
bool set_runtime(std::string_view name, std::string_view text);            // -> Layer::Runtime (REPL)
bool clear_runtime(std::string_view name);

// For a knob whose VALUE lives in another subsystem (PSXPORT_DEBUG -> lucent::enable_channels):
// record at the Runtime layer what that subsystem was just told, so the dump can say what the live
// setting is and that a console command is where it came from. Records; does not apply.
void note_runtime_external(std::string_view name, std::string_view text);

// Forget every environment binding so a subsequent read re-reads the environment. For tests, which
// need setenv() to take effect. Pair it with lucent::config::reset_cache().
void reset_for_test();

} // namespace psx::config

#endif // PSXPORT_CONFIG_H
