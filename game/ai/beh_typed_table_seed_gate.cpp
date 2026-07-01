// engine/beh_typed_table_seed_gate.cpp — PC-native per-object BEHAVIOR handler FUN_80133C14 (OVERLAY).
//
// An OVERLAY per-object behavior routine installed at node+0x1c by the field object driver and called
// every frame by the entity walk with the node in a0. Same SHAPE as the resident siblings
// FUN_800739AC (engine/the FUN_739ac handler.cpp) and FUN_80073CD8 (engine/the FUN_73cd8 handler.cpp): a small state
// machine on the node's state byte node[4] (0 init / 1 active / 2 idle / 3 despawn), but SIMPLER than
// either — there is no node[5] sub-machine.
//
//   state 0 (init): FUN_80051b70(obj, 0xc, 0x14) cull-record init; on success seed node fields
//                   (node[0x2a]=0x22, box/size 0x80..0x86, node[0]=node[4]=1, clear 0x29/0x46/0x42),
//                   call FUN_8004766c, look up a per-type halfword from data table 0x8014A6E4 indexed
//                   by node[3]*2 -> node[0x32], set node[0x60]=-0xc8. Advances to state 1.
//   state 1 (active): FUN_801337e4(obj); then a global gate on byte 0x800E7EAA:
//                   if (g < 0x1c)            -> just stash node[0x62]=state, return (early field path)
//                   else if (g == 0x22)      -> node[1]=state, FUN_80077ebc, then render tail
//                   else                     -> FUN_800778e4(obj, (int16)node[0x60]); if it returns 0,
//                                               stash node[0x62]=state and return; if nonzero -> render
//                                               tail (FUN_8004766c + FUN_800517f8).
//   state 2 / 3   : FUN_8007a624(obj)  (despawn — BOTH states 2 and 3 take this path).
//   state >=4 / impossible: epilogue (no-op).
//
// The data at 0x8014A6E4 is a halfword DATA table (loaded via lhu, indexed node[3]*2) — NOT a jump
// table; control flow has no computed jumps. Ownership model identical to the FUN_739ac handler/73cd8: CONTROL
// FLOW + every node/global memory write owned native, byte-for-byte; every sub-behavior CALL stays a
// rec_dispatch leaf (a0 set first). NO GTE, NO render packets here. RE'd 1:1 from disas 0x80133C14
// (epilogue jr ra @0x80133D64) on field RAM dump scratch/bin/field_ram_230.bin.
//
// Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) via channel "typed_table_seed_gateverify".

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80133C14u;

// Per-type halfword DATA table (lhu, indexed by node[3]*2). Loaded into node[0x32] in state 0
// (and likewise by FUN_801337e4). NOT control flow.
constexpr uint32_t TBL_A6E4 = 0x8014A6E4u;   // lui 0x8015; addiu -0x591c

void beh_typed_table_seed_gate(Core* c) {
  const uint32_t obj = c->r[4];               // 80133C1C move s1,a0
  uint8_t st = c->mem_r8(obj + 4);            // 80133C2C lbu s0,4(s1)   (state)

  // ---- dispatch (80133C34..80133C64) ----
  if (st != 1) {                              // 80133C34 beq s0,1 -> state 1
    if (st >= 2) {                            // 80133C38 slti v0,s0,2 ; 80133C3C beqz -> >=2  (s0 is lbu 0..255, so signed slti == unsigned compare here)
      // 80133C54 slti v0,s0,4 ; 80133C58 beqz -> epilogue (state>=4: no-op)
      if (st >= 4) return;
      // 80133C60 j 0x80133D4C  (state 2 OR 3 -> despawn)
      world_despawn(c, obj);           // 80133D4C jal 0x8007a624
      return;                                 // -> epilogue
    }
    if (st != 0) return;                      // 80133C44 beqz s0 -> state 0; else 80133C4C j epilogue
    // ---- STATE 0 (80133C68..80133CE0): cull-record init + field/box setup ----
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = 0x14;     // 80133C68 a1=0xc ; 80133C70 a2=0x14 ; a0=s1
    rec_dispatch(c, 0x80051B70u);                     // 80133C6C jal 0x80051b70
    if (c->r[2] != 0) return;                         // 80133C74 bnez v0 -> epilogue (init busy)
    c->mem_w8 (obj + 0x2a, 0x22);            // 80133C80 sb 0x22,0x2a(s1)
    c->mem_w16(obj + 0x80, 0x1e);            // 80133C88 sh 0x1e,0x80(s1)
    c->mem_w16(obj + 0x82, 0x3c);            // 80133C90 sh 0x3c,0x82(s1)
    c->mem_w16(obj + 0x84, 0x32);            // 80133C98 sh 0x32,0x84(s1)
    c->mem_w8 (obj + 4, 1);                  // 80133CA0 sb s2(1),4(s1)   (-> state 1)
    c->mem_w8 (obj + 0, 1);                  // 80133CA4 sb s2(1),0(s1)
    c->mem_w8 (obj + 0x29, 0);               // 80133CA8 sb zero,0x29(s1)
    c->mem_w8 (obj + 0x46, 0);               // 80133CAC sb zero,0x46(s1)
    c->mem_w16(obj + 0x86, 0x64);            // 80133CB4 sh v0(0x64),0x86(s1)  (delay slot of jal; v0=0x64 from 80133C9C)
    c->r[4] = obj; rec_dispatch(c, 0x8004766Cu);      // 80133CB0 jal 0x8004766c (a0=s1)
    uint8_t n3 = c->mem_r8(obj + 3);                  // 80133CBC lbu v0,3(s1)
    c->mem_w16(obj + 0x42, 0);               // 80133CC4 sh zero,0x42(s1)
    uint16_t tv = c->mem_r16(TBL_A6E4 + (uint32_t)n3 * 2);  // 80133CC8 sll v0,v0,1 ; 80133CCC addu ; 80133CD0 lhu v1,(v0)
    c->mem_w16(obj + 0x60, (uint16_t)(int16_t)-0xc8);// 80133CD4 v0=-0xc8 ; 80133CD8 sh v0,0x60(s1)
    c->mem_w16(obj + 0x32, tv);              // 80133CE0 sh v1,0x32(s1)  (delay slot of j epilogue)
    return;                                  // 80133CDC j 0x80133D54 (epilogue)
  }

  // ---- STATE 1 (80133CE4..80133D48): entry helper, then global gate ----
  c->r[4] = obj; rec_dispatch(c, 0x801337E4u);        // 80133CE4 jal 0x801337e4 (a0=s1)
  uint8_t g = c->mem_r8(0x800E7EAAu);                 // 80133CF0 lbu v1,0x7eaa(800e) -> 0x800E7EAA
  if (g < 0x1c) {                                     // 80133CF8 sltiu v0,v1,0x1c ; 80133CFC bnez -> 0x80133D20
    // 80133D20 j epilogue ; delay slot 80133D24 sh s0,0x62(s1)  (s0 = state byte = 1)
    c->mem_w16(obj + 0x62, (uint16_t)st);
    return;
  }
  if (g == 0x22) {                                    // 80133D04 beq v1,0x22 -> 0x80133D28
    c->mem_w8(obj + 1, st);                  // 80133D28 sb s0,1(s1)  (s0 = state = 1)
    c->r[4] = obj; rec_dispatch(c, 0x80077EBCu);      // 80133D2C jal 0x80077ebc (a0=s1)
    // fall into render tail (80133D34)
  } else {
    int16_t a1v = (int16_t)c->mem_r16(obj + 0x60);    // 80133D0C lh a1,0x60(s1) (sign-extend)
    c->r[4] = obj; c->r[5] = (uint32_t)(int32_t)a1v;
    rec_dispatch(c, 0x800778E4u);                     // 80133D10 jal 0x800778e4 (a0=s1, a1=node[0x60])
    if (c->r[2] == 0) {                               // 80133D18 bnez v0 -> render tail ; else fall to 0x80133D20
      // 80133D20 j epilogue ; delay slot 80133D24 sh s0,0x62(s1)
      c->mem_w16(obj + 0x62, (uint16_t)st);
      return;
    }
    // v0 != 0 -> render tail (80133D34)
  }

  // ---- render tail (80133D34..80133D44) ----
  c->r[4] = obj; rec_dispatch(c, 0x8004766Cu);        // 80133D34 jal 0x8004766c (a0=s1)
  c->r[4] = obj; rec_dispatch(c, 0x800517F8u);        // 80133D3C jal 0x800517f8 (a0=s1)
  // 80133D44 j epilogue
}

void ov_beh_typed_table_seed_gate(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("typed_table_seed_gateverify") ? 1 : 0;
  if (!s_v) { beh_typed_table_seed_gate(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_typed_table_seed_gate(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[typed_table_seed_gateverify] MISMATCH obj=%08x st=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[typed_table_seed_gateverify] %ld matches\n", ng);
}

}  // namespace

void beh_typed_table_seed_gate_register(void) {
}

// Exported entry — the verify wrapper ov_beh_typed_table_seed_gate is in the anonymous namespace above (internal
// linkage); the engine's per-object dispatch (engine_tomba2.cpp call_handler) calls THIS to run the
// owned behavior.
void ov_beh_typed_table_seed_gate_run(Core* c) { ov_beh_typed_table_seed_gate(c); }
