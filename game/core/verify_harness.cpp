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
