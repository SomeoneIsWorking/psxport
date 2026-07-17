// game/ai/beh_typed_init_exit_poker.cpp — PC-native per-object BEHAVIOR handler FUN_80118240.
//
// Overlay handler (~x1556/field-frame on seaside; ~370 instr, the biggest of the hot set), prologue
// 0x80118240; `jr ra` at 0x80118844. Disassembled from scratch/ram/field_seaside.bin. Outer state
// machine on node[4]; each state sub-dispatches on node[3] (and node[5]/node[0x5E] inner sub-states):
//   STATE 0 : INIT per node[3] in {0,1,2,3} — each seeds node block + node[0x80..0x86] sizes via
//             FUN_80077B38 (model-attach); node[3] 0/3 also call FUN_8004B354; then node[4] += 1.
//   STATE 1 : per node[3]; the heavy state — drives node[5]/node[0x5E] sub-states, FUN_80077EFC,
//             FUN_8004BD64 (5-arg, arg5 = &node[0x60] on the STACK), and a shared block that calls
//             FUN_80051D90 to fill scratchpad 0x1F8000C0 then copies node[0x2E]/0x32/0x36 from it +
//             FUN_8004B374. Several paths gate on area bytes (0x800BFAF9) / advance node[4]=3.
//   STATE 2 : per node[3] — runs FUN_8004D4C4(id,1)+FUN_8004B0D8 "exit" tails, sets node[4]=3, and
//             pokes area-flag bytes (0x800BF9DF |= 0x20; 0x800BF9EA clears bits at node[0x60]&31 and
//             (node[0x60]+4)&31; 0x800BF9EE |= 2); node[3]==0 first probes FUN_80040B48(5).
//   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/area-flag WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). Guest a0..a3 + the
// ONE stack arg (sp+16 for FUN_8004BD64) are set exactly as the guest does — we mirror the guest
// prologue (sp -= 48) so the leaf reads arg5 from the same frame slot. Init data words live in resident
// RAM (read live). Transcribed 1:1 as a register machine; the byte-exact A/B gate (full RAM+scratchpad
// vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_set_geom
#include "inventory.h"       // class Inventory — inv(c).giveAndFlag (FUN_8004D4C4)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN  = 0x80118240u;
constexpr uint32_t A1_M0   = 0x8014C808u;   // FUN_80077B38 arg (lui 0x8015 + addiu -14328)
constexpr uint32_t A1_M2   = 0x80017334u;   // FUN_80077B38 arg for node[3]==2 (lui 0x8001 + addiu 29492)

// Shared block @0x80118690: FUN_80051D90(node[0x10], a1_buf, 0x1F8000C0) writes the scratchpad work
// area, then node[0x2E]/0x32/0x36 are copied from 0x1F8000C0/C2/C4 and FUN_8004B374(node,0) is called.
static void shared_8690(Core* c, uint32_t nd, uint32_t a1_buf) {
  guest_leaf(c, 0x80051D90u, c->mem_r32(nd + 0x10), a1_buf, 0x1F8000C0u);
  c->mem_w16(nd + 0x2E, c->mem_r16(0x1F8000C0u));
  c->mem_w16(nd + 0x32, c->mem_r16(0x1F8000C2u));
  c->mem_w16(nd + 0x36, c->mem_r16(0x1F8000C4u));
  guest_leaf(c, 0x8004B374u, nd, 0);
}

}  // namespace

void beh_typed_init_exit_poker(Core* c) {
  uint32_t nd = c->r[4];                             // s2 = a0 (node)
  uint32_t entry_sp = c->r[29];
  c->r[29] = entry_sp - 48;                          // mirror the guest prologue frame

  uint32_t st = c->mem_r8(nd + 4);                   // node[4] = outer state
  uint8_t n3, n5, n5e, v0b;

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto S2;
  if (st == 3) { eng(c).spawn.despawn(nd); goto Lret; }   // STATE 3
  goto Lret;                                         // st >= 4

 // ================= STATE 0 (INIT) =================
 S0:
  n3 = c->mem_r8(nd + 3);
  switch (n3) {
    case 0:                                          // sub0 @0x801182e0
      c->mem_w8 (nd + 11, 16);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w8 (nd + 13, 0);
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      (c->r[4]=nd, c->r[5]=A1_M0, c->r[6]=15, eng(c).graphicsBind.setGeom());
      c->mem_w16(nd + 0x80, 20);
      c->mem_w16(nd + 0x82, 40);
      c->mem_w16(nd + 0x84, 20);
      c->mem_w16(nd + 0x86, 40);
      goto L8450;                                    // -> FUN_8004B354(node,0); node[4]+=1
    case 1:                                          // sub1 @0x80118334
      c->mem_w8 (nd + 11, 16);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w8 (nd + 13, 0);
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      (c->r[4]=nd, c->r[5]=A1_M0, c->r[6]=0, eng(c).graphicsBind.setGeom());
      c->mem_w16(nd + 0x2E, 17260);
      c->mem_w16(nd + 0x32, (uint16_t)(int16_t)-1900);
      c->mem_w16(nd + 0x36, 11200);
      goto L83d0;                                    // shared size block -> node[4]+=1
    case 2:                                          // sub2 @0x80118388
      c->mem_w8 (nd + 11, 18);
      c->mem_w16(nd + 0x56, (uint16_t)(int16_t)-384);
      c->mem_w16(nd + 0x58, 1792);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w8 (nd + 13, 0);
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF58u));
      (c->r[4]=nd, c->r[5]=A1_M2, c->r[6]=2, eng(c).graphicsBind.setGeom());
      goto L83d0;                                    // shared size block -> node[4]+=1
    case 3:                                          // sub3 @0x801183f4
      c->mem_w8 (nd + 11, 16);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w8 (nd + 13, 0);
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      (c->r[4]=nd, c->r[5]=A1_M0, c->r[6]=26, eng(c).graphicsBind.setGeom());
      c->mem_w16(nd + 0x80, 32);
      c->mem_w16(nd + 0x82, 64);
      c->mem_w16(nd + 0x84, 20);
      c->mem_w8 (nd + 0, 1);                          // node[0] = s1
      c->mem_w16(nd + 0x86, 40);
      goto L8450;                                    // -> FUN_8004B354(node,0); node[4]+=1
    default:
      goto L8458;                                    // node[3] >= 4: just node[4] += 1
  }

 L83d0:                                              // 0x801183d0 size block (sub1/sub2)
  c->mem_w16(nd + 0x80, 30);
  c->mem_w16(nd + 0x82, 40);
  c->mem_w16(nd + 0x84, 16);
  c->mem_w16(nd + 0x86, 32);
  goto L8458;
 L8450:
  guest_leaf(c, 0x8004B354u, nd, 0);                   // FUN_8004B354(node, 0)
 L8458:
  c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1)); // node[4] += 1
  goto Lret;

 // ================= STATE 1 =================
 S1:
  n3 = c->mem_r8(nd + 3);
  switch (n3) {
    case 0: goto S1_0;
    case 1: goto S1_1;
    case 2: goto S1_2;
    case 3: goto S1_3;
    default: goto Lret;
  }

 S1_0: {                                             // node[3]==0 @0x801184b0
    n5 = c->mem_r8(nd + 5);
    if (n5 == 0) {                                   // 0x801184d0
      c->mem_w8 (nd + 5, 1);
      c->mem_w16(nd + 0x60, (uint16_t)(int16_t)-60);
      c->mem_w16(nd + 0x62, 0);
      c->mem_w16(nd + 0x64, 0);
    } else if (n5 != 1) goto Lret;                   // n5 >= 2
    // 0x801184e4 (n5==0 fall-through, or n5==1)
    if (c->mem_r8(c->mem_r32(nd + 0x10) + 0x3F) == 0) goto Lret;
    guest_leaf(c, 0x80077EFCu, nd);                   // FUN_80077EFC(node)
    c->mem_w8(nd + 1, 1);                            // node[1] = s1
    {
      int16_t v = c->mem_r16s(c->mem_r32(nd + 0x10) + 0x16);
      if (v != 2) c->mem_w8(nd + 0, 1);              // node[0] = s1
      else        c->mem_w8(nd + 0, (uint8_t)v);     // node[0] = v (==2)
    }
    shared_8690(c, nd, nd + 0x60);                   // a1 = &node[0x60]
    goto Lret;
  }

 S1_1: {                                             // node[3]==1 @0x80118530
    n5e = c->mem_r8(nd + 0x5E);
    if (n5e == 0) {                                  // 0x80118554
      uint8_t v = c->mem_r8(nd + 5);
      if (v == 0) { c->mem_w8(nd + 5, 1); c->mem_w8(nd + 0, 1); goto Lret; }   // 0x80118574
      if (v == 1) goto L185e8;
      goto Lret;                                     // v >= 2
    }
    if (n5e == 1) {                                  // 0x80118580
      uint8_t v = c->mem_r8(nd + 5);
      if (v == 0) {                                  // 0x801185a0
        c->mem_w8 (nd + 5, 1);
        c->mem_w8 (nd + 0x47, 1);
        c->mem_w16(nd + 0x60, 0);
        c->mem_w16(nd + 0x62, 0);
        c->mem_w16(nd + 0x64, 0);
        c->mem_w8 (nd + 8, 240);
        goto Lret;
      }
      if (v == 1) {                                  // 0x801185c0
        if (c->mem_r8(0x800BFAF9u) == 0) { c->mem_w8(nd + 4, 3); goto Lret; }  // 0x80118750
        c->r[4]=nd; c->r[5]=1; c->r[6]=c->mem_r32(0x800E7F5Cu); c->r[7]=0;
        c->mem_w32(c->r[29] + 16, nd + 0x60); eng(c).graphicsBind.posCompose();   // FUN_8004BD64 — native
        goto L185e8;
      }
      goto Lret;                                     // v >= 2
    }
    goto Lret;                                       // n5e >= 2
  }
 L185e8:
  guest_leaf(c, 0x8007778Cu, nd);                      // FUN_8007778C(node)
  guest_leaf(c, 0x80077B5Cu, nd);                      // FUN_80077B5C(node)
  goto Lret;

 S1_2: {                                             // node[3]==2 @0x80118600
    n5 = c->mem_r8(nd + 5);
    if (n5 == 0) {                                   // 0x80118620
      c->mem_w16(nd + 0x64, 54);
      c->mem_w8 (nd + 5, 1);
      c->mem_w16(nd + 0x66, 80);
      c->mem_w16(nd + 0x68, 0);                       // delay-slot store: runs BEFORE the call
      guest_leaf(c, 0x8004B354u, nd, 0);               // FUN_8004B354(node, 0)
      goto Lret;
    }
    if (n5 == 1) {                                   // 0x80118648
      if (c->mem_r8(c->mem_r32(nd + 0x10) + 0x3F) == 0) goto Lret;
      guest_leaf(c, 0x80077EFCu, nd);                  // FUN_80077EFC(node)
      c->mem_w8(nd + 1, 1);                           // node[1] = s1
      {
        int16_t v = c->mem_r16s(c->mem_r32(nd + 0x10) + 0x16);
        if (v != 2) c->mem_w8(nd + 0, (uint8_t)v);    // node[0] = v   (s0 == node[3] == 2)
        else        c->mem_w8(nd + 0, 1);             // node[0] = s1
      }
      shared_8690(c, nd, nd + 0x64);                  // a1 = &node[0x64]
      goto Lret;
    }
    goto Lret;                                        // n5 >= 2
  }

 S1_3:                                               // node[3]==3 @0x801186c8
  guest_leaf(c, 0x8007778Cu, nd);                      // FUN_8007778C(node)
  if (c->r[2] == 0) goto Lret;                        // return value
  guest_leaf(c, 0x8004B374u, nd, 0);                   // FUN_8004B374(node, 0)
  goto Lret;

 // ================= STATE 2 =================
 S2:
  n3 = c->mem_r8(nd + 3);
  switch (n3) {
    case 0:                                          // st2sub0 @0x8011872c
      // FUN_80040B48 = SceneEvents::arm; caller advances only when events are enabled (r[2] >= 0).
      if (eng(c).sceneEvents.arm(5) >= 0) {
        inv(c).giveAndFlag(36, 1);              // FUN_8004D4C4(36, 1) [native]
        guest_leaf(c, 0x8004B0D8u, nd);                // FUN_8004B0D8(node)
      }
      c->mem_w8(nd + 4, 3);                           // node[4] = 3
      goto Lret;
    case 1:                                          // st2sub1 @0x8011875c
      inv(c).giveAndFlag(69, 1);                // FUN_8004D4C4(69, 1) [native]
      guest_leaf(c, 0x8004B0D8u, nd);                  // FUN_8004B0D8(node)
      c->mem_w8(nd + 4, 3);
      c->mem_w8(0x800BF9DFu, (uint8_t)(c->mem_r8(0x800BF9DFu) | 0x20));
      goto Lret;
    case 2: {                                        // st2sub_v1 @0x80118794
      inv(c).giveAndFlag(120, 1);               // FUN_8004D4C4(120, 1) [native]
      c->mem_w8(nd + 4, 3);                           // node[4] = 3 (delay slot)
      guest_leaf(c, 0x8004B0D8u, nd);                  // FUN_8004B0D8(node)
      uint8_t flg = c->mem_r8(0x800BF9EAu);
      int16_t p   = c->mem_r16s(nd + 0x60);
      flg = (uint8_t)(flg & ~(1u << ((uint32_t)p & 31)));
      c->mem_w8(0x800BF9EAu, flg);
      flg = (uint8_t)(flg & ~(1u << (((uint32_t)p + 4) & 31)));
      c->mem_w8(0x800BF9EAu, flg);
      guest_leaf(c, 0x80040C00u, 78);                  // FUN_80040C00(78)
      goto Lret;
    }
    case 3:                                          // st2sub3 @0x801187f8
      inv(c).giveAndFlag(83, 1);                // FUN_8004D4C4(83, 1) [native]
      guest_leaf(c, 0x8004B0D8u, nd);                  // FUN_8004B0D8(node)
      c->mem_w8(nd + 4, 3);                           // node[4] = s0 (==3)
      c->mem_w8(0x800BF9EEu, (uint8_t)(c->mem_r8(0x800BF9EEu) | 2));
      goto Lret;
    default:
      goto Lret;                                      // node[3] >= 4
  }

 Lret:
  c->r[29] = entry_sp;                                // restore caller sp
  return;
}
