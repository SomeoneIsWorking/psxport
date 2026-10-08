#ifndef PSXPORT_CONFIG_H
#define PSXPORT_CONFIG_H
// config.h — the CVar REGISTRY: lookup, mutation from the outside world, and introspection.
//
// Include config_var.h if you only need to READ a knob. This header is the mutation and reporting
// surface, and it is deliberately small so that "what can change a knob" stays answerable.
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

// Prove the environment audit reports both a known and an unknown variable.
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
