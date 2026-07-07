// game/math/mathlib.cpp — PC-native MATH/PRNG leaf primitives.
// The platform PRNG (rand LCG), the trig LUT lookups (sin/cos/quadrant), and the bitmap bit-test —
// pure leaf functions over guest tables/state, hot in the per-frame transform/anim math. Extracted
// verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code
// structure. Diagnostic A/B gates (randverify / trigverify / bitverify) are REPL channels, unchanged.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include "mathlib.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
void rec_super_call(Core*, uint32_t);

// FUN_8009A450 — the platform PRNG (`rand`): the classic glibc LCG state*0x41C64E6D + 12345, state at
// 0x80105EE8, returns (state>>16)&0x7FFF. Called from many hot per-frame loops (particle/effect jitter,
// range-random 0x80032A44). Pure platform primitive — exact native reimplementation (mult low word =
// mflo). `randverify` (lazy REPL gate) snapshots/restores the state word and A/B's v0 + new state vs the
// recomp body.
static inline uint32_t rand_lcg(Core* c) {
  uint32_t st = c->mem_r32(0x80105EE8u) * 0x41C64E6Du + 12345u;
  c->mem_w32(0x80105EE8u, st);
  return (st >> 16) & 0x7FFFu;
}
// Trig LUTs (FUN_80083E80 sin / FUN_80083F50 cos) are OWNED by class Trig (game/math/trig.h),
// which is byte-perfect vs the substrate (verified by the CutsceneCamera oracle test). The old
// static trig_sin/trig_cos/trig_lut helpers + ov_trig_sin/ov_trig_cos/ov_trig_lut orphan
// override-handlers used to live here; all callers were migrated to Trig::rsin / Trig::rcos.

// FUN_8004D7EC — pure bitmap bit-test (~2%, 6.8k calls): byte = bitmap[(int16)(idx/8)] then return
// byte & (1 << ((int16)(idx%8) & 31)); bitmap base is 0x800BFD34 when (sel&0xff)!=0 else 0x800BFCB4.
// Pure function over a guest bitmap — exact native reimpl. `bitverify` (lazy gate) A/B's v0.
uint32_t Bit::test7EC(int32_t idx, uint32_t sel) {
  Core* c = this->core;
  int v = c->game->verify.on("bitverify");
  int q  = (idx >= 0) ? idx : (idx + 7);
  int a2 = q >> 3;                        // idx/8 toward zero
  int a3 = idx - (a2 << 3);               // idx%8
  uint32_t base = (sel & 0xff) ? (0x800BF870u + 1220u) : (0x800BF870u + 1092u);
  uint8_t byte = c->mem_r8(base + (uint32_t)(int32_t)(int16_t)a2);
  uint32_t mine = (uint32_t)byte & (1u << ((uint32_t)(int32_t)(int16_t)a3 & 31u));
  if (v) {
    c->r[4] = (uint32_t)idx; c->r[5] = sel;         // taxi-in for the still-taxi verify super-call
    rec_super_call(c, 0x8004D7ECu);
    VerifyHarness::Check& chk = c->game->verify.check("bitverify");
    long &ng = chk.nMatch, &nb = chk.nMismatch;
    if ((uint32_t)c->r[2] != mine) { if (nb++ < 20) fprintf(stderr, "[bitverify] MISMATCH idx=%d sel=%x mine=%x oracle=%x\n", idx, sel, mine, (uint32_t)c->r[2]); }
    else if (++ng % 20000 == 0) fprintf(stderr, "[bitverify] %ld matches\n", ng);
  }
  c->r[2] = mine;
  return mine;
}

// FUN_8004D868 — sibling of FUN_8004D7EC (bit-test) against a fixed third bitmap @0x800BFDB4
// (no sel selector). Same q=idx/8 toward-zero, r=idx%8, return byte & (1<<r). Pure guest-bitmap read.
// Shares the `bitverify` gate with test7EC.
uint32_t Bit::test868(int32_t idx) {
  Core* c = this->core;
  int v = c->game->verify.on("bitverify");
  int q  = (idx >= 0) ? idx : (idx + 7);
  int a2 = q >> 3;                        // idx/8 toward zero
  int a3 = idx - (a2 << 3);               // idx%8
  uint32_t base = 0x800BF870u + 1348u;    // = 0x800BFDB4
  uint8_t byte = c->mem_r8(base + (uint32_t)(int32_t)(int16_t)a2);
  uint32_t mine = (uint32_t)byte & (1u << ((uint32_t)(int32_t)(int16_t)a3 & 31u));
  if (v) {
    c->r[4] = (uint32_t)idx;                        // taxi-in for the still-taxi verify super-call
    rec_super_call(c, 0x8004D868u);
    VerifyHarness::Check& chk = c->game->verify.check("bitverify868");
    long &ng = chk.nMatch, &nb = chk.nMismatch;
    if ((uint32_t)c->r[2] != mine) { if (nb++ < 20) fprintf(stderr, "[bitverify868] MISMATCH idx=%d mine=%x oracle=%x\n", idx, mine, (uint32_t)c->r[2]); }
    else if (++ng % 20000 == 0) fprintf(stderr, "[bitverify868] %ld matches\n", ng);
  }
  c->r[2] = mine;
  return mine;
}

// FUN_8006EFF4 — u32 flag-bit TEST on the fixed 32-bit word at 0x800BFE48. Pure 5-instruction body:
//   v0 = *(u32)0x800BFE48;  return (v0 >> idx) & 1;   -- srav masks idx to & 31 at the ISA level.
uint32_t Bit::testFE48(int32_t idx) {
  Core* c = this->core;
  return (c->mem_r32(0x800BFE48u) >> ((uint32_t)idx & 31u)) & 1u;
}

// FUN_8006F02C — u32 flag-bit SET on the fixed 32-bit word at 0x800BFE34. 7-instruction body:
//   *(u32)0x800BFE34 |= (1u << idx);                  -- sllv masks idx to & 31 at the ISA level.
void Bit::setFE34(int32_t idx) {
  Core* c = this->core;
  c->mem_w32(0x800BFE34u, c->mem_r32(0x800BFE34u) | (1u << ((uint32_t)idx & 31u)));
}
