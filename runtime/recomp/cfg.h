#ifndef PSXPORT_CFG_H
#define PSXPORT_CFG_H
// Centralized PSXPORT_* configuration — replaces the scattered getenv()+`static int=-1` boilerplate
// and, crucially, collapses the dozens of one-off diagnostic toggles into ONE debug-channel variable.
//
//   cfg_on (name)      - boolean config/feature flag: env present and not "0" -> 1. Cached.
//   cfg_int(name,def)  - integer-valued flag (e.g. a frame number / scale). Cached.
//   cfg_str(name)      - string-valued flag (paths, "x,y" coords). NULL if unset. Cached.
//   cfg_dbg(chan)      - is debug CHANNEL `chan` enabled? Driven by the SINGLE env var
//                        PSXPORT_DEBUG=chanA,chanB,...  (or PSXPORT_DEBUG=all). Replaces ~60 *_DBG/
//                        *DUMP/*LOG/*WATCH flags so diagnostics are one variable, not one-flag-each.
//
// All lookups read the environment once and cache. In genuinely hot paths (per-prim / per-GTE-op /
// per-store) still keep a local `static int x=-1; if(x<0) x=cfg_*(...)` so there is no per-call scan.
#ifdef __cplusplus
extern "C" {
#endif
int cfg_on(const char *name);
int cfg_int(const char *name, int def);
const char *cfg_str(const char *name);
// PSXPORT_ORACLE — the pure PSX reference mode (recomp gameplay + UNENHANCED PSX render). When on, NO
// native render enhancement may touch the picture: no fps60, no widescreen, no native depth / obj_depth
// compositing, no RenderObserver tagging. Cached. Every enhancement gate that could contaminate the PSX
// render consults this (or is forced off at boot). See docs/config.md + native_boot.cpp.
int oracle_mode(void);
// Is PC ENHANCEMENT `name` enabled? Driven by PSXPORT_ENH=<name,name|all>. Enhancements are the
// sanctioned third behavior class: deliberate, meaningful guest-state changes (expanded object
// load/unload, faster fades/transitions). Force-suppressed (returns 0, a notice PER KNOB) whenever
// PSXPORT_ORACLE or either SBS form is active, so oracle byte-compares can never be clobbered by a
// stray .env. Register every name in docs/config.md.
//
// THIS IS A C-CALLABLE FORWARDER, not the mechanism. PSXPORT_ENH is a CVar (config_vars.h: cv_enh) and
// the gate is psx::config::enh_named() -> enh_gate(), whose suppression input psx::config::compare_run()
// is the ONE definition of "this run is a byte-compare run". New C++ calls those directly; a game whose
// enhancements are its OWN CVars calls psx::config::enh(cv_my_knob) and gets the identical rule rather
// than a second copy of it. An EMPTY name is REFUSED with an error (the pre-migration body answered YES
// to it under PSXPORT_ENH=all) — see docs/config.md "PC enhancements".
int cfg_enh(const char *name);
int cfg_dbg(const char *chan);       // is debug CHANNEL `chan` enabled? (set via REPL `debug`)
void cfg_dbg_set(const char *chans); // REPL `debug <chans|all>`: enable diagnostic channels
// Generation counter for the enabled-channel SET, bumped whenever it changes. cfg_dbg() is NOT a cheap
// flag test — it string-compares the channel name against the enabled set — so a call site on a
// genuinely hot path caches cfg_dbg()'s answer and re-checks only when this counter moves.
unsigned cfg_dbg_generation(void);
// ...and the INLINE form, because the caching call sites this counter exists for are on the hottest
// path in the substrate and were paying an out-of-line call to read it. OtAttr::trackStore runs on
// EVERY guest store and reached the counter through two nested calls (cfg_dbg_generation ->
// bootstrap_once) just to decide it was not logging; the pair measured 3.42% + 2.54% of total CPU.
// The counter is zero until bootstrap_once() has run, so a zero here means "take the slow path once".
extern unsigned cfg_dbg_gen_v;
static inline unsigned cfg_dbg_generation_fast(void) {
  return cfg_dbg_gen_v ? cfg_dbg_gen_v : cfg_dbg_generation();
}
// THE diagnostic print primitive: no-op unless cfg_dbg(chan); emits "[chan] <msg>\n" (newline added
// if fmt lacks one) to stderr, or to PSXPORT_LOG_FILE=<path> when set. Replaces every
// `if (cfg_dbg(chan)) fprintf(stderr, ...)` two-step — new diagnostics use THIS, one line per site.
// Keep a raw cfg_dbg() guard only around genuinely non-print work (expensive dump loops, setVerbose).
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void cfg_logf(const char *chan, const char *fmt, ...);
// ALWAYS-ON levels (not channel-gated) — the messages a normal run is meant to print. Same sink and
// same "[chan] " prefixing as cfg_logf, so PSXPORT_LOG_FILE captures them too; warn/error suffix the
// tag ("[cd:warn]" / "[cd:error]") to stay greppable. Use these instead of raw fprintf(stderr, ...):
//   cfg_logi - normal progress/status      cfg_logw - recoverable oddity      cfg_loge - hard failure
// A message that should only appear when someone asks for it is a CHANNEL -> cfg_logf, not cfg_logi.
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void cfg_logi(const char *chan, const char *fmt, ...);
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void cfg_logw(const char *chan, const char *fmt, ...);
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void cfg_loge(const char *chan, const char *fmt, ...);
void cfg_dump(void); // log every active PSXPORT_* var (once); for boot-time visibility

// --- Line accumulator: for DUMPS built piece-by-piece (hex rows, byte tables, column runs) --------
// The logger emits one whole line per call, so a loop that appends `%02X` per byte cannot call it
// directly. Accumulate here, then flush once. Truncation-safe (marks "…" and stops).
//   CfgLine ln; cfg_line_reset(&ln);
//   cfg_line_addf(&ln, "  A @0x%08X:", addr);
//   for (…) cfg_line_addf(&ln, " %02X", b[i]);
//   cfg_line_flush(&ln, "sbs");            // -> cfg_logi("sbs", …) and resets
typedef struct {
  char buf[4096];
  unsigned used;
} CfgLine;
void cfg_line_reset(CfgLine *l);
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void cfg_line_addf(CfgLine *l, const char *fmt, ...);
void cfg_line_flush(CfgLine *l, const char *chan); // emit at info level, then reset
#ifdef __cplusplus
}
#endif
#endif
