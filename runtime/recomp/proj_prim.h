// class ProjPrim — per-Core vertex-depth cache used by the native depth path.
//
// submit.cpp records each vertex's view-space Z keyed by the packet vertex word's GUEST ADDRESS
// (setPz); the renderer's gp0_exec looks up the depth at each read address (lookupPz). Exact and
// deterministic by construction — replaced the value-keyed "attach" ring that could only correlate
// projected SXY back to a depth and was unreliable (same-pixel verts ambiguous; whole-frame staleness).
//
// PROPER OOP: one instance per Core, embedded on Render (`c->rsub.projprim`) — was a file-scope
// process-wide cache in gte_beetle.cpp before the SBS deglobalize sweep (2026-07-03). The two SBS
// cores need SEPARATE caches so their submits + lookups don't clobber each other's per-frame depths.
#pragma once
#include <stdint.h>
class Core;

class ProjPrim {
public:
  static constexpr int kMax = 65536;
  static constexpr int kHashSize = 16384;
  struct Stats {
    long set = 0;
    long hit = 0;
    long miss = 0;
    long stale = 0;
  };

  // How many generations an entry may survive. It was effectively 2 — one full buffer flip — because
  // an address-only entry could not be allowed to outlive the memory it described. With the word
  // guard on lookupPz an entry that outlives its vertex can no longer be SERVED, only refused, so
  // the limit is now about cache space rather than correctness: a pool deep enough to be drawn
  // several frames after it was filled keeps its depths. Raise it if a game's pool is deeper still;
  // the cost is entries kept, never a wrong depth.
  static constexpr int kGens = 8;

  // bind: mark this instance as the currently-bound cache. Parallels gte_bind/spu_bind — called per
  // core frame-step (native_step_frame) + at boot (dc_boot_init, game_main). Legacy hook for the
  // remaining call sites that lack a Core* in scope; new code should just reach the instance via
  // `c->rsub.projprim` directly.
  void bind(Core *c);
  static ProjPrim *current() {
    return sCurrent;
  }

  // reset: per-frame. Retires the OLDEST generation and starts a new one, keeping the PREVIOUS
  // frame's depths readable.
  //
  // WHY TWO GENERATIONS AND NOT ONE. A hard per-frame clear assumes a vertex is recorded and drawn in
  // the same frame. Engines with a DOUBLE-BUFFERED PACKET POOL fill pool A while the DMA draws pool B,
  // so a vertex recorded in frame N is drawn in frame N+1 and a one-frame cache wipes it in between.
  // That is not a hypothesis here: 6568 addresses in Spyro showed up as BOTH a record and a later
  // miss — the depth was recorded at exactly the address that then failed to resolve.
  //
  // Two is the right number, not "keep everything": the pool is reused, so an entry older than the
  // buffer cycle describes a vertex that no longer occupies that address, and serving it would be a
  // WRONG depth. Two generations covers one full flip and nothing more.
  void reset();

  // setPz: records a vertex's view-Z keyed by the packet-vertex GUEST ADDRESS `addr` — and stores
  // the WORD that was at that address, which is what makes the entry describe a vertex rather than
  // merely an address. See the note on lookupPz.
  void setPz(Core *c, uint32_t addr, float pz);

  // lookupPz: renderer's gp0_exec asks for the depth at packet-vertex read-address `addr`. Returns
  // true if hit (fills *pz), false on miss.
  //
  // THE ENTRY MUST STILL DESCRIBE THE WORD THAT IS THERE NOW. An address-only key is a claim about
  // memory that stops being true the moment the guest reuses that memory, and a packet pool exists
  // to be reused — so a 2D element written into a recycled pool slot would inherit the depth of the
  // 3D vertex that used to live there and sort into the world. That is not hypothetical: it is why
  // extending entry lifetime was abandoned once already, having depth-culled the player character
  // out of the frame. So an entry carries the word it was recorded against and is served only while
  // that word is unchanged; otherwise it is STALE, counted as such, and refused.
  bool lookupPz(Core *c, uint32_t addr, float *pz);

  // peekPz: same lookup, same staleness rule, but does NOT touch the hit/miss counters. Used by the
  // copy-propagation path, which probes an address for every word the guest copies — counting those
  // would swamp the render's own hit/miss ratio, which is the number that says whether native depth
  // is working at all.
  bool peekPz(Core *c, uint32_t addr, float *pz);

  // Near-miss histogram (`debug pznear`): for each missed lookup, which nearby offset WOULD have hit.
  // Separates "wrong buffer" from "right buffer, wrong word" — see the comment at the probe.
  void nearReport(const char *tag);

  bool overflowed() const {
    return mOverflow != 0;
  }
  int count() const {
    return mN;
  }
  Stats stats() const {
    return {mSetCt, mHitCt, mMissCt, mStaleCt};
  }
  void statsReset() {
    mSetCt = mHitCt = mMissCt = mStaleCt = 0;
  }

  // LIFETIME totals, never reset. statsReset() runs on every present, so the counters above only
  // ever describe one frame — which made "records=0 lookups=0" unreadable: it could mean the depth
  // path is dead, or merely that this particular frame drew nothing. These separate the two, and
  // are what render_depth_coverage_report() prints at run end.
  Stats totals() const {
    return {mSetTot, mHitTot, mMissTot, mStaleTot};
  }

private:
  struct Ent {
    uint32_t addr;
    float pz;
    int next;
    int gen;
    uint32_t word;
  };
  Ent mEntries[kMax];
  int mHead[kHashSize];
  int mN = 0, mInited = 0, mOverflow = 0;
  int mGen = 0; // current generation; entries from mGen and mGen-1 are readable
  long mSetCt = 0, mHitCt = 0, mMissCt = 0, mStaleCt = 0;
  long mSetTot = 0, mHitTot = 0, mMissTot = 0, mStaleTot = 0; // never reset; see totals()
  long mNear[16] = {0};
  long mNearMiss = 0;

  static ProjPrim *sCurrent;

  static uint32_t hashOf(uint32_t addr) {
    return ((addr >> 2) * 2654435761u) >> 18 & (kHashSize - 1);
  }
};
