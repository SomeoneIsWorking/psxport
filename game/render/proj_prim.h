// class ProjPrim — per-Core vertex-depth cache used by the native depth path.
//
// engine_submit records each vertex's view-space Z keyed by the packet vertex word's GUEST ADDRESS
// (setPz); the renderer's gp0_exec looks up the depth at each read address (lookupPz). Exact and
// deterministic by construction — replaced the value-keyed "attach" ring that could only correlate
// projected SXY back to a depth and was unreliable (same-pixel verts ambiguous; whole-frame staleness).
//
// PROPER OOP: one instance per Core, embedded on Render (`c->mRender->projprim`) — was a file-scope
// process-wide cache in gte_beetle.cpp before the SBS deglobalize sweep (2026-07-03). The two SBS
// cores need SEPARATE caches so their submits + lookups don't clobber each other's per-frame depths.
#pragma once
#include <stdint.h>
class Core;

class ProjPrim {
public:
  static constexpr int kMax      = 65536;
  static constexpr int kHashSize = 16384;
  struct Stats { long set = 0; long hit = 0; long miss = 0; };

  // bind: mark this instance as the currently-bound cache. Parallels gte_bind/spu_bind — called per
  // core frame-step (native_step_frame) + at boot (dc_boot_init, game_main). Legacy hook for the
  // remaining call sites that lack a Core* in scope; new code should just reach the instance via
  // `c->mRender->projprim` directly.
  void bind(Core* c);
  static ProjPrim* current() { return sCurrent; }

  // reset: per-frame — drop last frame's depths so none are read stale.
  void reset();

  // setPz: engine_submit records a vertex's view-Z keyed by the packet-vertex GUEST ADDRESS `addr`.
  void setPz(uint32_t addr, float pz);

  // lookupPz: renderer's gp0_exec asks for the depth at packet-vertex read-address `addr`. Returns
  // true if hit (fills *pz), false on miss.
  bool lookupPz(uint32_t addr, float* pz);

  bool overflowed() const { return mOverflow != 0; }
  int  count()      const { return mN; }
  Stats stats()     const { return {mSetCt, mHitCt, mMissCt}; }
  void  statsReset()      { mSetCt = mHitCt = mMissCt = 0; }

private:
  struct Ent { uint32_t addr; float pz; int next; };
  Ent  mEntries[kMax];
  int  mHead[kHashSize];
  int  mN = 0, mInited = 0, mOverflow = 0;
  long mSetCt = 0, mHitCt = 0, mMissCt = 0;

  static ProjPrim* sCurrent;

  static uint32_t hashOf(uint32_t addr) {
    return ((addr >> 2) * 2654435761u) >> 18 & (kHashSize - 1);
  }
};
