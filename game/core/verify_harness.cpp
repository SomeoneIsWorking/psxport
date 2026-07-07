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
