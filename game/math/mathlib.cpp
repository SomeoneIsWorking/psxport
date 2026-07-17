// game/math/mathlib.cpp — PC-native MATH/PRNG leaf primitives.
// The platform PRNG (rand LCG), the trig LUT lookups (sin/cos/quadrant), and the bitmap bit-test —
// pure leaf functions over guest tables/state, hot in the per-frame transform/anim math. Extracted
// verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code
// structure. Diagnostic A/B gates (randverify / trigverify / bitverify) are REPL channels, unchanged.
#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include "mathlib.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
void rec_super_call(Core*, uint32_t);

// FUN_8009A450 (the platform PRNG) is owned by `Rng::next` (game/math/rng.cpp) — same LCG
// (state*0x41C64E6D + 12345 at 0x80105EE8, returns (state>>16)&0x7FFF), called as `rngOf(c).next()`.
// This file used to carry an unused, uncalled duplicate (`rand_lcg`, ORPHAN) — deleted 2026-07-08
// (dual-ownership found via codemap; dead code, zero callers).
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

// FUN_8006F00C — sibling of setFE34: u32 flag-bit SET on 0x800BFE48 (the word testFE48 polls).
//   *(u32)0x800BFE48 |= (1u << idx);
void Bit::setFE48(int32_t idx) {
  Core* c = this->core;
  c->mem_w32(0x800BFE48u, c->mem_r32(0x800BFE48u) | (1u << ((uint32_t)idx & 31u)));
}

// FUN_8006F04C — child-link REQUEST-mailbox arbiter. disas 0x8006F04C..0x8006F0E0:
//   v1 = *(u8)0x800BF840; if ((v1 & 0x80) == 0) return;
//   id = v1 & 0xF;
//   if (id >= 9) goto CLEAR;                                  // sltiu id,9 guards the jump table
//   switch (id) via table @0x80016A8C (9 entries):
//     id==0,1,6 -> RETRY: cnt = *(u8)(0x800BFE3A+id); if (cnt<3) { cnt++; store; goto CLEAR; }
//                  else fall to GRANT
//     id==7,8   -> setFE34(id); fall to GRANT
//     id==2..5  -> goto CLEAR directly (no grant)
//   GRANT: setFE48(id);
//   CLEAR: *(u8)0x800BF840 = 0;
void Bit::processLinkRequest() {
  Core* c = this->core;
  uint8_t mailbox = c->mem_r8(0x800BF840u);
  if ((mailbox & 0x80u) == 0) return;
  uint32_t id = mailbox & 0xFu;
  bool grant = false;
  if (id < 9) {
    if (id == 0 || id == 1 || id == 6) {
      uint32_t addr = 0x800BFE3Au + id;
      uint8_t cnt = c->mem_r8(addr);
      if (cnt < 3) {
        c->mem_w8(addr, (uint8_t)(cnt + 1));
        c->mem_w8(0x800BF840u, 0);
        return;
      }
      grant = true;
    } else if (id == 7 || id == 8) {
      setFE34((int32_t)id);
      grant = true;
    }
    // id == 2..5: grant stays false (silently dropped)
  }
  if (grant) setFE48((int32_t)id);
  c->mem_w8(0x800BF840u, 0);
}
