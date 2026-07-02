// engine/beh_pickup_collect_trigger.cpp — PC-native per-object BEHAVIOR handler FUN_800741DC.
//
// The third resident per-object behavior routine reached in the seaside field (sibling of FUN_800739AC /
// FUN_80073CD8). Same state-byte shape (node[4]: 0 init / 1 active / 2 idle / 3 despawn). It is an
// item/pickup-style scene trigger: STATE 0 seeds the cull-record (FUN_80051B70 a1=1, a2=0x18) + box/size +
// node+0x56 from per-type table DAT_800a4cec; STATE 1 runs a node[5] sub-machine that registers a scene
// (FUN_8007E110 keyed by DAT_800a4cf8[node[3]]), waits on a pad edge (DAT_800e7e68 & DAT_1f800174), spawns
// a child (FUN_8007413C) bounded by DAT_800a4d04[node[3]] vs the counter DAT_800bf874, and on completion
// (case 4) emits two FUN_80027144 packets + SFX (FUN_80074590) and sets the per-type collected bit in
// DAT_800bfa23 / toggles FUN_80040b48/c00(0x39/0x3a) + the all-collected (==0x1f) reward.
//
// Ownership model (same as the FUN_739ac handler/73cd8): CONTROL FLOW + node/global memory writes owned native;
// every sub-behavior CALL stays reachable by address via rec_dispatch. NO GTE, NO render packets here.
// RE'd 1:1 from disas 0x800741DC (state-1 dispatch is a plain if-chain, not a jump table). NB like 73cd8 it
// calls cull FUN_8007778C and IGNORES the result. The case-4 path (node[5]==4) builds a small struct on the
// guest stack and hands it to FUN_80027144, so this handler mirrors the recomp body's `sp -= 0x30` frame
// discipline (see beh_pickup_collect_trigger wrapper) — the buffer then sits above the sub-call frames exactly as the recomp
// would place it. It WRITES guest node state the still-recomp content reads → content-INTERFACE: gated
// byte-exact (full RAM+scratchpad A/B vs rec_super_call). The idle field path is exercised by the gate; the
// pad/scene-driven sub-states (incl. case 4) are faithfully transcribed and verify when a scene drives them.

#include "core.h"
#include "cfg.h"
#include "graphics_bind.h"   // ov_obj_record_init — native graphics-bind (game/world)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x800741DCu;

// LAB_80074328 — the node[5]==0 / re-armed-99 path: on node[0x2b]==3 register a scene + advance.
inline void do_case0(Core* c, uint32_t obj) {
  if (c->mem_r8(obj + 0x2b) != 3) return;
  int16_t sid = c->mem_r16s(0x800A4CF8u + (uint32_t)c->mem_r8(obj + 3) * 2);
  c->r[4] = (uint32_t)(int32_t)sid; c->r[5] = 0;
  rec_dispatch(c, 0x8007E110u);
  c->mem_w32(obj + 0x14, c->r[2]);
  if (c->r[2] != 0) {
    if (c->mem_r8(0x800BF8EDu) == 0) {
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      c->r[4] = 0x39; rec_dispatch(c, 0x80040B48u);            // SFX (result ignored)
    } else {
      c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 2));
    }
  }
  c->mem_w8(0x800BF809u, 1);
}

// returns true if the body issued an early return (case-4 DAT_800bf8ee==0 path); false to run the common tail.
bool beh_pickup_collect_trigger_body(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t st = c->mem_r8(obj + 4);

  if (st != 1) {
    if (st >= 2) {                                   // 2 idle / 3 despawn / other
      if (st == 2) return true;
      if (st != 3) return true;
      world_despawn(c, obj);   // despawn
      return true;
    }
    if (st != 0) return true;
    // ---- STATE 0 ----
    if (c->mem_r8(0x800BF873u) != 0) { c->mem_w8(obj + 4, 3); return true; }
    c->r[4] = obj; c->r[5] = 1; c->r[6] = 0x18;
    c->engine.graphicsBind.recordInit();   // OWNED native graphics-bind (render-record alloc + geomblk resolve into node+0xC0)
    if (c->r[2] != 0) return true;
    c->mem_w16(obj + 0x80, 0x140);
    c->mem_w16(obj + 0x82, 0x280);
    c->mem_w16(obj + 0x84, 0x15e);
    c->mem_w16(obj + 0x86, 0x15e);
    c->mem_w8 (obj + 0, 1);
    c->mem_w8 (obj + 0x2b, 0);
    c->mem_w16(obj + 0x54, 0);
    c->mem_w8 (obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));
    uint16_t sid = c->mem_r16(0x800A4CECu + (uint32_t)c->mem_r8(obj + 3) * 2);
    c->mem_w16(obj + 0x58, 0);
    c->mem_w16(obj + 0x56, sid);
    c->r[4] = obj; c->engine.graphicsBind.renderUpdate();
    return true;
  }

  // ---- STATE 1 ----
  c->r[4] = obj; rec_dispatch(c, 0x8007778Cu);       // cull (result IGNORED)
  uint8_t sub = c->mem_r8(obj + 5);
  int iVar3 = 0; bool do_inc = false;

  if (sub == 2) {                                    // pad edge -> node[5]=3
    if (c->mem_r16(0x800E7E68u) & c->mem_r16(0x1F800174u)) c->mem_w8(obj + 5, 3);
  } else if (sub < 3) {
    if (sub == 0) { do_case0(c, obj); }
    else if (sub != 1) { c->mem_w8(obj + 0x2b, 0); return true; }
    else { rec_dispatch(c, 0x80042728u); iVar3 = (int)c->r[2]; do_inc = true; }   // case 1
  } else {
    if (sub == 4) {
      // ---- case 4: emit packets + collected-bit/reward ----
      uint32_t p = c->mem_r32(obj + 0x14);
      if (c->mem_r8(p + 4) > 1) {
        uint32_t buf = c->r[29] + 0x10;              // sp already = caller_sp - 0x30 (see wrapper)
        c->mem_w16(buf + 2, c->mem_r16(obj + 0x2e));
        uint16_t n84 = c->mem_r16(obj + 0x84);
        int32_t x   = (int16_t)n84;
        int32_t sgn = ((uint32_t)((uint32_t)n84 << 16)) >> 31;
        int32_t half = (x + sgn) >> 1;
        c->mem_w16(buf + 6, (uint16_t)((uint32_t)c->mem_r16(obj + 0x32) - (uint32_t)half));
        c->mem_w16(buf + 0xa, c->mem_r16(obj + 0x36));
        c->mem_w8(obj + 0, 2);
        c->mem_w8(obj + 4, 3);
        uint32_t src = c->mem_r32(c->mem_r32(obj + 0xc0) + 0x40);
        c->r[4] = src; c->r[5] = buf; c->r[6] = 0x800; c->r[7] = 0x18; rec_dispatch(c, 0x80027144u);
        c->r[4] = 0xc; c->r[5] = 0; c->r[6] = 0; rec_dispatch(c, 0x80074590u);
        src = c->mem_r32(c->mem_r32(obj + 0xc0) + 0x40);
        c->r[4] = src; c->r[5] = buf; c->r[6] = 0x800; c->r[7] = 8; rec_dispatch(c, 0x80027144u);
        c->r[4] = 0xc; c->r[5] = 0; c->r[6] = 0; rec_dispatch(c, 0x80074590u);
        c->r[4] = 0x39; rec_dispatch(c, 0x80040C00u);
        uint8_t prev = c->mem_r8(0x800BFA23u);
        uint32_t bit = 1u << (c->mem_r8(obj + 3) & 0x1f);
        c->mem_w8(0x800BFA23u, (uint8_t)(prev | (uint8_t)bit));
        if (c->mem_r8(0x800BF8EEu) == 0) {
          c->r[4] = 0x3a; rec_dispatch(c, 0x80040B48u);
          c->mem_w8(obj + 0x2b, 0);
          return true;                               // early return (LAB at 0x8007454c)
        }
        if ((((uint32_t)prev) | (bit & 0xff)) == 0x1f) { c->r[4] = 0x3a; rec_dispatch(c, 0x80040C00u); }
      }
    } else if (sub > 3) {                            // sub in {5..98,100+} no-op; 99 re-arms -> case0
      if (sub == 99) {
        c->mem_w8(0x800BF809u, 0);
        c->mem_w8(obj + 0, 1);
        c->mem_w8(obj + 5, 0);
        do_case0(c, obj);
      }
    } else {
      // ---- case 3: release prev, spawn child (bounded) ----
      uint32_t p = c->mem_r32(obj + 0x14);
      if (c->mem_r8(p + 4) < 2) { c->mem_w8(p + 4, 2); c->mem_w32(obj + 0x14, 0); }
      uint32_t lim = c->mem_r32(0x800A4D04u + (uint32_t)c->mem_r8(obj + 3) * 4);
      if (c->mem_r32(0x800BF874u) < lim) {
        c->mem_w8(obj + 0, 2);
        c->mem_w8(obj + 5, 99);
      } else {
        c->r[4] = obj; rec_dispatch(c, 0x8007413Cu);
        iVar3 = (int)c->r[2];
        c->mem_w32(obj + 0x14, c->r[2]);
        do_inc = true;
      }
    }
  }

  if (do_inc && iVar3 != 0) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
  c->mem_w8(obj + 0x2b, 0);
  return true;
}

// Mirror the recomp body's stack frame (addiu sp,-0x30) so case-4's FUN_80027144 buffer at sp+0x10 sits
// above the rec_dispatch sub-call frames exactly as the recomp would place it. Net-zero on sp across return.
void beh_pickup_collect_trigger(Core* c) {
  c->r[29] -= 0x30;
  beh_pickup_collect_trigger_body(c);
  c->r[29] += 0x30;
}

void ov_beh_pickup_collect_trigger(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("pickup_collect_triggerverify") ? 1 : 0;
  if (!s_v) { beh_pickup_collect_trigger(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_pickup_collect_trigger(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[pickup_collect_triggerverify] MISMATCH obj=%08x st=%u sub=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[pickup_collect_triggerverify] %ld matches\n", ng);
}

}  // namespace

void beh_pickup_collect_trigger_register(void) {
}

// Exported entry — the verify wrapper ov_beh_pickup_collect_trigger is in the anonymous namespace above (internal linkage);
// the engine's per-object dispatch (engine_tomba2.cpp call_handler) calls THIS to run the owned behavior.
void ov_beh_pickup_collect_trigger_run(Core* c) { ov_beh_pickup_collect_trigger(c); }
