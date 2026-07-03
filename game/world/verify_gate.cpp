// game/world/verify_gate.cpp — class VerifyGate impl (see verify_gate.h).
#include "core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "verify_gate.h"

void VerifyGate::run(uint32_t (*fn)(Core*), uint32_t super_addr, const char* gate, int on) {
  Core* c = core;
  if (!on) { c->r[2] = fn(c); return; }
  if (!ram0) { ram0 = (uint8_t*)malloc(0x200000); ramN = (uint8_t*)malloc(0x200000); }
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = fn(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, super_addr);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nMismatch++ < 40) fprintf(stderr, "[%s] MISMATCH a0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n", gate, a0, v0_n, v0_o, ro, so, sp);
  } else if (++nMatch % 20 == 0) fprintf(stderr, "[%s] %ld matches\n", gate, nMatch);
}
