// game/ai/beh_typed_variant_router.cpp — PC-native per-object BEHAVIOR handler FUN_8011C164.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8011C164
// (scratch/decomp/field2/8011c164.c), cross-checked against the raw disas (tools/disas.py). The biggest
// of the per-object handlers: a TYPE-routed actor. node[3] is the variant/type byte; node[4] the outer
// state.
//
//   STATE 0 (INIT): seed node[3c]=*PTR(0x800ecf80), node[d]=0, node[5c]=0, node[b]=0x10, node[0x47]=0,
//                   node[5a]=0; FUN_80077b38(node,0x8014c808,8); seed box node[80..86]=0x32/100/0x46/100,
//                   node[0x29]=0, node[60]=node[62]=0; then by TYPE node[3]:
//                     type<2  -> node[0]=1; copy 8-byte table entry @0x80148914+type*8 into
//                               node[2e]/[32]/[36] (16b) + node[2a] (8b).
//                     type==3 -> if mem8(0x800bf8bc)==0xFF: node[4]=3.   (else fall through, no-op)
//                     else    -> no-op.  node[4]:=1 always happens up front.
//   STATE 1 (ACTIVE): switch(node[3]) 0..0x14 routing to per-type sub-behaviors. Cases 0 & 1 are
//                     identical: a global gate (mem8(0x800e7eaa) >= 26 && FUN_80077870(node)!=0) then a
//                     PRNG-driven (FUN_8009a450) substate machine on node[6] (appear / bc3c / spawn-burst
//                     via FUN_80077b38 + node[0x40] countdown). Every STATE-1 exit sets node[0x29]=0.
//   STATE 2 (ENTER): FUN_80077e7c; node[1]=1; substate node[5]: ==1 -> arm node[6]/node[5a] +
//                    FUN_80077b38(node,0x8014c808,0xc); ==0 or ==2 -> FUN_8011c090.
//   STATE 3 (EXIT) : FUN_8007a624(node).
//
// CONTROL FLOW + the direct node/global WRITES owned native; every sub-behavior CALL stays a pure-PSX
// leaf via rec_dispatch. case-2's FUN_8004bd64 takes a 5th (stacked) arg pointing at an on-stack
// {0xfffe,0,0xffea} short array: reproduced by mirroring the recomp frame (sp-0x30) into the guest stack
// below entry sp (inside the gate's excluded [sp-0x800,sp) window) and dispatching with that frame sp.
// Byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_set_geom
#include "rng.h"       // class Rng (via rngOf(c).next())
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8011C164u;

static inline void     leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t prng(Core* c) { return (uint32_t)rngOf(c).next(); }   // FUN_8009A450 -> class Rng

// FUN_80077b38(node, 0x8014c808, n)
static inline void call_77b38(Core* c, uint32_t nd, uint32_t n) {
  c->r[4] = nd; c->r[5] = 0x8014C808u; c->r[6] = n; eng(c).graphicsBind.setGeom();
}

// STATE 1, cases 0 & 1 (identical): PRNG-driven substate machine on node[6]. Always ends node[0x29]=0.
static void state1_appear(Core* c, uint32_t nd) {
  if (c->mem_r8(0x800E7EAAu) < 26) { c->mem_w8(nd + 0x29, 0); return; }   // sltiu <26
  if (leafr1(c, nd, 0x80077870u) == 0) { c->mem_w8(nd + 0x29, 0); return; }
  uint8_t sub = c->mem_r8(nd + 6);
  if (sub == 1) {
    leaf1(c, nd, 0x8011BC3Cu);                                           // FUN_8011bc3c
  } else if (sub == 0) {
    uint32_t r = prng(c) & 7;
    if (r == 0) c->mem_w8(nd + 6, 2);
    else if (r < 5) c->mem_w8(nd + 6, 1);
    c->mem_w8(nd + 7, 0);                                                // break -> param_1[7]=0
  } else if (sub == 2) {
    leaf1(c, nd, 0x80077B5Cu);                                           // FUN_80077b5c
    uint8_t s7 = c->mem_r8(nd + 7);
    if (s7 == 0) {
      call_77b38(c, nd, 7);                                              // FUN_80077b38(node,...,7)
      uint32_t r = prng(c) & 0x1f;
      c->mem_w16(nd + 0x40, (uint16_t)(r + 0x20));
      c->mem_w8(nd + 7, (uint8_t)(c->mem_r8(nd + 7) + 1));
    } else if (s7 == 1) {
      uint16_t v = (uint16_t)(c->mem_r16(nd + 0x40) - 1);
      c->mem_w16(nd + 0x40, v);
      if (v == 0) { c->mem_w8(nd + 6, 0); c->mem_w8(nd + 7, 0); }        // sVar2==1 -> node[6]=0, node[7]=0
    }
    // s7 not in {0,1}: straight to node[0x29]=0
  }
  // sub not in {0,1,2}: straight to node[0x29]=0
  c->mem_w8(nd + 0x29, 0);
}

}  // namespace

void beh_typed_variant_router(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    // ---- STATE 1: type switch on node[3] ----
    uint8_t type = c->mem_r8(nd + 3);
    if (type >= 21) { c->mem_w8(nd + 0x29, 0); return; }                 // default
    switch (type) {
      case 0:
      case 1:
        state1_appear(c, nd);
        return;
      case 2: {
        // FUN_8004bd64(node, 1, *(node[0x10]+0xdc), same, &{0xfffe,0,0xffea}). 5th arg is stacked at
        // sp+0x10; the local array at sp+0x18..0x1c. Mirror the recomp frame (sp-0x30) below entry sp.
        uint32_t fsp = c->r[29] - 0x30;
        c->mem_w16(fsp + 0x18, 0xfffe);
        c->mem_w16(fsp + 0x1a, 0x0000);
        c->mem_w16(fsp + 0x1c, 0xffea);
        c->mem_w32(fsp + 0x10, fsp + 0x18);                             // stacked a4 = &array
        uint32_t v1 = c->mem_r32(nd + 0x10);
        uint32_t a2 = c->mem_r32(v1 + 0xdc);
        c->r[4] = nd; c->r[5] = 1; c->r[6] = a2; c->r[7] = a2;
        uint32_t save_sp = c->r[29]; c->r[29] = fsp;
        eng(c).graphicsBind.posCompose();                                           // FUN_8004bd64
        c->r[29] = save_sp;
        leaf1(c, nd, 0x80077B5Cu);                                       // FUN_80077b5c
        c->mem_w8(nd + 1, 1);
        leaf1(c, nd, 0x80077E7Cu);                                       // FUN_80077e7c
        c->mem_w8(nd + 0x29, 0);
        return;
      }
      case 3: case 7: case 8: case 9:
        leaf1(c, nd, 0x8011B324u);                                       // FUN_8011b324
        leaf1(c, nd, 0x80077B5Cu);                                       // FUN_80077b5c
        c->mem_w8(nd + 1, 1);
        leaf1(c, nd, 0x80077E7Cu);                                       // FUN_80077e7c
        c->mem_w8(nd + 0x29, 0);
        return;
      case 4: case 5: case 6:
        leaf1(c, nd, 0x8011B738u);                                       // FUN_8011b738
        c->mem_w8(nd + 0x29, 0);
        return;
      case 10: case 11: case 12: case 13:
        leaf1(c, nd, 0x8011ADA8u);                                       // FUN_8011ada8
        c->mem_w8(nd + 0x29, 0);
        return;
      case 0x14:
        if (leafr1(c, nd, 0x8007778Cu) != 0) leaf1(c, nd, 0x8011BF04u);  // FUN_8007778c / FUN_8011bf04
        c->mem_w8(nd + 0x29, 0);
        return;
      default:
        c->mem_w8(nd + 0x29, 0);
        return;
    }
  }

  if (st == 0) {
    // ---- STATE 0: INIT ----
    c->mem_w8(nd + 4, 1);
    uint32_t p = c->mem_r32(0x800ECF80u);                                // *PTR_DAT_800ecf80 (contents)
    c->mem_w8(nd + 0x0d, 0);
    c->mem_w16(nd + 0x5c, 0);
    c->mem_w8(nd + 0x0b, 0x10);
    c->mem_w8(nd + 0x47, 0);
    c->mem_w16(nd + 0x5a, 0);
    c->mem_w32(nd + 0x3c, p);
    call_77b38(c, nd, 8);                                                // FUN_80077b38(node,...,8)
    c->mem_w16(nd + 0x80, 0x32);
    c->mem_w16(nd + 0x82, 100);
    c->mem_w16(nd + 0x84, 0x46);
    c->mem_w16(nd + 0x86, 100);
    c->mem_w8(nd + 0x29, 0);
    c->mem_w16(nd + 0x60, 0);
    c->mem_w16(nd + 0x62, 0);
    uint8_t type = c->mem_r8(nd + 3);
    if (type < 2) {
      uint32_t e = 0x80148914u + (uint32_t)type * 8;
      c->mem_w8(nd + 0, 1);
      c->mem_w16(nd + 0x2e, c->mem_r16(e + 0));
      c->mem_w16(nd + 0x32, c->mem_r16(e + 2));
      c->mem_w16(nd + 0x36, c->mem_r16(e + 4));
      c->mem_w8(nd + 0x2a, c->mem_r8(e + 6));
    } else if (type == 3) {
      if (c->mem_r8(0x800BF8BCu) == 0xff) c->mem_w8(nd + 4, type);       // lbu == 255 (Ghidra's "==-1")
    }
    return;
  }

  if (st == 2) {
    // ---- STATE 2: ENTER ----
    leaf1(c, nd, 0x80077E7Cu);                                           // FUN_80077e7c
    c->mem_w8(nd + 1, 1);
    uint8_t sub = c->mem_r8(nd + 5);
    if (sub == 1) {
      if (c->mem_r8(nd + 6) != 0) return;
      c->mem_w8(nd + 6, sub);                                            // node[6] = node[5] (==1)
      c->mem_w16(nd + 0x5a, 0);
      call_77b38(c, nd, 0xc);                                            // FUN_80077b38(node,...,0xc)
    } else if (sub == 0 || sub == 2) {
      leaf1(c, nd, 0x8011C090u);                                        // FUN_8011c090
    }
    return;
  }

  if (st == 3) {
    eng(c).spawn.despawn(nd);                                           // FUN_8007a624
  }
}
