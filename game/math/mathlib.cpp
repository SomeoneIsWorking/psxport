// engine/mathlib.cpp — PC-native MATH/PRNG leaf primitives.
// The platform PRNG (rand LCG), the trig LUT lookups (sin/cos/quadrant), and the bitmap bit-test —
// pure leaf functions over guest tables/state, hot in the per-frame transform/anim math. Extracted
// verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code
// structure. Diagnostic A/B gates (randverify / trigverify / bitverify) are REPL channels, unchanged.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include "mathlib.h"
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

// FUN_8004D7EC — pure bitmap bit-test (~2%, 6.8k calls): byte = bitmap[(int16)(a0/8)] then return
// byte & (1 << ((int16)(a0%8) & 31)); bitmap base is 0x800BFD34 when (a1&0xff)!=0 else 0x800BFCB4.
// Pure function over a guest bitmap — exact native reimpl. `bitverify` (lazy gate) A/B's v0.
void ov_bittest_4d7ec(Core* c) {
  static int v = -1; if (v < 0) v = cfg_dbg("bitverify") ? 1 : 0;
  int a0 = (int)c->r[4]; uint32_t a1 = c->r[5];
  int q  = (a0 >= 0) ? a0 : (a0 + 7);
  int a2 = q >> 3;                        // a0/8 toward zero
  int a3 = a0 - (a2 << 3);                // a0%8
  uint32_t base = (a1 & 0xff) ? (0x800BF870u + 1220u) : (0x800BF870u + 1092u);
  uint8_t byte = c->mem_r8(base + (uint32_t)(int32_t)(int16_t)a2);
  uint32_t mine = (uint32_t)byte & (1u << ((uint32_t)(int32_t)(int16_t)a3 & 31u));
  if (v) {
    rec_super_call(c, 0x8004D7ECu);
    static long ng = 0, nb = 0;
    if ((uint32_t)c->r[2] != mine) { if (nb++ < 20) fprintf(stderr, "[bitverify] MISMATCH a0=%d a1=%x mine=%x oracle=%x\n", a0, a1, mine, (uint32_t)c->r[2]); }
    else if (++ng % 20000 == 0) fprintf(stderr, "[bitverify] %ld matches\n", ng);
  }
  c->r[2] = mine;
}

void ov_rand(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("randverify") ? 1 : 0;
  if (!s_v) { c->r[2] = rand_lcg(c); return; }
  uint32_t st0 = c->mem_r32(0x80105EE8u);
  uint32_t mine = rand_lcg(c); uint32_t st_n = c->mem_r32(0x80105EE8u);
  c->mem_w32(0x80105EE8u, st0);                      // restore
  rec_super_call(c, 0x8009A450u);
  static long ng = 0, nb = 0;
  if (c->r[2] != mine || c->mem_r32(0x80105EE8u) != st_n) {
    if (nb++ < 20) fprintf(stderr, "[randverify] MISMATCH v0 mine=%x oracle=%x state mine=%x oracle=%x\n",
                          mine, (uint32_t)c->r[2], st_n, c->mem_r32(0x80105EE8u));
  } else if (++ng % 20000 == 0) fprintf(stderr, "[randverify] %ld matches\n", ng);
  c->r[2] = mine; c->mem_w32(0x80105EE8u, st_n);     // keep native
}
