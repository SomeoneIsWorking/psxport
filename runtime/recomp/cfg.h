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
int         cfg_on (const char* name);
int         cfg_int(const char* name, int def);
const char* cfg_str(const char* name);
// PSXPORT_ORACLE — the pure PSX reference mode (recomp gameplay + UNENHANCED PSX render). When on, NO
// native render enhancement may touch the picture: no fps60, no widescreen, no native depth / obj_depth
// compositing, no RenderObserver tagging. Cached. Every enhancement gate that could contaminate the PSX
// render consults this (or is forced off at boot). See docs/config.md + native_boot.cpp.
int         oracle_mode(void);
// Is PC ENHANCEMENT `name` enabled? Driven by PSXPORT_ENH=<name,name|all>. Enhancements are the
// sanctioned third behavior class: deliberate, meaningful guest-state changes (expanded object
// load/unload, faster fades/transitions). Force-suppressed (returns 0, one-time notice) whenever
// PSXPORT_ORACLE or SBS is active, so oracle byte-compares can never be clobbered by a stray .env.
// Register every name in docs/config.md.
int         cfg_enh(const char* name);
int         cfg_dbg(const char* chan);            // is debug CHANNEL `chan` enabled? (set via REPL `debug`)
void        cfg_dbg_set(const char* chans);       // REPL `debug <chans|all>`: enable diagnostic channels
// THE diagnostic print primitive: no-op unless cfg_dbg(chan); emits "[chan] <msg>\n" (newline added
// if fmt lacks one) to stderr, or to PSXPORT_LOG_FILE=<path> when set. Replaces every
// `if (cfg_dbg(chan)) fprintf(stderr, ...)` two-step — new diagnostics use THIS, one line per site.
// Keep a raw cfg_dbg() guard only around genuinely non-print work (expensive dump loops, setVerbose).
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void        cfg_logf(const char* chan, const char* fmt, ...);
void        cfg_dump(void);   // log every active PSXPORT_* var (once); for boot-time visibility
#ifdef __cplusplus
}
#endif
#endif
