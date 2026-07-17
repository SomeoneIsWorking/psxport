// class ProjPrim — impl. See proj_prim.h for the design.
#include "proj_prim.h"

ProjPrim* ProjPrim::sCurrent = nullptr;

void ProjPrim::bind(Core* /*c*/) { sCurrent = this; }

void ProjPrim::reset() {
  mN = 0; mOverflow = 0;
  for (int i = 0; i < kHashSize; i++) mHead[i] = -1;
  mInited = 1;
}

void ProjPrim::setPz(uint32_t addr, float pz) {
  mSetCt++;
  if (!mInited) reset();
  addr &= 0x1FFFFC;
  uint32_t h = hashOf(addr);
  for (int i = mHead[h]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) { mEntries[i].pz = pz; return; }
  if (mN >= kMax) { mOverflow = 1; return; }
  Ent* e = &mEntries[mN];
  e->addr = addr; e->pz = pz; e->next = mHead[h]; mHead[h] = mN++;
}

bool ProjPrim::lookupPz(uint32_t addr, float* pz) {
  if (!mInited) return false;
  addr &= 0x1FFFFC;
  for (int i = mHead[hashOf(addr)]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) {
      if (pz) *pz = mEntries[i].pz;
      mHitCt++;
      return true;
    }
  mMissCt++;
  return false;
}
