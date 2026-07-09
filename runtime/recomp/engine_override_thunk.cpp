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

OverrideFn lookup(uint32_t addr, bool wantGen) {
  for (int i = 0; i < g_n; i++)
    if (g_tab[i].addr == addr) return wantGen ? g_tab[i].gen : g_tab[i].native;
  return nullptr;
}

void put(uint32_t addr, OverrideFn native, OverrideFn gen) {
  for (int i = 0; i < g_n; i++)
    if (g_tab[i].addr == addr) { g_tab[i] = {addr, native, gen}; return; }
  if (g_n >= kCap) { fprintf(stderr, "[engine_override_thunk] table full (kCap=%d)\n", kCap); abort(); }
  g_tab[g_n++] = {addr, native, gen};
}
}

// The ONE shared thunk the generated wrapper invokes. c->pc = guest address at entry.
void engine_override_thunk(Core* c) {
  const uint32_t addr = c->pc;
  const bool oracle = c->game && c->game->psx_fallback;
  OverrideFn fn = lookup(addr, oracle);
  if (!fn) {
    fprintf(stderr, "[engine_override_thunk] no %s entry for pc=%08X (table has %d)\n",
            oracle ? "gen" : "native", addr, g_n);
    abort();
  }
  fn(c);
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
