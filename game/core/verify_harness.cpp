// game/core/verify_harness.cpp — class VerifyHarness impl (see verify_harness.h).
#include "core.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "verify_harness.h"

VerifyHarness::Check& VerifyHarness::check(const char* name) {
  for (int i = 0; i < mNChecks; i++)
    if (mChecks[i].name == name || strcmp(mChecks[i].name, name) == 0) return mChecks[i];
  if (mNChecks < kMaxChecks) { mChecks[mNChecks].name = name; return mChecks[mNChecks++]; }
  return mChecks[kMaxChecks - 1];   // table full — diagnostics only, degrade to a shared entry
}

int VerifyHarness::on(const char* chan) {
  Check& k = check(chan);
  if (k.flag < 0) k.flag = cfg_dbg(chan) ? 1 : 0;
  return k.flag;
}

uint8_t* VerifyHarness::ram0() {
  if (!mRam0) mRam0 = (uint8_t*)malloc(0x200000);
  return mRam0;
}

uint8_t* VerifyHarness::ramN() {
  if (!mRamN) mRamN = (uint8_t*)malloc(0x200000);
  return mRamN;
}

void VerifyHarness::run(uint32_t (*fn)(Core*), uint32_t superAddr, const char* gate, int on) {
  Core* c = core;
  if (!on) { c->r[2] = fn(c); return; }
  uint8_t* r0 = ram0();
  uint8_t* rN = ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4];
  memcpy(r0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = fn(c);
  uint32_t v0_n = c->r[2];
  memcpy(rN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, r0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, superAddr);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != rN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  Check& k = check(gate);
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (k.nMismatch++ < 40) fprintf(stderr, "[%s] MISMATCH a0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n", gate, a0, v0_n, v0_o, ro, so, sp);
  } else if (++k.nMatch % 20 == 0) fprintf(stderr, "[%s] %ld matches\n", gate, k.nMatch);
}

// ---- STRICT mirror TDD gate (see header). USER 2026-07-08: every faithful mirror must be
// verified by RUNNING it against the substrate body — asserted verification is worthless. ----
void rec_dispatch(Core*, uint32_t);

bool VerifyHarness::strictArmed(uint32_t addr) {
  if (mStrictMode < 0) {
    const char* e = cfg_str("PSXPORT_MIRROR_VERIFY");
    if (!e || !*e) mStrictMode = 0;
    else if (!strcmp(e, "all") || !strcmp(e, "ALL")) mStrictMode = 2;
    else {
      mStrictMode = 1;
      for (const char* p = e; p && *p && mStrictN < 32; ) {
        mStrictList[mStrictN++] = ((uint32_t)strtoul(p, nullptr, 0) & 0x1FFFFFFFu) | 0x80000000u;
        p = strchr(p, ','); if (p) p++;
      }
    }
  }
  if (mStrictMode == 0 || inCheck) return false;
  if (mStrictMode == 2) return true;
  uint32_t k = (addr & 0x1FFFFFFFu) | 0x80000000u;
  for (int i = 0; i < mStrictN; i++) if (mStrictList[i] == k) return true;
  return false;
}

// ABI-relevant register set a caller/callee can consume after return; everything else is
// per-compiler scratch noise a mirror need not reproduce. NO memory exemptions — the guest
// stack is compared like any other RAM (that is the strictness this gate exists for).
static const int   kStrictReg[]  = { 2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31 };
static const char* kStrictName[] = { "v0","v1","s0","s1","s2","s3","s4","s5","s6","s7","gp","sp","fp","ra" };
static constexpr int kNStrictReg = 14;

void VerifyHarness::strictCheck(uint32_t addr, void (*fn)(void*), void* ctx) {
  Core* c = core;
  if (!mStrictPreRam) { mStrictPreRam = (uint8_t*)malloc(0x200000); mStrictNatRam = (uint8_t*)malloc(0x200000); }
  uint8_t preSpad[0x400]; uint32_t preRegs[34];
  inCheck = true;
  memcpy(mStrictPreRam, c->ram, 0x200000); memcpy(preSpad, c->scratch, 0x400);
  memcpy(preRegs, c->r, 32 * 4); preRegs[32] = c->hi; preRegs[33] = c->lo;
  fn(ctx);                                          // leg 1: the native mirror
  memcpy(mStrictNatRam, c->ram, 0x200000); memcpy(mStrictNatSpad, c->scratch, 0x400);
  for (int i = 0; i < kNStrictReg; i++) mStrictNatRegs[i] = c->r[kStrictReg[i]];
  mStrictNatRegs[14] = c->hi; mStrictNatRegs[15] = c->lo;
  memcpy(c->ram, mStrictPreRam, 0x200000); memcpy(c->scratch, preSpad, 0x400);
  memcpy(c->r, preRegs, 32 * 4); c->hi = preRegs[32]; c->lo = preRegs[33];
  inSubstrateLeg = true;                            // leg 2: pure substrate (EngineOverrides off)
  rec_dispatch(c, addr);
  inSubstrateLeg = false;
  int bad = 0;
  for (uint32_t i = 0; i < 0x200000 && bad < 16; i++)
    if (c->ram[i] != mStrictNatRam[i]) {
      fprintf(stderr, "[mirror-verify] 0x%08X MISMATCH ram 0x%08X: native=%02X substrate=%02X\n",
              addr, 0x80000000u + i, mStrictNatRam[i], c->ram[i]); bad++;
    }
  for (uint32_t i = 0; i < 0x400 && bad < 16; i++)
    if (c->scratch[i] != mStrictNatSpad[i]) {
      fprintf(stderr, "[mirror-verify] 0x%08X MISMATCH spad 0x%08X: native=%02X substrate=%02X\n",
              addr, 0x1F800000u + i, mStrictNatSpad[i], c->scratch[i]); bad++;
    }
  for (int i = 0; i < kNStrictReg; i++)
    if (c->r[kStrictReg[i]] != mStrictNatRegs[i]) {
      fprintf(stderr, "[mirror-verify] 0x%08X MISMATCH reg %s: native=%08X substrate=%08X\n",
              addr, kStrictName[i], mStrictNatRegs[i], c->r[kStrictReg[i]]); bad++;
    }
  if (c->hi != mStrictNatRegs[14] || c->lo != mStrictNatRegs[15]) {
    fprintf(stderr, "[mirror-verify] 0x%08X MISMATCH hi/lo: native=%08X/%08X substrate=%08X/%08X\n",
            addr, mStrictNatRegs[14], mStrictNatRegs[15], c->hi, c->lo); bad++;
  }
  if (bad) {
    fprintf(stderr, "[mirror-verify] 0x%08X FAILED (%d+ diffs) — native mirror is NOT byte-exact.\n", addr, bad);
    abort();
  }
  memcpy(c->ram, mStrictNatRam, 0x200000); memcpy(c->scratch, mStrictNatSpad, 0x400);
  for (int i = 0; i < kNStrictReg; i++) c->r[kStrictReg[i]] = mStrictNatRegs[i];
  c->hi = mStrictNatRegs[14]; c->lo = mStrictNatRegs[15];
  inCheck = false;
  Check& k = check("mirror-verify");
  if (++k.nMatch % 64 == 1) fprintf(stderr, "[mirror-verify] 0x%08X OK (pass #%ld)\n", addr, k.nMatch);
}

// ---- fork-level SKIP-vs-FAITHFUL observable gate (see header). ----
#include "observables.h"
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
  // prims (FUN_80051F80/FB4 via EngineOverrides... which are SUPPRESSED, so the SUBSTRATE prims
  // run and funnel into scheduler_yield) no-ops; EngineOverrides off = pure core-B behavior.
  int in_stage_save = c->game->pcSched.in_stage;
  c->game->pcSched.in_stage = 0;
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
