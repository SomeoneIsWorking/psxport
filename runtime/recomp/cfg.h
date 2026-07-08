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
int         cfg_dbg(const char* chan);            // is debug CHANNEL `chan` enabled? (set via REPL `debug`)
void        cfg_dbg_set(const char* chans);       // REPL `debug <chans|all>`: enable diagnostic channels
void        cfg_dump(void);   // log every active PSXPORT_* var (once); for boot-time visibility
#ifdef __cplusplus
}
#endif
#endif
