// game/core/verify_skip.cpp — VerifyHarness's pc_skip-vs-faithful OBSERVABLE gate (split out of the
// framework verify_harness.cpp during the P1.7c framework/game decoupling: this half reaches the
// game's observable set (observables.h) + SPU banks, so it lives game-side while the class itself is
// framework. The framework never calls skipArmed/skipCheck — only the pc_skip path (game) does.)
#include "core.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "verify_harness.h"
#include "observables.h"     // the game's positive-observable set (game/core)
#include "spu_state.h"       // SPU_PeekRAM / SPU_PokeRAM (the bound instance = this Game's SPU)
#include "game.h"            // pcSched.in_stage force-off during the oracle leg
bool VerifyHarness::skipArmed(uint32_t addr) {
  if (mSkipMode < 0) {
    const char* e = cfg_str("PSXPORT_SKIP_VERIFY");
    if (!e || !*e) mSkipMode = 0;
    else if (!strcmp(e, "all") || !strcmp(e, "ALL")) mSkipMode = 2;
    else {
      mSkipMode = 1;
      for (const char* p = e; p && *p && mSkipN < 32; ) {
        mSkipList[mSkipN++] = ((uint32_t)strtoul(p, nullptr, 0) & 0x1FFFFFFFu) | 0x80000000u;
        p = strchr(p, ','); if (p) p++;
      }
    }
  }
  if (mSkipMode == 0 || inCheck) return false;
  if (mSkipMode == 2) return true;
  uint32_t k = (addr & 0x1FFFFFFFu) | 0x80000000u;
  for (int i = 0; i < mSkipN; i++) if (mSkipList[i] == k) return true;
  return false;
}

void VerifyHarness::skipCheck(uint32_t addr, void (*skipFn)(void*), void* skipCtx,
                              void (*oracleFn)(void*), void* oracleCtx) {
  Core* c = core;
  if (!mStrictPreRam) { mStrictPreRam = (uint8_t*)malloc(0x200000); mStrictNatRam = (uint8_t*)malloc(0x200000); }
  if (!mSkipSpuA) { mSkipSpuA = (uint8_t*)malloc(524288); mSkipSpuB = (uint8_t*)malloc(524288); mSkipSpuPre = (uint8_t*)malloc(524288); }
  uint8_t preSpad[0x400]; uint32_t preRegs[34];
  inCheck = true;
  // pre-state (guest + SPU RAM — the sample banks are part of the observable set)
  memcpy(mStrictPreRam, c->ram, 0x200000); memcpy(preSpad, c->scratch, 0x400);
  memcpy(preRegs, c->r, 32 * 4); preRegs[32] = c->hi; preRegs[33] = c->lo;
  c->game->spu.bind(); SPU_PeekRAM(mSkipSpuPre);
  // leg 1: the SKIP shortcut
  skipFn(skipCtx);
  memcpy(mStrictNatRam, c->ram, 0x200000);
  uint8_t skipSpad[0x400]; memcpy(skipSpad, c->scratch, 0x400);
  uint32_t skipRegs[34]; memcpy(skipRegs, c->r, 32 * 4); skipRegs[32] = c->hi; skipRegs[33] = c->lo;
  c->game->spu.bind(); SPU_PeekRAM(mSkipSpuA);
  // rewind (guest + SPU)
  memcpy(c->ram, mStrictPreRam, 0x200000); memcpy(c->scratch, preSpad, 0x400);
  memcpy(c->r, preRegs, 32 * 4); c->hi = preRegs[32]; c->lo = preRegs[33];
  c->game->spu.bind(); SPU_PokeRAM(mSkipSpuPre);
  // leg 2: the substrate ORACLE arc, inline+synchronous — in_stage=0 makes the scheduler yield
  // prims (FUN_80051F80/FB4, normally reached via the override registry's native dispatch — which
  // is SUPPRESSED here, so the SUBSTRATE prims run and funnel into scheduler_yield) no-ops; the
  // override registry's native dispatch off = pure core-B behavior.
  int in_stage_save = c->game->pcSched.in_stage;
  c->game->pcSched.in_stage = 0;
  // Pre-deliver the sound-DMA-complete event: SPU-upload polls inside the oracle arc (e.g.
  // FUN_800753D4 -> FUN_80096A40 -> FUN_800993A0) busy-wait on it, and with yields no-op'd the
  // loop would spin forever (no frame stepping inside a leg). The skip legs deliver the same
  // event themselves (Asset::preload_cel); the observable list does not include the event table.
  c->game->hle.deliverEvent(0xF0000009u, 0xFFFFFFFFu);
  inSubstrateLeg = true;
  bool inCheck_save = inCheck; inCheck = false;   // allow the oracle leg's no-op yields (guard is for MV legs)
  oracleFn(oracleCtx);
  inCheck = inCheck_save;
  inSubstrateLeg = false;
  c->game->pcSched.in_stage = in_stage_save;
  c->game->spu.bind(); SPU_PeekRAM(mSkipSpuB);
  // compare the OBSERVABLE positive list: oracle post-state (current) vs skip post-state
  int bad = 0;
  for (int i = 0; i < kNObsRegions; i++) {
    const ObsRegion& R = kObsRegions[i];
    for (uint32_t a = R.lo; a < R.hi && bad < 16; a++) {
      bool spad = (a >> 24) == 0x1F;
      uint8_t vs = spad ? skipSpad[a & 0x3FF]      : mStrictNatRam[a & 0x1FFFFFu];
      uint8_t vo = spad ? c->scratch[a & 0x3FF]    : c->ram[a & 0x1FFFFFu];
      if (vs != vo) {
        fprintf(stderr, "[skip-verify] 0x%08X MISMATCH [%s] @0x%08X: skip=%02X oracle=%02X\n",
                addr, R.label, a, vs, vo); bad++;
      }
    }
  }
  { // per-area fx table deref (pointer chased on the oracle side)
    uint8_t area = (uint8_t)c->ram[0xBF870u];
    uint32_t po = *(uint32_t*)&c->ram[0xA4EF8u + area * 4u];
    uint32_t ps = *(uint32_t*)&mStrictNatRam[0xA4EF8u + area * 4u];
    if (po == ps && po && (po & 0x1FFFFFFFu) < 0x1FFE00u) {
      for (uint32_t o = 0; o < 0x200 && bad < 16; o++)
        if (mStrictNatRam[(po & 0x1FFFFFu) + o] != c->ram[(po & 0x1FFFFFu) + o]) {
          fprintf(stderr, "[skip-verify] 0x%08X MISMATCH [area_fx_deref] @0x%08X: skip=%02X oracle=%02X\n",
                  addr, po + o, mStrictNatRam[(po & 0x1FFFFFu) + o], c->ram[(po & 0x1FFFFFu) + o]); bad++;
        }
    }
  }
  if (memcmp(mSkipSpuA, mSkipSpuB, 524288) != 0) {
    for (uint32_t o = 0; o < 524288 && bad < 24; o++)
      if (mSkipSpuA[o] != mSkipSpuB[o]) {
        fprintf(stderr, "[skip-verify] 0x%08X MISMATCH [SPU RAM] @0x%05X: skip=%02X oracle=%02X\n",
                addr, o, mSkipSpuA[o], mSkipSpuB[o]); bad++;
        o += 15;   // sample the diff shape, don't spam every byte
      }
  }
  if (bad) {
    fprintf(stderr, "[skip-verify] 0x%08X FAILED (%d+ observable diffs) — skip leg does NOT produce "
                    "the oracle's observable output.\n", addr, bad);
    abort();
  }
  // match: continue from the SKIP result (the real pc_skip execution path)
  memcpy(c->ram, mStrictNatRam, 0x200000); memcpy(c->scratch, skipSpad, 0x400);
  memcpy(c->r, skipRegs, 32 * 4); c->hi = skipRegs[32]; c->lo = skipRegs[33];
  c->game->spu.bind(); SPU_PokeRAM(mSkipSpuA);
  inCheck = false;
  Check& k = check("skip-verify");
  if (++k.nMatch % 16 == 1) fprintf(stderr, "[skip-verify] 0x%08X OK (pass #%ld)\n", addr, k.nMatch);
}
