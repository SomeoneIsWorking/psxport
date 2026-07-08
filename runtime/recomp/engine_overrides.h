// class EngineOverrides — the GLOBAL DISPATCH override table for game/engine functions.
//
// User directive 2026-07-07: the old "only hardware-sync HLE may intercept, PSX never calls PC"
// rule is retired. A native engine class may be WIRED BY GUEST ADDRESS here (e.g. the fade engine
// at its FUN_xxxx entry), and then EVERY caller — recompiled substrate or native PC code calling
// through rec_dispatch — reaches the native method. This removes the contiguity requirement
// (own callers before callees): a leaf engine can be owned globally on its own.
//
// Contract for every registered handler:
//   - Guest ABI: read args from c->r[4..7]/stack, return via c->r[2]. Same as a substrate body.
//   - BYTE-EXACT: the handler must reproduce the substrate body's guest-RAM/scratchpad writes
//     exactly — SBS is the gate. Core B (psx_fallback) never consults this table, so it stays
//     the pure substrate reference the override is compared against.
//   - Native code SHOULD call the wired address via rec_dispatch(c, addr) rather than the class
//     method directly where uniform tracing matters; the `dispatch` debug channel logs every
//     override hit (frame, core, addr, name, ra, a0-a3) at the single choke point.
//
// KNOWN GAP (found + partly closed 2026-07-08, docs/findings/tooling.md "EngineOverrides::
// register_ is BLIND to a direct substrate call"): a registration here is invisible to a call the
// recompiler emits as a DIRECT intra-module `func_<addr>(c)` (checks the recompiler's OWN
// `g_override[]`/`g_ov_<mod>_override[]` table, never rec_dispatch). Closing that for one address
// needs a SECOND wire-up (`shard_set_override` / `ov_<mod>_set_override`) with a psx_fallback-
// gated trampoline — see game/object/actor_sm_reward.cpp and game/core/pc_scheduler.cpp for the
// pattern. Because that second path never calls `run()`, it must call `traceHit()` itself to stay
// visible on the `dispatch`/`ovhit` channels — otherwise a real, firing override looks "silent"
// and gets mistaken for dead code (this is exactly what happened to PcScheduler's 5 primitives).
//
// One per Game; registration at boot, read on every rec_dispatch.
#pragma once
#include <cstdint>
#include <cstdio>
struct Core;
class  Game;

typedef void (*EngineOverrideFn)(Core* c);

class EngineOverrides {
public:
  Game* game = nullptr;   // back-pointer wired by Game()

  // Wire addr -> fn. `name` is the trace label (native class::method). Aborts if full or dup.
  void register_(uint32_t addr, const char* name, EngineOverrideFn fn);

  // Called at the top of rec_dispatch for every target. Returns true if an override ran.
  // Never fires on a psx_fallback (SBS core B) Game — B is the pure substrate reference.
  bool run(Core* c, uint32_t addr);

  // Record a hit that did NOT go through run() — i.e. a shard_set_override/ov_<mod>_set_override
  // trampoline reached via the recompiler's OWN g_override[] table (a direct substrate `jal`,
  // never rec_dispatch). Call this from the trampoline BEFORE invoking the native handler, so the
  // `ovhit`/`dispatch` channels see the hit regardless of which of the two tables reached it.
  // `addr` must already be `register_`'d (asserts via fprintf+abort if not found — a trampoline
  // that traces an unregistered address is itself a bug). `viaDirect` labels the trace line.
  void traceHit(Core* c, uint32_t addr, bool viaDirect = true);

  // `ovhit` channel (PSXPORT_DEBUG=ovhit): dump per-address hit counts at exit — the definitive
  // "was this override ever reached" answer, cheap enough to run over a full session (a counter
  // bump, no per-hit I/O) unlike the verbose per-hit `dispatch` trace. Counts EVERY hit, via
  // either run() or traceHit(), so a registered-but-never-reached address reads 0 even though the
  // recdep/dispatch channels are individually blind to different subsets of callers (see the class
  // comment above + docs/findings/tooling.md).
  void dumpHitCounts() const;

  int count() const { return mN; }

private:
  static constexpr int kMax = 128;
  uint32_t         mAddr[kMax] = {0};
  const char*      mName[kMax] = {nullptr};
  EngineOverrideFn mFn[kMax]   = {nullptr};
  uint64_t         mHits[kMax] = {0};
  uint32_t         mLo = 0xFFFFFFFF, mHi = 0;   // [min,max] fast reject gate
  int              mN = 0;
};
