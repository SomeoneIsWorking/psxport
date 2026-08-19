// class ProjPrim — impl. See proj_prim.h for the design.
#include "core.h"   // Core::mem_r32 — the word guard reads the address it recorded against
#include "proj_prim.h"
#include <lucent/log.h>
#include <stdio.h>

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
  // WHY THE GENERATION IS STILL ROLLED PER FRAME, even though that is provably wrong for a
  // double-buffered packet pool — and what the correct fix has to do.
  //
  // A generation is meant to be one POOL BUFFER, and the retirement rule below ("older than one full
  // buffer flip") is only sound if generations and flips advance together. A pool that submits ~1600
  // prims on one frame and ZERO on the next burns a generation without a flip, so the buffer filled
  // two frames ago has its depths compacted away before the DMA draws it. Measured on such a port:
  // frames alternate between `hit=1547 miss=0` and `hit=0 miss=1540`, and a near-miss probe
  // (`debug pznear`) confirms the depths are not merely at the wrong offset — nothing within +/-32
  // bytes. Exactly half the frames lose every vertex.
  //
  // Rolling only on frames that RECORDED something fixes the alternation and takes resolved lookups
  // from 6.9% to 23% — AND BREAKS THE PICTURE: entries then outlive the address they describe, the
  // pool reuses those addresses, and stale depths get served as real ones. The player character was
  // depth-culled out of the frame. That is the hazard this rule was written against, so the naive
  // extension trades a visible miss for an invisible lie, which is worse.
  //
  // The sound fix keys entries so a reused address CANNOT alias — carry the pool buffer's identity in
  // the key, or invalidate on the pool pointer wrapping — rather than extending how long an
  // address-only key stays live. Until then the alternation stands, because half the vertices
  // resolving correctly beats all of them resolving wrongly.
  mGen++;
  int n = 0;
  for (int i = 0; i < kHashSize; i++) mHead[i] = -1;
  for (int i = 0; i < mN; i++) {
    if (mEntries[i].gen < mGen - (kGens - 1)) continue;   // beyond the retained generations
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

// The word currently at `addr`, read at the address the CALLER passed — never at one rebuilt from
// the masked key. pz_key strips the KSEG mirror bits, and re-adding a mirror to recover a readable
// address is a guess: scratchpad (0x1F800000) and low RAM normalize to keys that would need
// different mirrors, so a single reconstruction silently reads the wrong memory for one of them.
// Every caller already holds the real address; pass it.
static inline uint32_t word_at(Core* c, uint32_t addr) { return c->mem_r32(addr); }

void ProjPrim::setPz(Core* c, uint32_t addr, float pz) {
  mSetCt++; mSetTot++;
  if (!mInited) reset();
  const uint32_t w = word_at(c, addr);
  addr = pz_key(addr);
  if (s_pz_dbg_set < 12)
    { s_pz_dbg_set++; lucent::debug("pzaddr", "RECORD [{:06X}] pz={:.1f}", addr, (double)pz); }
  uint32_t h = hashOf(addr);
  for (int i = mHead[h]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) { mEntries[i].pz = pz; mEntries[i].gen = mGen; mEntries[i].word = w; return; }
  if (mN >= kMax) { mOverflow = 1; return; }
  Ent* e = &mEntries[mN];
  e->addr = addr; e->pz = pz; e->gen = mGen; e->word = w; e->next = mHead[h]; mHead[h] = mN++;
}

bool ProjPrim::peekPz(Core* c, uint32_t addr, float* pz) {
  if (!mInited) return false;
  const uint32_t real = addr;
  addr = pz_key(addr);
  for (int i = mHead[hashOf(addr)]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) {
      if (mEntries[i].word != word_at(c, real)) return false;   // the vertex it described is gone
      if (pz) *pz = mEntries[i].pz;
      return true;
    }
  return false;
}

bool ProjPrim::lookupPz(Core* c, uint32_t addr, float* pz) {
  if (!mInited) return false;
  const uint32_t real = addr;
  addr = pz_key(addr);
  for (int i = mHead[hashOf(addr)]; i >= 0; i = mEntries[i].next)
    if (mEntries[i].addr == addr) {
      // STALE: the address is ours, but the guest has since written something else there. Refusing
      // is the whole point — serving it is how a recycled pool slot hands a 2D element a world
      // depth. Counted apart from a plain miss because they want opposite fixes: stale means the
      // entry lifetime outran the buffer, absent means the depth was never recorded.
      if (mEntries[i].word != word_at(c, real)) {
        mStaleCt++; mStaleTot++;
        mMissCt++; mMissTot++;
        return false;
      }
      if (pz) *pz = mEntries[i].pz;
      mHitCt++; mHitTot++;
      return true;
    }
  mMissCt++; mMissTot++;
  if (s_pz_dbg_miss < 12)
    { s_pz_dbg_miss++; lucent::debug("pzaddr", "MISS   [{:06X}]", addr); }
  // NEAR-MISS HISTOGRAM (`debug pznear`). "Records climb, hits do not" has two completely different
  // causes — the depth is being attached to the WRONG BUFFER entirely, or to the RIGHT buffer at the
  // WRONG WORD — and the hit/miss ratio cannot tell them apart. This can: for every miss, probe the
  // cache at a spread of nearby offsets and count which one WOULD have hit. A flat-zero histogram
  // means the recorded addresses are nowhere near what the renderer reads (wrong buffer); a spike at
  // a single offset means the tap is one fixed stride off (right buffer, wrong word), which is a
  // one-line fix rather than an open question.
  //
  // Deliberately NOT capped at the first N: this fires on the same frames the summary describes, and
  // the first N misses of a run are boot frames with no geometry at all — the mistake `pzaddr`'s own
  // 12-line cap makes, which is why it has never answered this question.
  if (lucent::channel_on("pznear")) {
    static const int kOff[] = {-32,-28,-24,-20,-16,-12,-8,-4,4,8,12,16,20,24,28,32};
    for (int k = 0; k < (int)(sizeof kOff / sizeof kOff[0]); k++)
      if (peekPz(c, (uint32_t)((int32_t)real + kOff[k]), nullptr)) mNear[k]++;
    mNearMiss++;
  }
  return false;
}

// Report the near-miss histogram, and say plainly when it found nothing — a silent "(no data)" here
// would read exactly like "the offsets are all wrong", which is a different answer.
void ProjPrim::nearReport(const char* tag) {
  if (!lucent::channel_on("pznear")) return;
  static const int kOff[] = {-32,-28,-24,-20,-16,-12,-8,-4,4,8,12,16,20,24,28,32};
  long tot = 0;
  for (int k = 0; k < 16; k++) tot += mNear[k];
  if (!mNearMiss) { lucent::debug("pznear", "{}: no misses probed yet", tag ? tag : "(null)"); return; }
  if (!tot) {
    lucent::debug("pznear", "{}: {} misses probed, NONE had a recorded depth within +/-32 bytes — the "
                            "depths are not in this buffer at all, so no stride fix can help",
                  tag ? tag : "(null)", mNearMiss);
    mNearMiss = 0;
    return;
  }
  char line[256]; int n = 0;
  for (int k = 0; k < 16 && n < (int)sizeof line - 24; k++)
    if (mNear[k]) n += snprintf(line + n, sizeof line - n, " %+d:%ld", kOff[k], mNear[k]);
  lucent::debug("pznear", "{}: {} misses probed, {} had a recorded depth nearby —{}", tag ? tag : "(null)", mNearMiss, tot, line);
  for (int k = 0; k < 16; k++) mNear[k] = 0;
  mNearMiss = 0;
}
