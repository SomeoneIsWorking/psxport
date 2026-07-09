// game/audio/sequencer.cpp — Sequencer method bodies. See sequencer.h for the RE contract.
//
// 0x80090BD0 SsSeqCalled (the pointer 0x800AC42C names its FUN_800909C0 caller expects) is NOT
// drafted here — Ghidra + generated/shard_3.c:21497 show it as a reentrancy-guarded (flag at
// 0x8010CC24) loop over up to 7 sequences x up to 15 channels each, testing a per-sequence active
// bitmask (0x8010CC28) and, per channel, testing 8 independent bit-flags in a per-channel struct
// at channel[+152] to conditionally call 7 DISTINCT unowned leaves (func_800910F0, func_80090E40
// x2 call-sites, func_80092080 x2, func_80091050, func_80091910, func_80091970) plus a prep call
// (func_800931C0). None of those 7 leaves is owned yet, and each is itself nontrivial libsnd
// channel-state logic (e.g. 0x80091910 toggles a channel "stop" bit + a busy flag — RE'd inline
// while scoping this draft, generated/shard_3.c:21635). Faithfully porting SsSeqCalled requires
// RE'ing all 7 leaves first; out of scope for this wide-RE pass (see fleet-workflow.md tier
// rules — MAP, don't half-draft). Left as a rec_dispatch call below; promote to a full native
// body once the leaf cluster is owned.

#include "audio/sequencer.h"
#include "core.h"

#define SEQ_USER_CB   0x800AC430u   // DAT_800ac430 — optional user callback fn-ptr
#define SEQ_TICK_FN   0x800AC42Cu   // DAT_800ac42c — *SsSeqCalled fn-ptr (0x80090BD0 today)

// 0x800909C0 FUN_800909c0 — libsnd per-VBlank tick wrapper. WIDE-RE DRAFT, UNWIRED (see header).
void Sequencer::frameTick() {
  Core* c = core;
  uint32_t cb = c->mem_r32(SEQ_USER_CB);
  if (cb != 0u) rec_dispatch(c, cb);
  uint32_t seq = c->mem_r32(SEQ_TICK_FN);
  rec_dispatch(c, seq);
}
