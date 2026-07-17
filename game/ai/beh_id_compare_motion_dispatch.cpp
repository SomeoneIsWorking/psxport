// game/ai/beh_id_compare_motion_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80145230 (OVERLAY).
//
// An OVERLAY per-object behavior routine (guest 0x8014xxxx) with the same state-byte shape as the
// resident/overlay siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler): a state machine on the
// node's state byte node[4] (0 init / 1 active / 2 idle / 3 despawn). The bulk of the work lives in
// ~14 sub-functions it CALLS, which stay PSX leaves via rec_dispatch:
//   state 0  -> FUN_80140544                          (per-type init; then -> epilogue)
//   state 1  -> a despawn-gate (node[0x32]/node[0x66]/DAT_800e7eaa), an animation/tick gate
//               (FUN_8014047c, node[0x2b] dec + node[0x56] +/-0x80 by pad bit), then a node[3]
//               compare-dispatch — 1..5 -> motion (FUN_80144928/800781e0/801409c0); 0x80/0x81 ->
//               native eng(c).attackOrbit.{aimAtTargetAnchor,orbitTargetMotion}() (was
//               FUN_80145af0/FUN_801458e0 — see game/ai/attack_orbit_substate.{h,cpp}); 0 OR any
//               other (>=6) -> FUN_80143a00 default — then a common tail:
//               cull FUN_800777fc, an SFX edge (node[0x67]==1 -> FUN_8009a450/FUN_80074590), a
//               DAT_8014bf5e countdown that on underflow may spawn (FUN_8003116c) keyed on
//               node[0x68], then FUN_80076d68 + render FUN_800518fc, and a node[0x29]->node[0xb]
//               flag fold; finally node[0x29]=node[0x5f]=node[0x67]=0.
//   state 2  -> FUN_80144b50                          (idle tick), then epilogue
//   state 3  -> node[0]=2 always; if node[0x1b]&0x40 clear bit 0x40 else despawn FUN_8007a624
//
// Ownership model (identical to the siblings): CONTROL FLOW + node/global memory writes owned native;
// every sub-behavior CALL stays a reachable PSX leaf via rec_dispatch (NO recursion into them). NO GTE,
// NO render packets here. RE'd 1:1 from disas 0x80145230..0x80145670 (no memory jump tables — all
// dispatch is compare-and-branch; decoded from the field RAM dump scratch/bin/field_ram_230.bin). It
// WRITES guest node state the still-recomp content reads -> content-INTERFACE: gated byte-exact (full
// RAM+scratchpad A/B vs rec_super_call). The idle/active field path is exercised by the gate; the
// input/scene-driven sub-states are faithfully transcribed and verify when a scene drives them.
//
// Register map (prologue 0x80145230): s0 = node (a0); s4 = node+0x60; s1 = node+0x60 (reloaded in tail);
// s3 = 1 (const); s2 = lui 0x8015 (DAT_8014bf5e = s2-0x40a2). a0 stays 2 from 0x8014527c into state 3.

#include "core.h"
#include "game_ctx.h"
#include "render/render.h"       // Core::mRender (NodeXform)
#include "render/cull.h"    // Cull::cullWrapperFlag2 (FUN_800777FC)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "animation.h" // Animation::step (FUN_80076D68)
#include "rng.h"       // class Rng (via rngOf(c).next())
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80145230u;

}  // namespace

void beh_id_compare_motion_dispatch(Core* c) {
  const uint32_t obj = c->r[4];                        // s0 = a0 = node              [0x80145238]
  const uint32_t s4  = obj + 0x60;                     // s4 = node+0x60              [0x8014525C]
  uint8_t st = c->mem_r8(obj + 4);                     // v1 = node[4] (state)        [0x80145250]

  // ---- state dispatch [0x80145258..0x80145294] ----
  if (st != 1) {                                       // beq v1,1 -> state 1         [0x80145258]
    if (st >= 2) {                                     // slti v0,v1,2 / beqz -> 0x8014527c [0x80145260..64]
      if (st == 2) {                                   // beq v1,2 -> 0x8014561c      [0x80145280]
        // ---- STATE 2 (idle) [0x8014561C] ----
        c->r[4] = obj; rec_dispatch(c, 0x80144B50u);   // FUN_80144b50                [0x8014561C]
        return;                                        // j 0x80145654 (epilogue)
      }
      if (st == 3) {                                   // beq v1,3 -> 0x8014562c      [0x80145288]
        // ---- STATE 3 (despawn) [0x8014562C] ----
        uint8_t n1b = c->mem_r8(obj + 0x1b);           // lbu v1,0x1b(s0)
        c->mem_w8(obj + 0, 2);                         // sb a0(=2),0(s0)  [delay slot @0x8014563C, ALWAYS]
        if (c->mem_r8(obj + 0x1b) & 0x40) {            // andi 0x40; beqz -> 0x8014564c [0x80145638]
          c->mem_w8(obj + 0x1b, (uint8_t)(n1b & 0xbf));// sb v0,0x1b(s0)  [delay slot @0x80145648]
          return;                                      // j 0x80145654 (NO despawn)
        }
        eng(c).spawn.despawn(obj);   // FUN_8007a624 (despawn)      [0x8014564C]
        return;                                        // j 0x80145654 (epilogue)
      }
      return;                                          // other (>=2) -> epilogue     [0x80145290]
    }
    if (st != 0) return;                               // beqz v1 -> state 0; else j epilogue [0x8014526C/74]
    // ---- STATE 0 (init) [0x80145298] ----
    c->r[4] = obj; rec_dispatch(c, 0x80140544u);       // FUN_80140544                [0x80145298]
    return;                                            // j 0x80145654 (epilogue)
  }

  // ================= STATE 1 (active) [0x801452A8] =================
  // ---- despawn gate [0x801452A8..0x801452EC] ----
  int16_t n32 = c->mem_r16s(obj + 0x32);       // lh a0,0x32(s0)              [0x801452A8]
  // a0>0 forces v0=1 (delay slot) and SKIPS the xori; a0<=0 computes cond then xori.
  bool to_state3;
  if (n32 > 0) {                                       // bgtz a0 -> 0x801452e0       [0x801452B0]
    to_state3 = true;                                  // v0=1 -> beqz false -> sb node[4]=3
  } else {
    bool cond;                                         // [0x801452B8..0x801452D8]
    if (c->mem_r8(s4 + 6) == 0x81) {                   // lbu node[0x66]; bne 0x81    [0x801452C0]
      cond = (n32 < -0x81);                            // slti a0,-0x81  [delay slot @0x801452CC, j 0x801452dc]
    } else {
      cond = (c->mem_r8(0x800E7EAAu) < 0xc);           // lbu DAT_800e7eaa; sltiu 0xc [0x801452D0..D8]
    }
    to_state3 = !cond;                                 // xori v0,1; beqz -> continue body iff cond [0x801452DC..E0]
  }
  if (to_state3) {                                     // beqz v0 false -> set state 3 & return [0x801452E0]
    c->mem_w8(obj + 4, 3);                             // sb v0(=3),4(s0)  [delay slot @0x801452EC]
    return;                                            // j 0x80145654 (epilogue)
  }

  // ---- animation/tick gate [0x801452F0..0x80145338] ----
  // Faithful flow:
  //   v0 = DAT_1f800137; if v0==0 -> 0x80145314 (skip the ==2 test).
  //   else v1 = DAT_800bf89c; if v1!=2 -> 0x80145328 (call FUN_8014047c).   [==2 falls to 0x80145314]
  //   0x80145314: v0 = DAT_800bf809; if v0==0 -> 0x80145340 (tick branch).  [!=0 falls to 0x80145328]
  //   0x80145328: jal FUN_8014047c(node); if v0==0 -> 0x80145388 (cull tail); else j epilogue (return).
  //   (DAT_800bf89c = -0x764(0x800c0000); DAT_800bf809 = -0x7f7(0x800c0000).)
  {
    bool to_tick;                                       // true -> 0x80145340; false -> 0x80145328 (gate)
    if (c->mem_r8(0x1F800137u) != 0 &&                  // [0x801452FC]
        c->mem_r8(0x800BF89Cu) != 2) {                  // [0x8014530C] -> 0x80145328 (gate), skip tick test
      to_tick = false;
    } else {                                            // 0x80145314: test DAT_800bf809
      to_tick = (c->mem_r8(0x800BF809u) == 0);          // beqz -> 0x80145340 (tick); else 0x80145328 (gate) [0x80145320]
    }

    if (to_tick) {
      // ---- node[0x2b] tick branch [0x80145340] ----
      uint8_t n2b = c->mem_r8(obj + 0x2b);              // lbu v0,0x2b(s0)             [0x80145340]
      if (n2b != 0) {                                   // beqz -> 0x801453a8 (node3 dispatch) [0x80145348]
        c->mem_w8(obj + 0x2b, (uint8_t)(n2b - 1));      // sb v0-1,0x2b(s0)  [delay slot @0x80145350]
        if (c->mem_r16(0x1F80017Cu) & 1) {              // lhu DAT_1f80017c; andi 1; beqz -> 0x80145378 [0x80145360]
          c->mem_w16(obj + 0x56,                         // node[0x56] += 0x80          [0x8014536C..74]
                     (uint16_t)(c->mem_r16(obj + 0x56) + 0x80));
        } else {
          c->mem_w16(obj + 0x56,                         // node[0x56] -= 0x80          [0x80145378..80]
                     (uint16_t)(c->mem_r16(obj + 0x56) - 0x80));
        }
        // sh node[0x56] [0x80145384]; then fall to cull tail 0x80145388
        // ---- cull tail (n2b!=0 path) [0x80145388] ----
        c->r[4] = obj; eng(c).cull.cullWrapperFlag2();     // FUN_800777FC (native)
        if (c->r[2] == 0) return;                       // beqz v0 -> epilogue          [0x80145390]
        rend(c)->mNodeXform.buildWithOffset(obj);              // FUN_800518FC (native)        [0x80145398]
        return;                                         // j 0x80145654 (epilogue)
      }
      // n2b==0 -> fall to 0x801453a8 (node[3] dispatch). The jal at 0x801453A8 below runs in BOTH the
      // tick-fallthrough (n2b==0) and gate paths — see GOTCHA note.
    } else {
      // ---- gate call [0x80145328] ----
      c->r[4] = obj; rec_dispatch(c, 0x8014047Cu);      // FUN_8014047c                [0x80145328]
      if (c->r[2] != 0) return;                         // bnez v0 -> epilogue          [0x80145330]
      // v0==0 -> 0x80145388 cull tail (NOT the node[3] dispatch). Distinct from the n2b==0 fallthrough.
      c->r[4] = obj; eng(c).cull.cullWrapperFlag2();     // FUN_800777FC (native)
      if (c->r[2] == 0) return;                         // beqz v0 -> epilogue          [0x80145390]
      rend(c)->mNodeXform.buildWithOffset(obj);                // FUN_800518FC (native)        [0x80145398]
      return;                                           // j 0x80145654 (epilogue)
    }
  }

  // ---- node[3] compare-dispatch [0x801453A8..0x80145510] ----
  // Reached ONLY from the tick branch with n2b==0 (fall through 0x801453A8). It re-calls FUN_8014047c.
  c->r[4] = obj; rec_dispatch(c, 0x8014047Cu);          // FUN_8014047c                [0x801453A8]
  if (c->r[2] != 0) goto after_no_cull;                 // bnez v0 -> 0x8014560c        [0x801453B0]
  // Dispatch on node[3] [0x801453C0..0x801453EC]:
  //   n3 < 6:  n3==0 -> 0x801453EC (the "default" block below);  n3 in 1..5 -> 0x80145420 (motion).
  //   n3 >= 6: ==0x80 -> 0x801454F8 (FUN_80145af0); ==0x81 -> 0x80145508 (FUN_801458e0);
  //            ELSE FALLS THROUGH to 0x801453EC — i.e. the same block as n3==0 (GOTCHA: not motion).
  {
    uint8_t n3 = c->mem_r8(obj + 3);                    // lbu v1,3(s0)                [0x801453B8]
    if (n3 >= 1 && n3 <= 5) {                           // [0x801453C4/CC -> 0x80145420]
      goto n3_motion;
    }
    if (n3 == 0x80) {                                   // beq 0x80 -> 0x801454f8       [0x801453DC]
      c->r[4] = obj; eng(c).attackOrbit.aimAtTargetAnchor();  // FUN_80145af0 (native)  [0x801454F8]
      goto second_cull;                                 // j 0x80145510
    }
    if (n3 == 0x81) {                                   // beq 0x81 -> 0x80145508       [0x801453E4]
      c->r[4] = obj; eng(c).attackOrbit.orbitTargetMotion();  // FUN_801458e0 (native)  [0x80145508]
      goto second_cull;                                 // j 0x80145510
    }
    // n3==0 OR (n3>=6 && !=0x80 && !=0x81) -> 0x801453EC
    // ---- default / node[3]==0 block [0x801453EC] ----
    c->r[4] = obj; rec_dispatch(c, 0x80143A00u);        // FUN_80143a00                [0x801453EC]
    if (c->mem_r8(obj + 0x2a) == 1) {                   // lbu node[0x2a]; bne 1 -> 0x80145510 [0x801453FC]
      int16_t n2e = c->mem_r16s(obj + 0x2e);    // lh node[0x2e]              [0x80145404]
      if (n2e >= 0x31a9) {                              // slti 0x31a9; bnez -> 0x80145510 (skip) [0x8014540C/10]
        c->mem_w16(obj + 0x2e, 0x31a8);                 // sh 0x31a8,node[0x2e] [delay slot @0x8014541C]
      }
    }
    goto second_cull;                                   // j 0x80145510
  }

n3_motion:;
  // ---- node[3] motion block [0x80145420..0x801454F4] ----
  {
    c->r[4] = obj; rec_dispatch(c, 0x80144928u);        // FUN_80144928                [0x80145420]
    uint16_t r = (uint16_t)c->r[2];                     // sll v0,0x10 -> low 16 bits  [0x80145428]
    if (r == 0) goto second_cull;                       // beqz -> 0x80145510           [0x8014542C]
    if (c->mem_r8(obj + 3) == 2) {                      // lbu node[3]; bne 2 -> 0x8014544c [0x8014543C]
      c->mem_w32(obj + 4, 0x801u);                      // sw 0x801,node[4] (node[4..7]) [0x801454EC]
      c->mem_w8(obj + 3, 0);                            // sb zero,node[3] [delay slot @0x801454F4]
      goto second_cull;                                 // j 0x80145510
    }
    // node[3]!=2: compute heading bucket, FUN_800781e0, then FUN_801409c0 [0x8014544C..0x801454CC]
    int16_t a2 = c->mem_r16s(obj + 0x2e);       // lh a2,0x2e(s0)
    int16_t t0 = c->mem_r16s(0x1F800160u);      // lh a0,0x160(0x1f80_0000)
    int16_t s36 = c->mem_r16s(obj + 0x36);      // lh v0,0x36(s0)
    int16_t t1 = c->mem_r16s(0x1F800164u);      // lh a1,0x164(0x1f80_0000)
    c->r[4] = (uint32_t)(int32_t)(int16_t)(a2 - t0);    // a0 = node[0x2e]-DAT_1f800160 (subu, used as arg)
    c->r[5] = (uint32_t)(int32_t)(int16_t)(s36 - t1);   // a1 = node[0x36]-DAT_1f800164
    rec_dispatch(c, 0x800781E0u);                       // FUN_800781e0 -> v0           [0x80145464]
    int32_t a1v;
    if (c->mem_r16(0x800E7FFEu) & 0x8200) {             // lh DAT_800e7ffe; andi 0x8200; bnez -> a1=-1 [0x80145478..7C]
      a1v = -1;                                         // addiu a1,-1  [delay slot @0x80145498]
    } else {
      // Nested slti/bnez ladder on v (sra16) [0x80145480..0x801454BC]. Bound order 0x641 > 0x44d > 0x259.
      // Decoded exactly (each bnez = "slti was true"):
      //   v >= 0x641            -> a1 = -1   (0x80145488/8C fall-through, 0x80145498)
      //   0x44d <= v < 0x641    -> a1 =  0   (0x8014549C fall-through, 0x801454A8)
      //   0x259 <= v < 0x44d    -> a1 =  1   (0x801454AC fall-through, 0x801454B8)
      //   v < 0x259             -> a1 =  2   (0x801454BC)
      int16_t v = (int16_t)(uint16_t)c->r[2];           // sll v0,0x10; sra 0x10        [0x80145480..84]
      if (v >= 0x641)      a1v = -1;
      else if (v >= 0x44d) a1v = 0;
      else if (v >= 0x259) a1v = 1;
      else                 a1v = 2;
    }
    c->r[4] = obj; c->r[5] = (uint32_t)a1v;             // a0=node, a1=(sll16/sra16 of a1v) [0x801454C0..C4]
    rec_dispatch(c, 0x801409C0u);                       // FUN_801409c0 -> v0           [0x801454C8]
    uint32_t v1 = c->r[2];                              // move v1,v0                   [0x801454D0]
    if ((v1 & 0xffff) == 0) {                           // andi 0xffff; beqz -> 0x801454ec [0x801454D8]
      c->mem_w32(obj + 4, 0x301u);                      // sw 0x301,node[4] (node[4..7]) [0x801454EC]
      c->mem_w8(obj + 3, 0);                            // sb zero,node[3] [delay slot @0x801454F4]
      goto second_cull;                                 // j 0x80145510
    }
    c->mem_w8(obj + 5, (uint8_t)v1);                    // sb v1,5(s0)                 [0x801454E0]
    c->mem_w8(obj + 6, 0);                              // sb zero,6(s0) [delay slot @0x801454E8]
    c->mem_w8(obj + 3, 0);                              // sb zero,node[3] [delay slot @0x801454F4]
    goto second_cull;                                   // j 0x80145510
  }

second_cull:;
  // ---- common tail: cull, SFX edge, countdown spawn, render [0x80145510..0x8014560C] ----
  c->r[4] = obj; eng(c).cull.cullWrapperFlag2();     // FUN_800777FC (native)
  if (c->r[2] == 0) goto after_no_cull;                 // beqz v0 -> 0x8014560c        [0x80145518]
  {
    if (c->mem_r8(s4 + 7) == 1) {                       // lbu node[0x67]; bne 1 -> 0x80145548 [0x80145528]
      uint32_t rr = (uint32_t)rngOf(c).next();                 // FUN_8009A450 -> native class Rng    [0x80145530]
      eng(c).sfx.trigger(0x87, (int)(rr & 3), 0);   // FUN_80074590 (native) — id 0x87 hits path A (per-area)
    }
    // ---- DAT_8014bf5e countdown [0x80145548..0x801455C4] ----
    uint8_t cd = c->mem_r8(0x8014BF5Eu);                // lbu v0,DAT_8014bf5e         [0x80145548]
    c->mem_w8(0x8014BF5Eu, (uint8_t)(cd - 1));          // sb v0-1 [delay slot @0x8014555C, ALWAYS]
    if ((int8_t)cd < 0) {                               // sll v0,0x18; bgez -> 0x801455c8 (skip) [0x80145558]
      uint8_t kind = (uint8_t)((c->mem_r16(s4 + 8) >> 8) & 0xf);  // lhu node[0x68]; srl8; andi 0xf [0x80145560..6C]
      bool spawn;
      if (kind == 1) {                                  // beq kind,1 -> 0x80145594     [0x80145570]
        spawn = true;
      } else if (kind != 2) {                           // bne kind,2 -> 0x801455c4     [0x80145578]
        spawn = false;
      } else {                                          // kind==2
        spawn = (c->mem_r16s(obj + 0x4e) >= 0x501);  // lh node[0x4e]; slti 0x501; bnez skip [0x80145588]
      }
      if (spawn) {                                      // ---- spawn at 0x80145594 ----
        // FUN_8003116c takes a1 = GUEST pointer to an on-stack arg struct. Reproduce the sp-relative
        // stores into the live guest stack exactly as the disasm does, then pass guest sp+0x10.
        // a0=8 [0x80145598], a1=sp+0x10 [0x801455A4], a2=-0x50 [0x801455B0].
        uint32_t gsp = c->r[29];
        c->mem_w16(gsp + 0x12, (uint16_t)c->mem_r16(obj + 0x2e));  // sh node[0x2e],0x12(sp) [0x8014559C]
        c->mem_w16(gsp + 0x16, (uint16_t)c->mem_r16(s4 + 0xa));    // sh node[0x6a],0x16(sp) [0x801455A8]
        c->mem_w16(gsp + 0x1a, (uint16_t)c->mem_r16(obj + 0x36));  // sh node[0x36],0x1a(sp) [0x801455B8]
        eng(c).spawn.spawnAndInit(8, gsp + 0x10, (uint32_t)(int32_t)-0x50);   // FUN_8003116c (spawn) [0x801455B4]
        c->mem_w8(0x8014BF5Eu, 0xa);                    // sb 0xa,DAT_8014bf5e          [0x801455C0]
      }
      c->mem_w16(s4 + 8, 0);                            // sh zero,8(s1)=node[0x68]     [0x801455C4]
    }
    // ---- render + flag fold [0x801455C8..0x80145608] ----
    eng(c).animation.step(obj);                       // FUN_80076D68 (native)       [0x801455C8]
    rend(c)->mNodeXform.buildWithOffset(obj);                  // FUN_800518FC (native)       [0x801455D0]
    if (c->mem_r8(obj + 0x29) != 0) {                   // lbu node[0x29]; beqz -> 0x801455fc [0x801455E0]
      c->mem_w8(obj + 0xb, (uint8_t)((c->mem_r8(obj + 0xb) & 0xc0) | 0x80));  // node[0xb]&0xc0|0x80 [0x801455E8..F8]
    } else {
      c->mem_w8(obj + 0xb, (uint8_t)(c->mem_r8(obj + 0xb) & 0x3f));           // node[0xb]&0x3f     [0x801455FC..04]
    }
  }

after_no_cull:;
  // ---- 0x8014560C (common epilogue clears, also the cull-miss target) ----
  c->mem_w8(obj + 0x29, 0);                             // sb zero,0x29(s0)            [0x8014560C]
  c->mem_w8(obj + 0x5f, 0);                             // sb zero,0x5f(s0)            [0x80145610]
  c->mem_w8(s4 + 7, 0);                                 // sb zero,7(s4)=node[0x67] [delay slot @0x80145618]
  // j 0x80145654 (epilogue)
}
