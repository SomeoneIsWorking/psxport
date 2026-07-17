// game/ai/beh_scatter_ramp_machine.cpp — PC-native per-object BEHAVIOR handler FUN_8013C9C0.
//
// 3rd-hottest still-PSX OVERLAY handler (~x4302/field-frame on seaside; ~190 instr), an area-overlay
// routine (prologue 0x8013C9C0; `jr ra` at 0x8013CDCC). Lives only at runtime in the area overlay
// (NOT in MAIN.EXE) — disassembled from scratch/ram/field_seaside.bin, including its two in-overlay
// jump tables (jt0 @0x8010A000 [11 entries, node[5]] and jt1 @0x8010A030 [10 entries, node[5]-1]).
//
// Two-level state machine. OUTER state = node[4]:
//   STATE >=4 : nothing (exit).   STATE 2/3 : FUN_8007A624(node), exit.
//   STATE 0   : seed from two in-overlay tables (word table @0x80109FC4 by node[3]-1 -> node[0x50];
//               stride-6 table @0x80109FD6 by node[3] -> node[0x2c]/0x2e/0x30), set node[4..7]=0x00010001,
//               node[0x32]=-70, node[0x3c]=0x80109FC0, node[0x48]=node[0x4c]=0; pick node[5] (7 or 0) and
//               byte@0x80109FC0 (8 or 16) on 0x800BF9E0<6; then FALL INTO state 1.
//   STATE 1   : area early-outs (byte 0x800E7FEB==1, 0x800BF9E0>=20 -> node[4]=3, byte 0x800BF816==1,
//               byte 0x800E7EAA>=17); a node[6]/node[7] countdown timer; then dispatch jt0[node[5]] — an
//               11-way INNER sub-state machine (animation/scatter ramps: walks a node[0x56] counter up/down,
//               flips node[0x54] timers, calls FUN_80074590 twice for an effect, toggles 0x800BF9E0 /
//               0x80109FC0 area bytes). TAIL: for node[3] in {1,3} dispatch jt1[node[5]-1] to call
//               FUN_8002B278(node) for some sub-states, else exit.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node/global/overlay memory WRITES owned
// native; the sub-behavior CALLs (FUN_80074590, FUN_8002B278, FUN_8007A624) stay reachable via rec_dispatch
// (pure-PSX leaf). NO GTE, NO render packets. Transcribed 1:1 as a register machine (locals = guest regs,
// goto labels = guest addresses); the two guest jump tables become switch->goto. Delay-slot effects kept
// exact (e.g. the c9f0 `lui a0` then `ori a0,a0,1` forming the 0x00010001 state word; the cc14/cd44
// `sll v0,24; bgez` signed-byte timer tests). The byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. a0/a1/a2 written into c->r only for the leaf calls (= the guest
// writes there). v0 (handler return) is NOT reproduced.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::coneCull2b278 (FUN_8002B278)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013C9C0u;

}  // namespace

void beh_scatter_ramp_machine(Core* c) {
  uint32_t obj = c->r[4];                        // s0 = a0 (node)
  uint32_t a3 = obj + 0x50;                      // a3 = node+0x50 (c9dc, constant)
  uint32_t v0, v1;

  uint8_t st = c->mem_r8(obj + 4);               // node[4] = outer state
  if (st == 1) goto Lcabc;
  if (st < 2) { if (st == 0) goto Lca10; goto Lret; }  // st<2 -> only st==0 reachable
  if (st < 4) goto Lcdbc;                              // st in {2,3}
  goto Lret;                                           // st >= 4 default

 Lcdbc:                                          // STATE 2/3 — FUN_8007A624(node)
  eng(c).spawn.despawn(obj);
  goto Lret;

 Lca10:                                          // STATE 0 — seed from the overlay tables
  {
    uint8_t n3 = c->mem_r8(obj + 3);
    v1 = c->mem_r32(0x80109FC4u + ((uint32_t)(n3 - 1) << 2));   // word table[node[3]-1]
    c->mem_w32(obj + 4, 0x00010001u);            // sw a0=0x00010001 -> node[4]=1,5=0,6=1,7=0
    c->mem_w32(a3 + 0, v1);                       // node[0x50] = tableword
    uint32_t t1 = 0x80109FD6u + (uint32_t)(n3 * 6);            // stride-6 table[node[3]]
    c->mem_w16(obj + 0x2c, c->mem_r16(t1 + 0));
    c->mem_w16(obj + 0x2e, c->mem_r16(t1 + 2));
    v1 = c->mem_r16(t1 + 4);
    c->mem_w32(obj + 0x3c, 0x80109FC0u);
    c->mem_w16(obj + 0x32, (uint16_t)(int16_t)-70);
    c->mem_w32(obj + 0x48, 0);
    c->mem_w16(obj + 0x4c, 0);
    c->mem_w16(obj + 0x30, (uint16_t)v1);
  }
  if (c->mem_r8(0x800BF9E0u) < 6) {              // val < 6
    c->mem_w8(obj + 5, 0);
    c->mem_w8(0x80109FC0u, 16);
  } else {                                       // val >= 6
    c->mem_w8(obj + 5, 7);
    c->mem_w8(0x80109FC0u, 8);
  }
  // fall through to STATE 1

 Lcabc:                                          // STATE 1
  if (c->mem_r8(0x800E7FEBu) == 1) goto Lret;
  if (!(c->mem_r8(0x800BF9E0u) < 20)) { c->mem_w8(obj + 4, 3); goto Lret; }   // >= 20 -> node[4]=3
  if (c->mem_r8(0x800BF816u) == 1) goto Lret;
  if (!(c->mem_r8(0x800E7EAAu) < 17)) goto Lret;                              // >= 17 exit
  // node[6]/node[7] countdown timer
  v0 = (uint8_t)(c->mem_r8(obj + 6) - 1);
  c->mem_w8(obj + 6, (uint8_t)v0);
  if ((v0 & 0xff) == 0) {
    c->mem_w8(obj + 6, 3);
    v0 = (uint8_t)(c->mem_r8(obj + 7) + 1);
    c->mem_w8(obj + 7, (uint8_t)v0);
    if (!((v0 & 0xff) < 6)) c->mem_w8(obj + 7, 0);
  }
  {
    uint8_t n5 = c->mem_r8(obj + 5);             // INNER sub-state
    if (!(n5 < 11)) goto Lcd60;
    switch (n5) {
      case 0:  goto Lcb88; case 1:  goto Lcb94; case 2:  goto Lcbc8; case 3:  goto Lcbe4;
      case 4:  goto Lcc04; case 5:  goto Lcc30; case 6:  goto Lcc6c; case 7:  goto Lcc98;
      case 8:  goto Lcce4; case 9:  goto Lcd20; default: goto Lcd58;   // 10
    }
  }

 Lcb88:                                          // sub-state 0
  c->mem_w8(obj + 5, 1);
  c->mem_w16(a3 + 6, 0);                          // node[0x56] = 0
  // fall into sub-state 1
 Lcb94:                                          // sub-state 1
  if (c->mem_r8(0x800BF9E0u) < 6) goto Lcd60;     // val < 6
  c->mem_w8(a3 + 4, 24);                          // node[0x54] = 24
  c->mem_w8(obj + 5, 2);
  goto Lccc4;                                      // (a0/a1/a2 = 7,0,0 set in Lccc4)

 Lcbc8:                                          // sub-state 2
  if (c->mem_r16s(a3 + 6) < -199) { c->mem_w8(obj + 5, 3); goto Lcc24; }
  goto Lcc44;

 Lcbe4:                                          // sub-state 3
  if (c->mem_r16s(a3 + 6) < 0) goto Lcc7c;
  c->mem_w8(obj + 5, 4);
  c->mem_w8(a3 + 4, 96);                          // node[0x54] = 96
  goto Lcd60;

 Lcc04:                                          // sub-state 4
  {
    uint8_t nv = (uint8_t)(c->mem_r8(a3 + 4) - 1);
    c->mem_w8(a3 + 4, nv);
    if ((int8_t)nv >= 0) goto Lcd60;
    c->mem_w8(obj + 5, 5);
  }
  // fall into Lcc24
 Lcc24:
  c->mem_w8(a3 + 4, 24);                          // node[0x54] = 24
  goto Lcd60;

 Lcc30:                                          // sub-state 5
  if (c->mem_r16s(a3 + 6) < -199) {
    c->mem_w8(obj + 5, 6);
    c->mem_w8(a3 + 4, 24);
    c->mem_w8(0x80109FC0u, 8);
    goto Lcd60;
  }
  // fall into Lcc44
 Lcc44:
  v1 = c->mem_r16(a3 + 6);
  c->mem_w16(a3 + 6, (uint16_t)(v1 - 6));         // node[0x56] -= 6
  goto Lcd60;

 Lcc6c:                                          // sub-state 6
  if (c->mem_r16s(a3 + 6) >= 0) {
    c->mem_w8(0x800BF9E0u, 7);
    c->mem_w8(obj + 5, 7);
    goto Lcd60;
  }
  // fall into Lcc7c
 Lcc7c:
  v1 = c->mem_r16(a3 + 6);
  c->mem_w16(a3 + 6, (uint16_t)(v1 + 6));         // node[0x56] += 6
  goto Lcd60;

 Lcc98:                                          // sub-state 7
  if (c->mem_r8(0x800BF9E0u) != 16) goto Lcd60;
  c->mem_w8(obj + 5, 8);
  c->mem_w8(a3 + 4, 30);                          // node[0x54] = 30
  // fall into Lccc4
 Lccc4:
  eng(c).sfx.trigger(7, 0, 0);     // FUN_80074590 (native)
  eng(c).sfx.trigger(148, 0, 0);   // FUN_80074590 (native; id 148 → path A per-area)
  goto Lcd60;

 Lcce4:                                          // sub-state 8
  if (c->mem_r16s(a3 + 6) < -199) {
    c->mem_w8(0x800BF9E0u, 17);
    c->mem_w8(obj + 5, 9);
    c->mem_w8(a3 + 4, 120);                       // node[0x54] = 120
    goto Lcd60;
  }
  v1 = c->mem_r16(a3 + 6);
  c->mem_w16(a3 + 6, (uint16_t)(v1 - 3));         // node[0x56] -= 3
  goto Lcd60;

 Lcd20:                                          // sub-state 9
  if (c->mem_r16s(a3 + 6) < 0)
    c->mem_w16(a3 + 6, (uint16_t)(c->mem_r16(a3 + 6) + 3));   // node[0x56] += 3
  {
    uint8_t nv = (uint8_t)(c->mem_r8(a3 + 4) - 1);
    c->mem_w8(a3 + 4, nv);
    if ((int8_t)nv >= 0) goto Lcd60;
    c->mem_w8(obj + 5, 10);
  }
  goto Lcd60;

 Lcd58:                                          // sub-state 10
  c->mem_w8(obj + 4, 3);
  // fall into Lcd60

 Lcd60:                                          // TAIL
  {
    uint8_t n3 = c->mem_r8(obj + 3);
    if (n3 != 1 && n3 != 3) goto Lcdac;
    uint8_t n5 = c->mem_r8(obj + 5);
    uint32_t idx = (uint8_t)(n5 - 1);
    if (!(idx < 10)) goto Lcdac;
    // jt1 @0x8010A030: {0,1,5,6,7}->Lcdac (call), {2,3,4,8,9}->exit
    switch (idx) {
      case 2: case 3: case 4: case 8: case 9: goto Lret;
      default: goto Lcdac;
    }
  }
 Lcdac:
  c->r[4] = obj; eng(c).cull.coneCull2b278();     // FUN_8002B278 (native)
 Lret:
  return;
}
