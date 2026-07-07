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
// One per Game; registration at boot, read on every rec_dispatch.
#pragma once
#include <cstdint>
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

  int count() const { return mN; }

private:
  static constexpr int kMax = 128;
  uint32_t         mAddr[kMax] = {0};
  const char*      mName[kMax] = {nullptr};
  EngineOverrideFn mFn[kMax]   = {nullptr};
  uint32_t         mLo = 0xFFFFFFFF, mHi = 0;   // [min,max] fast reject gate
  int              mN = 0;
};
