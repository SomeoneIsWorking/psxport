// engine_override_thunk.cpp — the ONE oracle gate for engine/game natives installed into the
// recompiler's process-global override tables (g_override[] / g_ov_*_override[]).
//
// PROBLEM. g_override[] and the g_ov_<mod>_override[] arrays are single PROCESS-GLOBAL tables
// shared by every Core, including SBS core B (the pure-substrate oracle). The generated wrapper
//   void func_X(Core* c){ c->pc=X; if(g_override[i]){g_override[i](c);return;} gen_func_X(c); }
// consults that shared table on BOTH cores. So any engine/game native installed there also runs on
// the oracle unless the installed function defers to the real gen_func_* body when
// c->game->psx_fallback is set. Doing that gate inside every cluster's trampoline is error-prone:
// clusters that forget it (perobj_billboard / overlay_gt3gt4 / overlay_ground_gt3gt4 /
// quad_rtpt_submit, all landed 2026-07-08) silently broke the oracle — SBS then compared
// native-vs-native and reported a FALSE 0-div, masking any real divergence in those clusters.
//
// FIX. Engine/game installs go through engine_set_override_main / _a00 here, which stores
// {native, gen} keyed by guest address and installs a SINGLE shared thunk into the table. The thunk
// reads c->pc (the wrapper stamps it immediately before invoking the override, so it is exactly the
// guest address at entry) and runs gen on the oracle, native everywhere else. The gate now lives in
// ONE place and can't be forgotten per-cluster.
//
// The oracle is thereby guaranteed to run ONLY the recompiled body for these engine/game addresses
// — the state PSXPORT_GATE=1 was in ~7 days ago (commit 95157d3, before the 2026-07-08 render
// clusters landed). PlatformHle (async->sync + HLE BIOS) and the native scheduler primitives install
// via the RAW shard_set_override / ov_*_set_override directly and are the ONLY overrides that
// legitimately fire on the oracle (they must, or the no-IRQ runtime hangs).
//
// NOTE: rec_dispatch's EngineOverrides path is ALREADY oracle-gated (overlay_router.cpp gates on
// !psx_fallback before consulting engine_overrides.run), so rec_dispatch-routed natives need no
// change. This file is for the DIRECT-substrate-call path that bypasses rec_dispatch.
#include "core.h"
#include "game.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

typedef void (*OverrideFn)(Core*);
extern void shard_set_override(uint32_t, OverrideFn);    // generated/shard_disp.c
extern void ov_a00_set_override(uint32_t, OverrideFn);   // generated/ov_a00_disp.c

namespace {
struct Entry { uint32_t addr; OverrideFn native; OverrideFn gen; };
constexpr int kCap = 256;
Entry g_tab[kCap];
int g_n = 0;

// Per-slot hit counters (core A native vs core B oracle) for divergence attribution. Lightweight
// (uint64), dumped at atexit — the thunk is only for a handful of render addresses, so negligible
// overhead. Under SBS, native==core A, oracle==core B; a native/oracle count MISMATCH on a slot
// means the two cores reach that address a different number of times (control-flow divergence),
// while equal counts + divergent RAM means a logic (output) divergence in the native body.
uint64_t g_nativeHits[kCap];
uint64_t g_oracleHits[kCap];

void dump_atexit() {
  // Only meaningful under SBS (oracle=coreB). Skip the noise on standalone runs where oracle hits
  // are all zero.
  bool anyOracle = false;
  for (int i = 0; i < g_n; i++) if (g_oracleHits[i]) { anyOracle = true; break; }
  if (!anyOracle) return;
  fprintf(stderr, "[engine_override_thunk] per-address hit counts (native=coreA / oracle=coreB):\n");
  for (int i = 0; i < g_n; i++) {
    fprintf(stderr, "  0x%08X : native=%llu  oracle=%llu%s\n",
            g_tab[i].addr, (unsigned long long)g_nativeHits[i], (unsigned long long)g_oracleHits[i],
            g_nativeHits[i] != g_oracleHits[i] ? "   <-- COUNT MISMATCH (control-flow divergence)" : "");
  }
}

int lookup_slot(uint32_t addr) {
  for (int i = 0; i < g_n; i++) if (g_tab[i].addr == addr) return i;
  return -1;
}

void put(uint32_t addr, OverrideFn native, OverrideFn gen) {
  int slot = lookup_slot(addr);
  if (slot < 0) {
    if (g_n >= kCap) { fprintf(stderr, "[engine_override_thunk] table full (kCap=%d)\n", kCap); abort(); }
    slot = g_n++;
    static bool s_atexit = false;
    if (!s_atexit) { s_atexit = true; atexit(dump_atexit); }
  }
  g_tab[slot] = {addr, native, gen};
}
}

// The ONE shared thunk the generated wrapper invokes. c->pc = guest address at entry.
//
// PSXPORT_THUNK_FORCE_GEN=0xADDR[,0xADDR2,...] — force the listed addresses to run their GEN body
// even on core A (i.e. treat them as oracle). Bisection knob: when a native cluster diverges,
// force-gen it to confirm it's the culprit (core A then matches core B). Parsed once, lazily.
void engine_override_thunk(Core* c) {
  static const char* s_force = (const char*)1;   // sentinel: parse on first call
  if (s_force == (const char*)1) {
    s_force = getenv("PSXPORT_THUNK_FORCE_GEN");
    if (s_force) fprintf(stderr, "[engine_override_thunk] FORCE_GEN active: %s\n", s_force);
  }
  const uint32_t addr = c->pc;
  const int slot = lookup_slot(addr);
  if (slot < 0) {
    fprintf(stderr, "[engine_override_thunk] no entry for pc=%08X (table has %d)\n", addr, g_n);
    abort();
  }
  // verify.inSubstrateLeg: MV_CHECK's substrate replay leg must behave exactly like SBS core B.
  // overlay_router.cpp already gates the EngineOverrides path on it; without the same gate HERE, a
  // strictCheck on a thunk-wired address would run the NATIVE body in its "substrate" leg and
  // compare native-vs-native -- a fake pass (found at the 2026-07-10 sequencer wiring pass).
  bool oracle = c->game && (c->game->psx_fallback || c->game->verify.inSubstrateLeg);
  if (!oracle && s_force) {
    for (const char* p = s_force; p && *p; ) {
      uint32_t a = (uint32_t)strtoul(p, (char**)&p, 0);
      if (a == addr) { oracle = true; break; }
      while (*p == ',' || *p == ' ') p++;
    }
  }
  if (oracle) { g_oracleHits[slot]++; g_tab[slot].gen(c); }
  else        { g_nativeHits[slot]++; g_tab[slot].native(c); }
}

// Installers — one per recompiler module whose raw set_override we must reach. The shared thunk is
// what actually lands in the table; {native, gen} is recorded here for the thunk to dispatch on.
void engine_set_override_main(uint32_t addr, OverrideFn native, OverrideFn gen) {
  put(addr, native, gen);
  shard_set_override(addr, engine_override_thunk);
}
void engine_set_override_a00(uint32_t addr, OverrideFn native, OverrideFn gen) {
  put(addr, native, gen);
  ov_a00_set_override(addr, engine_override_thunk);
}
