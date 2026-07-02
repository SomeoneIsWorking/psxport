// engine/beh_visibility_gate_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_8004C238.
//
// A RESIDENT MAIN.EXE per-object behavior routine (range 0x8004C238..0x8004C92C; epilogue at
// 0x8004C920, `jr ra` at 0x8004C928; next function prologue at 0x8004C930). Same SHAPE as the
// sibling owned behaviors (the FUN_73cd8 handler / the FUN_739ac handler / actor_sm_24448): a state machine on the
// node's state byte node[4] (0 init / 1 active / 2 transition / 3 despawn).
//
//   STATE 0 (init)        : JT-A 0x800157A4 indexed by node[0x5e] (<16) -> one of two init callees
//                           (0x8004A828 / 0x8004A9A4) selected by node[0x28]&0x7f for idx 0-5; always
//                           advances node[4] -> 1 (delay-slot store, runs even on the >=16 bail).
//   STATE 1 (active)       : JT-B 0x800157E4 indexed by node[0x5e] (<16). Cases 0-5 run a shared
//                           VISIBILITY GATE (node[0x28]&0x80 + global 0x800BF816/0x800BF817 vs
//                           node[0x6a], else cull FUN_8007778C) that sets node[1] and optionally
//                           submits via FUN_80077EFC, then dispatch a per-case action
//                           (0x8004B150 / 0x80049A60 / 0x8004B208) with a1=0 or 1 and a post-call
//                           (0x80077B5C or 0x80051844). Cases 6-15 are area/event handlers gated on
//                           the area byte 0x800BF870.
//   STATE 2 (transition)   : node[5] sub-machine. node[5]==0 -> JT-C 0x80015824 indexed by node[3]
//                           (<18) seeding timers / sub-behaviors, conditionally advancing node[5];
//                           node[5]==1 -> finalize (0x8004D714 / 0x8004D79C) and set node[4]=3.
//   STATE 3 (despawn)      : FUN_8007A624(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + every node/global memory write owned
// native byte-for-byte; every sub-behavior CALL stays reachable by address via rec_dispatch (leaf,
// no recursion). NO GTE, NO render packets here. RE'd 1:1 from disas 0x8004C238. It WRITES guest
// node state the still-recomp content reads -> content-INTERFACE: gated byte-exact (full
// RAM+scratchpad A/B vs rec_super_call).

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8004C238u;

// ---- STATE 1 shared VISIBILITY GATE (the body at 0x8004c324 / c3a4 / c414 / c49c / c51c / c58c) ----
// 0x8004c324: v0 = node[0x28] & 0x80; if 0 -> cull path (jal 0x8007778c).
//   bit set : v0 = byte@0x800BF816; if 0 -> skip (node[1] unchanged, no submit).
//             else v1 = byte@0x800BF817; if v1 != (s16)node[0x6a] -> skip; else node[1]=1 + submit.
//   bit clr : jal 0x8007778c(node); if it returns nonzero -> skip submit; else submit.
// "submit" = jal 0x80077efc(node).  GOTCHA: in the bit-set match path node[1]=1 is written in the
// delay slot of the `j` into the submit call (0x8004c364 `sb v0,1(s0)`), then control falls into the
// submit; node[1] is NOT touched on the cull path. Cases 1 & 4 are the same logic with a compiler
// variation (no separate node[1] re-check before the action — see callers).
void state1_gate(Core* c, uint32_t obj) {
  uint8_t f = c->mem_r8(obj + 0x28);
  bool submit;
  if (f & 0x80) {
    if (c->mem_r8(0x800BF816u) == 0) {
      submit = false;                                 // 0x8004c344: -> skip
    } else if ((uint32_t)(uint8_t)c->mem_r8(0x800BF817u) !=
               (uint32_t)c->mem_r16s(obj + 0x6a)) {
      // 0x8004c358: bne compares full 32-bit regs: v1 = zero-ext byte, v0 = sign-ext halfword.
      submit = false;                                 // 0x8004c358: bne -> skip
    } else {
      c->mem_w8(obj + 1, 1);                          // 0x8004c364 (delay slot): node[1] = 1
      submit = true;                                  // falls into 0x80077efc
    }
  } else {
    c->r[4] = obj; rec_dispatch(c, 0x8007778Cu);      // 0x8004c368: cull
    submit = (c->r[2] == 0);                          // 0x8004c370: bne v0,zero -> skip submit
  }
  if (submit) { c->r[4] = obj; rec_dispatch(c, 0x80077EFCu); }  // 0x8004c378
}

void beh_visibility_gate_dispatch(Core* c) {
  const uint32_t obj = c->r[4];                        // 0x8004c240: s0 = a0 (object node ptr)
  uint8_t st = c->mem_r8(obj + 4);                     // 0x8004c248: node[4]

  // ---- top-level state dispatch (0x8004c248..0x8004c288) ----
  if (st == 1) goto state1;                            // 0x8004c250: beq v1,1
  if (st >= 2) {                                       // 0x8004c258: slti v0,v1,2 ; beq->c270
    if (st == 2) goto state2;                          // 0x8004c274
    if (st == 3) {                                     // 0x8004c27c
      // ---- STATE 3 (despawn) @0x8004c918 ----
      world_despawn(c, obj);
      return;                                          // -> EPI
    }
    return;                                            // 0x8004c284: default -> EPI
  }
  if (st != 0) return;                                 // 0x8004c260: only state 0 left (delay-checked)

  // ================= STATE 0 (init) @0x8004c28c =================
  {
    uint8_t n5e = c->mem_r8(obj + 0x5e);               // 0x8004c28c: node[0x5e]
    // 0x8004c294 sltiu v0,n5e,16 ; 0x8004c298 beq v0,zero,c920 ; 0x8004c29c (DELAY) sb a0,4(s0).
    // GOTCHA: node[4]=1 is the delay slot of the >=16 bail -> it runs UNCONDITIONALLY.
    c->mem_w8(obj + 4, 1);                             // 0x8004c29c (delay): node[4] = 1 (a0==1)
    if (n5e >= 16) return;                             // 0x8004c298: -> EPI

    // JT-A 0x800157A4 (node[0x5e]): idx 0-5 -> c2c0 ; 6-9,12-15 -> c2d4 ; 10-11 -> c2e4
    bool call_a9a4;
    if (n5e <= 5) {
      // 0x8004c2c0: if (node[0x28] & 0x7f) -> 0x8004a9a4 else 0x8004a828
      call_a9a4 = ((c->mem_r8(obj + 0x28) & 0x7f) != 0);
    } else if (n5e == 10 || n5e == 11) {
      call_a9a4 = true;                                // -> c2e4
    } else {
      call_a9a4 = false;                               // 6-9,12-15 -> c2d4
    }
    c->r[4] = obj;
    rec_dispatch(c, call_a9a4 ? 0x8004A9A4u : 0x8004A828u);
    return;                                            // -> EPI
  }

  // ================= STATE 1 (active) @0x8004c2f4 =================
state1:
  {
    uint8_t n5e = c->mem_r8(obj + 0x5e);               // 0x8004c2f4
    if (n5e >= 16) goto tail_c74c;                     // 0x8004c300: beq -> 0x8004c74c

    // JT-B 0x800157E4 (node[0x5e])
    switch (n5e) {
      // ---- gate cases 0-5 (the visibility-gated action cases) ----
      case 0:                                          // @0x8004c324
        state1_gate(c, obj);
        if (c->mem_r8(obj + 1) == 0) goto tail_c74c;   // 0x8004c388: beq node[1],0 -> c74c
        c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x8004B150u);
        goto post_c48c;                                // 0x8004c39c
      case 1:                                          // @0x8004c3a4 (no node[1] re-check)
        state1_gate(c, obj);
        c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x80049A60u);
        goto post_c48c;                                // 0x8004c40c
      case 2:                                          // @0x8004c414
        state1_gate(c, obj);
        if (c->mem_r8(obj + 1) == 0) goto tail_c74c;   // 0x8004c478
        c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x8004B208u);
        goto post_c48c;                                // 0x8004c48c (falls into post)
      case 3:                                          // @0x8004c49c
        state1_gate(c, obj);
        if (c->mem_r8(obj + 1) == 0) goto tail_c74c;   // 0x8004c500
        c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x8004B150u);
        goto post_c604;                                // 0x8004c514
      case 4:                                          // @0x8004c51c (no node[1] re-check)
        state1_gate(c, obj);
        c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x80049A60u);
        goto post_c604;                                // 0x8004c584
      case 5:                                          // @0x8004c58c
        state1_gate(c, obj);
        if (c->mem_r8(obj + 1) == 0) goto tail_c74c;   // 0x8004c5f0
        c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x8004B208u);
        goto post_c604;                                // 0x8004c604 (falls into post)

      // ---- area/event handler cases 6-15 ----
      case 6: {                                        // @0x8004c614
        uint8_t area = c->mem_r8(0x800BF870u);
        if (area == 0) { c->r[4] = obj; rec_dispatch(c, 0x80118B10u); goto tail_c750; }
        if (area == 6) { c->r[4] = obj; rec_dispatch(c, 0x80116288u); goto tail_c750; }
        goto tail_c74c;                                // 0x8004c638: bne v1,6 -> c74c
      }
      case 7:                                          // @0x8004c650
        c->r[4] = obj; rec_dispatch(c, 0x80118DB0u); goto tail_c750;
      case 8:                                          // @0x8004c660
        c->r[4] = obj; rec_dispatch(c, 0x80119350u); goto tail_c750;
      case 9: {                                        // @0x8004c670
        uint8_t area = c->mem_r8(0x800BF870u);
        if (area == 0) { c->r[4] = obj; rec_dispatch(c, 0x80118F50u); goto tail_c750; }
        if (area == 4) { c->r[4] = obj; rec_dispatch(c, 0x801193D4u); goto tail_c750; }
        if (area == 5) { c->r[4] = obj; rec_dispatch(c, 0x80112F88u); goto tail_c750; }
        if (area == 6) { c->r[4] = obj; rec_dispatch(c, 0x801160D4u); goto tail_c750; }
        if (area == 8) { c->r[4] = obj; rec_dispatch(c, 0x80116750u); goto tail_c750; }
        goto tail_c74c;                                // 0x8004c6dc: bne v1,8 -> c74c
      }
      case 10:                                         // @0x8004c6f4
        c->r[4] = obj; rec_dispatch(c, 0x80119454u); goto tail_c750;
      case 11:                                         // @0x8004c704
        c->r[4] = obj; rec_dispatch(c, 0x801132B8u); goto tail_c750;
      case 12:                                         // @0x8004c714
        c->r[4] = obj; rec_dispatch(c, 0x801132F0u); goto tail_c750;
      case 13:                                         // @0x8004c724
        c->r[4] = obj; rec_dispatch(c, 0x801133F4u); goto tail_c750;
      case 14:                                         // @0x8004c734
        c->r[4] = obj; rec_dispatch(c, 0x80113490u); goto tail_c750;
      case 15:                                         // @0x8004c744 (falls into c74c)
        c->r[4] = obj; rec_dispatch(c, 0x80119170u); goto tail_c74c;
    }
  }

post_c48c:                                             // @0x8004c48c
  c->r[4] = obj; rec_dispatch(c, 0x80077B5Cu);
  c->mem_w8(obj + 0x29, 0);                            // 0x8004c498 (delay slot of j c750)
  goto tail_c750;                                      // 0x8004c494

post_c604:                                             // @0x8004c604
  c->r[4] = obj; rec_dispatch(c, 0x80051844u);
  c->mem_w8(obj + 0x29, 0);                            // 0x8004c610 (delay slot of j c750)
  goto tail_c750;                                      // 0x8004c60c

tail_c74c:                                             // @0x8004c74c
  // falls into tail_c750
tail_c750:                                             // @0x8004c750
  // Every recomp path that reaches c750 has just written node[0x29]=0: tail_c74c (0x8004c74c) and ALSO
  // each cases-6-14 `j c750` sets node[0x29]=0 in its DELAY SLOT (0x8004c634/c64c/c65c/c66c/c690/c6a8/
  // c6c0/c6d8/c6f0/c700/c710/c720/c730/c740). The earlier transcription dropped those delay-slot stores
  // for cases 6-14; clearing node[0x29] here covers all of them faithfully (redundant for cases 0-5,
  // which already cleared it in post_c48c/post_c604). (later-232c fix)
  c->mem_w8(obj + 0x29, 0);                            // node[0x29] = 0 (all c750 predecessors)
  c->mem_w8(obj + 0x2b, 0);                            // 0x8004c754 (delay slot of j c920): node[0x2b]=0
  return;                                              // -> EPI

  // ================= STATE 2 (transition) @0x8004c758 =================
state2:
  {
    uint8_t n5 = c->mem_r8(obj + 5);                   // 0x8004c758: node[5]
    if (n5 == 1) goto state2_n5_1;                     // 0x8004c768: beq v0,a0(==1) -> c8c8
    if (n5 != 0) return;                               // 0x8004c770: default -> EPI

    // node[5] == 0 (@0x8004c778): JT-C 0x80015824 indexed by node[3]
    uint8_t n3 = c->mem_r8(obj + 3);                   // 0x8004c778
    bool check_ret;                                    // true: go via 0x8004c8ac (advance only if nonzero)
                                                       // false: go via 0x8004c8b4 (always advance)
    if (n3 >= 18) {                                    // 0x8004c784: sltiu fails -> default
      c->r[4] = obj; rec_dispatch(c, 0x8004A3D4u);     // @0x8004c8a4 (default)
      check_ret = true;
    } else {
      switch (n3) {                                    // JT-C
        case 0:  c->r[4]=obj; c->r[5]=1;      rec_dispatch(c, 0x80049E54u); check_ret=true;  break; // c7a8
        case 1:  c->r[4]=obj; c->r[5]=2;      rec_dispatch(c, 0x80049E54u); check_ret=true;  break; // c7bc
        case 4:  c->r[4]=obj; c->r[5]=100;    rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c7d0
        case 5:  c->r[4]=obj; c->r[5]=200;    rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c7e4
        case 6:  c->r[4]=obj; c->r[5]=500;    rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c7f8
        case 7:  c->r[4]=obj; c->r[5]=1000;   rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c80c
        case 8:  c->r[4]=obj; c->r[5]=5000;   rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c820
        case 9:  c->r[4]=obj; c->r[5]=10000;  rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c834
        case 10: c->r[4]=obj; c->r[5]=20000;  rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c848
        case 11: c->r[4]=obj; c->r[5]=0x186A0;rec_dispatch(c, 0x8004B3F4u); check_ret=false; break; // c85c (100000)
        case 15: c->r[4]=obj;                 rec_dispatch(c, 0x8004A118u); check_ret=true;  break; // c874
        case 16: c->r[4]=obj;                 rec_dispatch(c, 0x8004A2A0u); check_ret=true;  break; // c884
        case 17: c->r[4]=obj;                 rec_dispatch(c, 0x8004B428u); check_ret=true;  break; // c894
        default: // cases 2,3,12,13,14 -> JT-C default 0x8004c8a4
          c->r[4]=obj; rec_dispatch(c, 0x8004A3D4u); check_ret=true; break;
      }
    }
    // 0x8004c8ac: if check_ret and return==0 -> EPI (no advance)
    if (check_ret && c->r[2] == 0) return;             // 0x8004c8ac: beq v0,zero,c920
    // 0x8004c8b4: node[5] = node[5] + 1
    c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
    return;                                            // -> EPI
  }

state2_n5_1:                                            // @0x8004c8c8 (node[5] == 1)
  {
    uint8_t nv = 3;                                    // GOTCHA: v0=3 set in branch delay slots -> node[4]=3
    if ((int16_t)c->mem_r16(obj + 0x60) != 0) {        // 0x8004c8c8: lh node[0x60]; beq->c910 (delay v0=3)
      uint16_t flags = c->mem_r16(obj + 0x64);         // 0x8004c8d8: lhu node[0x64]
      int16_t arg = (int16_t)c->mem_r16(obj + 0x62);   // node[0x62] (s16)
      if (flags & 0x4) {                               // 0x8004c8e4: bne -> c900
        c->r[4] = (uint32_t)(int32_t)arg; c->r[5] = 1; rec_dispatch(c, 0x8004D79Cu); // 0x8004c904
      } else {
        c->r[4] = (uint32_t)(int32_t)arg; c->r[5] = 1; rec_dispatch(c, 0x8004D714u); // 0x8004c8f0
      }
    }
    c->mem_w8(obj + 4, nv);                            // 0x8004c914 (delay slot of j c920): node[4] = 3
    return;                                            // -> EPI
  }
}

void ov_beh_visibility_gate_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("visibility_gate_dispatchverify") ? 1 : 0;
  if (!s_v) { beh_visibility_gate_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_visibility_gate_dispatch(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[visibility_gate_dispatchverify] MISMATCH obj=%08x st=%u n5e=%u sub=%u ram@%x (nat=%02x ora=%02x) spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 0x5e), c->mem_r8(obj + 5), ro,
                           ro >= 0 ? ramN[ro] : 0, ro >= 0 ? c->ram[ro] : 0, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[visibility_gate_dispatchverify] %ld matches\n", ng);
}

}  // namespace

void beh_visibility_gate_dispatch_register(void) {
}

// Exported entry — the verify wrapper ov_beh_visibility_gate_dispatch is in the anonymous namespace above (internal
// linkage); the engine's per-object dispatch calls THIS to run the owned behavior.
void ov_beh_visibility_gate_dispatch_run(Core* c) { ov_beh_visibility_gate_dispatch(c); }
