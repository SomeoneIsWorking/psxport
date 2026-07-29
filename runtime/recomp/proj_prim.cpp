// class ProjPrim — impl. See proj_prim.h for the design.
#include "proj_prim.h"
#include "cfg.h"

ProjPrim* ProjPrim::sCurrent = nullptr;

void ProjPrim::bind(Core* /*c*/) { sCurrent = this; }

// `debug pzaddr` — print the first few addresses RECORDED and the first few that MISSED a lookup, in
// the same frame. Native depth only works if those two sets are the same addresses, and when they are
// not, no amount of extra recording coverage helps: the counters alone (records=N, hit=0, miss=M)
// cannot tell "the wrong vertices are recorded" from "not enough vertices are recorded", and those
// want opposite fixes. Printing both sets answers it directly instead of by inference.
static int s_pz_dbg_set = 0, s_pz_dbg_miss = 0;

void ProjPrim::reset() {
  s_pz_dbg_set = s_pz_dbg_miss = 0;   // per-frame, so the two sets printed are from the SAME frame
  if (!mInited) {
    mN = 0; mOverflow = 0; mGen = 0;
    for (int i = 0; i < kHashSize; i++) mHead[i] = -1;
    mInited = 1;
    return;
  }
  // Advance the generation and drop entries older than the previous one. Compaction is a single pass
  // over the live entries; the table is small and this runs once per frame.
  mGen++;
  int n = 0;
  for (int i = 0; i < kHashSize; i++) mHead[i] = -1;
  for (int i = 0; i < mN; i++) {
    if (mEntries[i].gen < mGen - 1) continue;      // older than one full buffer flip
    Ent e = mEntries[i];
    uint32_t h = hashOf(e.addr);
    mEntries[n] = e; mEntries[n].next = mHead[h]; mHead[h] = n; n++;
  }
  mN = n;
  mOverflow = 0;
}

// Key normalization. Strip the KSEG mirror bits (0x80000000 / 0xA0000000) so the same physical word
// keys identically however the guest addressed it — but keep enough of the address to tell the
// SCRATCHPAD (0x1F800000) apart from low RAM. The old `& 0x1FFFFC` folded them together: a vertex
// staged at scratchpad 0x1F800018 and one at RAM 0x00000018 became the same key, so a staged vertex
// could silently answer a lookup for an unrelated RAM packet — a WRONG depth, which is worse than a
// miss. Spyro stages vertices in the scratchpad, so this collision is reachable, not theoretical.
static inline uint32_t pz_key(uint32_t addr) { return addr & 0x1FFFFFFC; }

void ProjPrim::setPz(uint32_t addr, float pz) {
  mSetCt++;
  if (!mInited) reset();
  addr = pz_key(addr);
  if (cfg_dbg("pzaddr") && s_pz_dbg_set < 12)
    { s_pz_dbg_set++; cfg_logf("pzaddr", "RECORD [%06X] pz=%.1f", addr, (double)pz); }
  uint32_t h = hashOf(addr);
  for (int i = mHead[h]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) { mEntries[i].pz = pz; mEntries[i].gen = mGen; return; }
  if (mN >= kMax) { mOverflow = 1; return; }
  Ent* e = &mEntries[mN];
  e->addr = addr; e->pz = pz; e->gen = mGen; e->next = mHead[h]; mHead[h] = mN++;
}

bool ProjPrim::peekPz(uint32_t addr, float* pz) {
  if (!mInited) return false;
  addr = pz_key(addr);
  for (int i = mHead[hashOf(addr)]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) { if (pz) *pz = mEntries[i].pz; return true; }
  return false;
}

bool ProjPrim::lookupPz(uint32_t addr, float* pz) {
  if (!mInited) return false;
  addr = pz_key(addr);
  for (int i = mHead[hashOf(addr)]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) {
      if (pz) *pz = mEntries[i].pz;
      mHitCt++;
      return true;
    }
  mMissCt++;
  if (cfg_dbg("pzaddr") && s_pz_dbg_miss < 12)
    { s_pz_dbg_miss++; cfg_logf("pzaddr", "MISS   [%06X]", addr); }
  return false;
}
