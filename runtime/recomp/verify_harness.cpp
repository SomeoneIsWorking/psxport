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
    if (k.nMismatch++ < 40) cfg_logi("verify_harness", "[%s] MISMATCH a0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x", gate, a0, v0_n, v0_o, ro, so, sp);
  } else if (++k.nMatch % 20 == 0) cfg_logi("verify_harness", "[%s] %ld matches", gate, k.nMatch);
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

void VerifyHarness::journalTrack(uint32_t off, uint8_t origByte) {
  if (!mJournalDirty) mJournalDirty = (uint8_t*)calloc(0x200000 / 8, 1);
  uint32_t byteIdx = off >> 3; uint8_t bit = (uint8_t)(1u << (off & 7));
  if (mJournalDirty[byteIdx] & bit) return;   // already touched this invocation — orig already recorded
  mJournalDirty[byteIdx] |= bit;
  mJournal.push_back({off, origByte, origByte});   // .nat defaults to .orig (right for a leg-2-only touch — see strictCheck)
}

// strictCheck: the fast journal-based path (default). Dispatches to strictCheckFull under
// PSXPORT_MIRROR_VERIFY_FULL=1 — the authoritative full-2MB-scan reference this path is validated
// against (same verdicts / same first mismatch, see docs/config.md "Mirror TDD gate").
//
// Two-leg protocol (mirrors strictCheckFull's semantics exactly, just incrementally):
//   1. Snapshot scratch+regs (small, still a full copy — cheap). Arm the journal, run leg 1 (native
//      mirror). Every first-touch RAM byte gets an entry {addr, orig, nat=orig}. After leg 1, fix up
//      .nat for those n1 entries to the byte's ACTUAL leg-1-final value (mJournal[i].nat = c->ram[addr]).
//   2. Rewind: write .orig back over just the n1 touched bytes (not all 2 MB), restore scratch+regs.
//      Arm the journal again (bitmap carries over, so leg-1 addresses are NOT re-recorded — their
//      .orig is already correct) and run leg 2 (pure substrate replay). Any NEW address leg 2 touches
//      gets an entry with .nat left at .orig — correct, because leg 1 never wrote there, so leg 1's
//      "final" value for that byte IS the original (this exactly matches strictCheckFull, where an
//      address untouched by leg 1 keeps its pre-leg1 value in the post-leg1 snapshot).
//   3. Compare: for every entry in the UNION (both legs), current c->ram[addr] (leg 2's result, never
//      rewound) vs .nat (leg 1's result). This is the same comparison strictCheckFull does over all
//      2 MB — restricted to the only addresses that could possibly differ (anything neither leg wrote
//      is byte-identical by construction, so skipping it is not a narrowing, it's a proof by
//      construction that it can't diverge).
//   4. Continue-from-native: write .nat back over the union (matches strictCheckFull's unconditional
//      `memcpy(c->ram, mStrictNatRam, ...)` — for a leg-2-only address that's .orig, i.e. "as if leg 2
//      never ran", identical to what a full memcpy from the post-leg1 snapshot would produce there).
void VerifyHarness::strictCheck(uint32_t addr, void (*fn)(void*), void* ctx) {
  if (mFullMode < 0) mFullMode = cfg_on("PSXPORT_MIRROR_VERIFY_FULL") ? 1 : 0;
  if (mFullMode) { strictCheckFull(addr, fn, ctx); return; }
  Core* c = core;
  uint8_t preSpad[0x400]; uint32_t preRegs[34];
  inCheck = true;
  memcpy(preSpad, c->scratch, 0x400);
  memcpy(preRegs, c->r, 32 * 4); preRegs[32] = c->hi; preRegs[33] = c->lo;
  uint32_t entrySp = preRegs[29], entryRa = preRegs[31];
  uint64_t invocation = mLastMirrorCount;   // stamped by mirrorSampleGate just before this call

  mJournal.clear();
  mJournalArmed = true;
  fn(ctx);                                          // leg 1: the native mirror
  mJournalArmed = false;
  size_t n1 = mJournal.size();
  for (size_t i = 0; i < n1; i++) mJournal[i].nat = c->ram[mJournal[i].addr];   // leg-1-final values
  uint8_t natSpad[0x400]; memcpy(natSpad, c->scratch, 0x400);
  uint32_t natRegs[kNStrictReg]; for (int i = 0; i < kNStrictReg; i++) natRegs[i] = c->r[kStrictReg[i]];
  uint32_t natHi = c->hi, natLo = c->lo;

  for (size_t i = 0; i < n1; i++) c->ram[mJournal[i].addr] = mJournal[i].orig;   // rewind (touched bytes only)
  memcpy(c->scratch, preSpad, 0x400);
  memcpy(c->r, preRegs, 32 * 4); c->hi = preRegs[32]; c->lo = preRegs[33];

  inSubstrateLeg = true;                            // leg 2: pure substrate (override registry's native dispatch off)
  mJournalArmed = true;                              // journal continues into the SAME touched-set
  rec_dispatch(c, addr);
  mJournalArmed = false;
  inSubstrateLeg = false;

  for (size_t i = 0; i < mJournal.size(); i++) {     // clear only the bits we set (not the whole bitmap)
    uint32_t off = mJournal[i].addr;
    mJournalDirty[off >> 3] &= (uint8_t)~(1u << (off & 7));
  }

  int bad = 0;
  bool headerPrinted = false;
  auto printHeader = [&]() {
    if (headerPrinted) return;
    headerPrinted = true;
    cfg_logi("mirror-verify", "0x%08X MISMATCH at invocation #%llu entry sp=%08X ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X", addr, (unsigned long long)invocation, entrySp, entryRa, preRegs[4], preRegs[5], preRegs[6], preRegs[7]);
  };
  for (size_t i = 0; i < mJournal.size() && bad < 16; i++) {
    uint32_t a = mJournal[i].addr;
    uint8_t curVal = c->ram[a];                      // leg 2's result (never rewound)
    if (curVal != mJournal[i].nat) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH ram 0x%08X: native=%02X substrate=%02X", addr, 0x80000000u + a, mJournal[i].nat, curVal); bad++;
    }
  }
  for (uint32_t i = 0; i < 0x400 && bad < 16; i++)
    if (c->scratch[i] != natSpad[i]) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH spad 0x%08X: native=%02X substrate=%02X", addr, 0x1F800000u + i, natSpad[i], c->scratch[i]); bad++;
    }
  for (int i = 0; i < kNStrictReg; i++)
    if (c->r[kStrictReg[i]] != natRegs[i]) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH reg %s: native=%08X substrate=%08X", addr, kStrictName[i], natRegs[i], c->r[kStrictReg[i]]); bad++;
    }
  if (c->hi != natHi || c->lo != natLo) {
    printHeader();
    cfg_logi("mirror-verify", "0x%08X MISMATCH hi/lo: native=%08X/%08X substrate=%08X/%08X", addr, natHi, natLo, c->hi, c->lo); bad++;
  }
  if (bad) {
    bool cont = cfg_on("PSXPORT_MIRROR_VERIFY_CONTINUE");
    Check& k = check("mirror-verify");
    k.nMismatch++;
    if (!cont) {
      cfg_loge("mirror-verify", "0x%08X FAILED (%d+ diffs) — native mirror is NOT byte-exact. Aborting (set PSXPORT_MIRROR_VERIFY_CONTINUE=1 to log-and-continue).", addr, bad);
      abort();
    }
    cfg_logi("mirror-verify", "0x%08X CONTINUING past %d+ diffs (PSXPORT_MIRROR_VERIFY_CONTINUE=1) — execution proceeds from the NATIVE result.", addr, bad);
  }
  for (size_t i = 0; i < mJournal.size(); i++) c->ram[mJournal[i].addr] = mJournal[i].nat;   // continue-from-native
  memcpy(c->scratch, natSpad, 0x400);
  for (int i = 0; i < kNStrictReg; i++) c->r[kStrictReg[i]] = natRegs[i];
  c->hi = natHi; c->lo = natLo;
  inCheck = false;
  if (!bad) {
    Check& k = check("mirror-verify");
    if (++k.nMatch % 64 == 1) cfg_logi("mirror-verify", "0x%08X OK (pass #%ld)", addr, k.nMatch);
  }
}

// strictCheckFull: the ORIGINAL full-2MB-snapshot/compare path (PSXPORT_MIRROR_VERIFY_FULL=1). Kept
// as the authoritative slow reference strictCheck's fast journal path is validated against.
void VerifyHarness::strictCheckFull(uint32_t addr, void (*fn)(void*), void* ctx) {
  Core* c = core;
  if (!mStrictPreRam) { mStrictPreRam = (uint8_t*)malloc(0x200000); mStrictNatRam = (uint8_t*)malloc(0x200000); }
  uint8_t preSpad[0x400]; uint32_t preRegs[34];
  inCheck = true;
  memcpy(mStrictPreRam, c->ram, 0x200000); memcpy(preSpad, c->scratch, 0x400);
  memcpy(preRegs, c->r, 32 * 4); preRegs[32] = c->hi; preRegs[33] = c->lo;
  uint32_t entrySp = preRegs[29], entryRa = preRegs[31];
  uint64_t invocation = mLastMirrorCount;   // stamped by mirrorSampleGate just before this call
  fn(ctx);                                          // leg 1: the native mirror
  memcpy(mStrictNatRam, c->ram, 0x200000); memcpy(mStrictNatSpad, c->scratch, 0x400);
  for (int i = 0; i < kNStrictReg; i++) mStrictNatRegs[i] = c->r[kStrictReg[i]];
  mStrictNatRegs[14] = c->hi; mStrictNatRegs[15] = c->lo;
  memcpy(c->ram, mStrictPreRam, 0x200000); memcpy(c->scratch, preSpad, 0x400);
  memcpy(c->r, preRegs, 32 * 4); c->hi = preRegs[32]; c->lo = preRegs[33];
  inSubstrateLeg = true;                            // leg 2: pure substrate (override registry's native dispatch off)
  rec_dispatch(c, addr);
  inSubstrateLeg = false;
  int bad = 0;
  bool headerPrinted = false;
  auto printHeader = [&]() {
    if (headerPrinted) return;
    headerPrinted = true;
    cfg_logi("mirror-verify", "0x%08X MISMATCH at invocation #%llu entry sp=%08X ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X", addr, (unsigned long long)invocation, entrySp, entryRa, preRegs[4], preRegs[5], preRegs[6], preRegs[7]);
  };
  for (uint32_t i = 0; i < 0x200000 && bad < 16; i++)
    if (c->ram[i] != mStrictNatRam[i]) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH ram 0x%08X: native=%02X substrate=%02X", addr, 0x80000000u + i, mStrictNatRam[i], c->ram[i]); bad++;
    }
  for (uint32_t i = 0; i < 0x400 && bad < 16; i++)
    if (c->scratch[i] != mStrictNatSpad[i]) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH spad 0x%08X: native=%02X substrate=%02X", addr, 0x1F800000u + i, mStrictNatSpad[i], c->scratch[i]); bad++;
    }
  for (int i = 0; i < kNStrictReg; i++)
    if (c->r[kStrictReg[i]] != mStrictNatRegs[i]) {
      printHeader();
      cfg_logi("mirror-verify", "0x%08X MISMATCH reg %s: native=%08X substrate=%08X", addr, kStrictName[i], mStrictNatRegs[i], c->r[kStrictReg[i]]); bad++;
    }
  if (c->hi != mStrictNatRegs[14] || c->lo != mStrictNatRegs[15]) {
    printHeader();
    cfg_logi("mirror-verify", "0x%08X MISMATCH hi/lo: native=%08X/%08X substrate=%08X/%08X", addr, mStrictNatRegs[14], mStrictNatRegs[15], c->hi, c->lo); bad++;
  }
  if (bad) {
    bool cont = cfg_on("PSXPORT_MIRROR_VERIFY_CONTINUE");
    Check& k = check("mirror-verify");
    k.nMismatch++;
    if (!cont) {
      cfg_loge("mirror-verify", "0x%08X FAILED (%d+ diffs) — native mirror is NOT byte-exact. Aborting (set PSXPORT_MIRROR_VERIFY_CONTINUE=1 to log-and-continue).", addr, bad);
      abort();
    }
    cfg_logi("mirror-verify", "0x%08X CONTINUING past %d+ diffs (PSXPORT_MIRROR_VERIFY_CONTINUE=1) — execution proceeds from the NATIVE result.", addr, bad);
  }
  memcpy(c->ram, mStrictNatRam, 0x200000); memcpy(c->scratch, mStrictNatSpad, 0x400);
  for (int i = 0; i < kNStrictReg; i++) c->r[kStrictReg[i]] = mStrictNatRegs[i];
  c->hi = mStrictNatRegs[14]; c->lo = mStrictNatRegs[15];
  inCheck = false;
  if (!bad) {
    Check& k = check("mirror-verify");
    if (++k.nMatch % 64 == 1) cfg_logi("mirror-verify", "0x%08X OK (pass #%ld)", addr, k.nMatch);
  }
}

// mirrorSampleGate: the generalized "verify ALL wired overrides" entry point. The override
// registry's shared dispatch thunk (runtime/recomp/override_registry.h) calls this instead of
// strictArmed(addr) directly, so PSXPORT_MIRROR_VERIFY=all covers every wired address with no
// per-call-site MV_CHECK.
bool VerifyHarness::mirrorSampleGate(uint32_t addr) {
  if (!strictArmed(addr)) return false;   // handles mode parsing + inCheck no-nesting guard
  if (mMirrorEvery < 0) {
    mMirrorEvery = cfg_int("PSXPORT_MIRROR_VERIFY_EVERY", 1);
    if (mMirrorEvery < 1) mMirrorEvery = 1;
  }
  uint32_t k = addr & 0x1FFFFFFFu;
  int slot = -1;
  for (int i = 0; i < mMirrorCntN; i++) if (mMirrorCntAddr[i] == k) { slot = i; break; }
  if (slot < 0) {
    if (mMirrorCntN < kMaxMirrorAddrs) { slot = mMirrorCntN++; mMirrorCntAddr[slot] = k; mMirrorCnt[slot] = 0; }
    else slot = kMaxMirrorAddrs - 1;    // table full — diagnostics only, degrade to a shared counter
  }
  uint64_t n = ++mMirrorCnt[slot];
  mLastMirrorCount = n;
  return mMirrorEvery <= 1 || (n % (uint64_t)mMirrorEvery) == 1;
}

