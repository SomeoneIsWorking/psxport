#ifndef PSXPORT_CONFIG_VARS_H
#define PSXPORT_CONFIG_VARS_H
// config_vars.h — THE INVENTORY. Every migrated knob is listed here and defined in config.cpp.
// Ordinary values expose a CVar; policy values expose only their typed API so consumers cannot
// duplicate parsing or couple themselves to the backing storage. This is the file you grep to answer
// "what knobs exist".
//
// A knob that is NOT here still works: cfg_on / cfg_int / cfg_str fall through to the environment
// exactly as they always did (runtime/psx/cfg.cpp), and the registry records the read so the
// environment audit can tell it apart from a typo. Migration order and the remaining list are in
// docs/config.md.
//
// TO MIGRATE A KNOB: declare it here, define it in config.cpp next to its neighbours, and change the
// call site from cfg_*("PSXPORT_X") to cv_x.get(). Then check the migrated value against the
// pre-migration one in tests/test_config_cvar.cpp — the compatibility gate is the point of the
// exercise, not a formality.
#include "config_var.h"
#include "diagnostic_run.h"
#include "fallback_policy.h"
#include "render_mode.h" // RenderPath — the type cv_render_path resolves to

namespace psx::config {

// PSXPORT_NOAUDIO — open no audio device. Half of what "headless" means (the other half, no window,
// is the consuming game's own launch decision).
extern BoolVar cv_noaudio;

// PSXPORT_NOPACE — do not sleep to hold the present rate. An agent's headless progress run wants
// frames as fast as they come; a normal run wants 60 Hz.
extern BoolVar cv_nopace;
extern BoolVar cv_producers; // arm the producer census (ot_attr.h) — see config.cpp for the cost

// PSXPORT_REPL — the interactive REPL on stdin, serviced by the product frame loop.
extern BoolVar cv_repl;

// ── watchdog ────────────────────────────────────────────────────────────────────────────────────
// PSXPORT_WATCHDOG — frame-progress timeout in seconds. Default 3, ON even when unset, so a hang
// self-aborts with a backtrace instead of wedging. 0 disables it.
extern IntVar cv_watchdog;
extern IntVar cv_spin_ticks; // guest instructions per spin-detector sample (0 = off)
extern IntVar cv_spin_runs;  // consecutive starved in-region samples that declare a spin
// PSXPORT_WATCHDOG_BOOT — the larger grace for the FIRST present, which legitimately blocks while
// the driver compiles every pipeline. -1 (the default) means "derive": max(PSXPORT_WATCHDOG, 45).
// An explicit 0 still means 0, which is why the sentinel is -1 and not 0.
extern IntVar cv_watchdog_boot;

// ── assets and settings ─────────────────────────────────────────────────────────────────────────
// PSXPORT_ASSET_DIR — the directory CONTAINING `assets/`, for a consumer whose cwd is its own repo
// root. Empty = cwd-relative `assets/rml/...`.
extern TextVar cv_asset_dir;

// PSXPORT_SETTINGS — path to the settings file. NOT persistable, and that is not a detail: this knob
// SELECTS the file the Value layer is read from and written to, so a Value-layer copy of it inside
// that file could disagree with the file it is in.
extern TextVar cv_settings_path;

// ── enhancements ────────────────────────────────────────────────────────────────────────────────
// PSXPORT_FPS60 — the interpolated-60fps tier. Documented in docs/config.md since it was written and
// READ BY NOTHING until this migration: a run with it set was indistinguishable from a run without.
// It is the reason the environment audit exists. Its Value layer is the `fps60=` line in
// psxport_settings.ini, written by the F1 overlay (runtime/psx/mods.cpp).
extern BoolVar cv_fps60;

// PSXPORT_ENH=<name,name|all> — the sanctioned pc_enh class: deliberate, MEANINGFUL guest-state
// changes on top of the faithful engine. Read it through enh_named() / enh(), never by parsing the
// text at a call site. Persistable: an enhancement selection is a user preference, the class the
// Value layer exists for (cf. cv_fps60).
//
// MIGRATED 2026-08-12 (Tomba2Engine kanban #92). It used to be read by cfg_enh() straight out of
// lucent::config into a function-local SEEDED STATIC, which cost three things: no Value layer (the
// settings file could not select an enhancement), no Runtime layer (nor could the REPL), and — because
// the static was seeded on the first call and never re-read — no way for anything to change the answer
// later in the process, including the suppression below. Gate: tests/test_config_enh.cpp.
extern TextVar cv_enh;

// PSXPORT_DIAGNOSTIC_RUN — product | compare-candidate | compare-reference. This is a diagnostic
// role for the same shipping dynarec/native runtime, never an interpreter/backend selector. The
// backing TextVar is intentionally private: consumers use this typed API and ScopedDiagnosticRun.
// The API is declared once in diagnostic_run.h, included above.

// PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT — maximum automatic interpreter-fallback blocks admitted
// during one bounded executor call. The compiled default is one: the known difficult-block escape
// remains available, while a second block is a typed fault instead of an interpreter-dominated run.
psx::cpu::FallbackPolicy lightrec_fallback_policy();

// Resolve one enhancement and announce an active selection once per key.
bool enh_gate(const char *key, bool asked);
// The two ways to reach it. `enh(v)` is for a game that declares its enhancements as its OWN CVars
// (the shape megamanx4 uses); `enh_named(name)` is for a name selected through PSXPORT_ENH, and is
// what cfg_enh() forwards to.
bool enh(const CVar<bool> &v);
bool enh_named(const char *name);

// PSXPORT_RENDER_PATH — the render path: native | gte | psx.
// Read it through render_path() below, never by parsing the text at a call site.
extern TextVar cv_render_path;
RenderPath render_path(RenderPath fallback = RenderPath::Native);

// ── the graphics-producer DB ────────────────────────────────────────────────────────────────────
// PSXPORT_PRODUCERS_DIR — where the per-run producer-census JSONL and the accumulated claim set are
// written. Default is the game repo's gitignored scratch/ tree; NEVER /tmp (small tmpfs here).
extern TextVar cv_producers_dir;

// PSXPORT_PRODUCERS_DB — the claim set the guest leg resolves against: either the flat claims file or a
// run JSONL. Empty = <PRODUCERS_DIR>/claims.txt, the file the previous run appended.
//
// WHY THIS KNOB HAS TO EXIST AT ALL, since a path with a default looks like scaffolding: no single leg
// runs both halves of the comparison (pc_render never GP0-executes the guest packets; psx_render never
// runs a native producer), so the addresses native producers key are earned on ONE run and consumed by
// ANOTHER. Pointing this at a specific DB is how a harness compares against a chosen baseline instead of
// whatever the last run happened to leave behind.
extern TextVar cv_producers_db;

// ── declared for introspection only; resolved elsewhere ─────────────────────────────────────────
// lucent reads these two for itself — it is BUILT with LUCENT_CHANNEL_ENV="PSXPORT_DEBUG" and
// LUCENT_LOG_FILE_ENV="PSXPORT_LOG_FILE" (cmake/psxport.cmake), resolving both lazily on its first
// log call so there is no initialisation that can fail to run. They are declared here, marked
// `external`, purely so the environment audit does not report the two most-used knobs in the port as
// unknown, and so `report()` can show what the run was configured with. NOTHING reads their value
// from the registry and nothing may start: PSXPORT_DEBUG has 44 call sites across four repos and is
// wired through CMake into lucent. tests/test_lucent_channel_env.cpp is the gate on that.
extern TextVar cv_debug_channels;
extern TextVar cv_log_file;

// ── the audit's own calibration target ──────────────────────────────────────────────────────────
// PSXPORT_CFG_SELFTEST_DECLARED — reserved. It configures nothing and never will. It exists so
// selftest() can run the environment audit against a DECLARED name as well as an undeclared one: a
// classifier only ever exercised on one of its two classes has not been tested, it has been
// admired. Not documented as a knob in docs/config.md, because it is not one.
extern BoolVar cv_selftest_declared;

} // namespace psx::config

#endif // PSXPORT_CONFIG_VARS_H
