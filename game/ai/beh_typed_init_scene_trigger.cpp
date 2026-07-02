// engine/beh_typed_init_scene_trigger.cpp — PC-native per-object BEHAVIOR handler FUN_80073CD8.
//
// The second resident, GENERIC per-object behavior routine the field OBJECT-PLACEMENT DRIVER
// (FUN_80072A78, ov_place_objects) installs at node+0x1c. Same SHAPE as the sibling FUN_800739AC
// (engine/the FUN_739ac handler.cpp) — a state machine on the node's state byte node[4] (0 init / 1 active /
// 2 idle / 3 despawn) — but larger: state-0 has a per-node[3] sub-switch (jump table 0x80016B68,
// node[3]-2 in [0,30]) that seeds box/size fields from per-type data, and state-1 has a node[5]
// sub-machine (jump table 0x80016BE8, node[5] in [0,6]) that talks to FUN_8007E110 / FUN_80040B48 /
// FUN_80042728 and drives a scene/UI trigger (push-into-scene / save flags / area transition).
//
// Ownership model (identical to the FUN_739ac handler / actor_sm_24448 / script_vm): CONTROL FLOW + node/global
// memory writes owned native; every sub-behavior CALL stays reachable by address via rec_dispatch (each
// honors its own override identically). NO GTE, NO render packets here. RE'd 1:1 from disas 0x80073CD8
// (state-0 JT 0x80016B68, state-1 JT 0x80016BE8) — see docs/engine_re.md. It WRITES guest node state the
// still-recomp content reads → content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs
// rec_super_call). NB: state-1 calls the cull FUN_8007778C but, unlike 739ac, IGNORES its result and
// always falls through to the sub-machine. The IDLE field path is exercised by the gate; the input/
// scene-driven sub-states are faithfully transcribed and verify when a scene drives them.

#include "core.h"
#include "cfg.h"
#include "graphics_bind.h"   // ov_obj_record_init — native graphics-bind (game/world)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (c->engine.spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80073CD8u;

// LAB_80040b48 confirm helper: call FUN_80040b48(arg); if it returns nonzero, node[5] = 5.
inline void confirm_set5(Core* c, uint32_t obj, uint32_t arg) {
  c->r[4] = arg; rec_dispatch(c, 0x80040B48u);
  if (c->r[2] != 0) c->mem_w8(obj + 5, 5);
}

void beh_typed_init_scene_trigger(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t st = c->mem_r8(obj + 4);

  if (st != 1) {
    if (st >= 2) {                                   // state 2 (idle) / 3 (despawn) / other
      if (st == 2) return;
      if (st != 3) return;
      c->engine.spawn.despawn(obj);   // despawn
      return;
    }
    if (st != 0) return;                             // (only state 0 left)
    // ---- STATE 0: cull-record init + size/box setup ----
    uint8_t area = c->mem_r8(0x800BF870u);
    int16_t tv = c->mem_r16s(0x800A4C94u + (uint32_t)area * 2);
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = (uint32_t)(int32_t)tv;
    c->engine.graphicsBind.recordInit();   // OWNED native graphics-bind (render-record alloc + geomblk resolve into node+0xC0)
    if (c->r[2] != 0) return;                        // init busy/failed -> EPI
    c->mem_w8 (obj + 0x2b, 0);
    c->mem_w32(obj + 0x14, 0);
    c->mem_w16(obj + 0x54, 0);
    c->mem_w16(obj + 0x58, 0);
    c->mem_w8 (obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // -> state 1
    area = c->mem_r8(0x800BF870u);
    uint16_t uv;
    if (area == 2 || area == 7 || area == 0x14) {
      c->mem_w8 (obj + 0, 1);
      c->mem_w16(obj + 0x80, 0xa0);
      c->mem_w16(obj + 0x82, 0x140);
      uv = 0xed;
    } else {
      if (c->mem_r8(0x800BF873u) == 0) c->mem_w8(obj + 0, 1);
      c->mem_w16(obj + 0x80, 300);
      c->mem_w16(obj + 0x82, 600);
      uv = 0xcb;
    }
    c->mem_w16(obj + 0x84, uv);
    c->mem_w16(obj + 0x86, uv);
    uint8_t n3 = c->mem_r8(obj + 3);
    switch (n3) {                                    // JT 0x80016B68 (node[3]-2)
      case 2:
        c->mem_w8 (obj + 8, 0);
        c->mem_w16(obj + 0x56, 0xc00);
        break;
      case 5: case 6: case 7: case 0x1d: case 0x1e:
        c->mem_w16(obj + 0x56, 0);
        break;
      case 8:
        c->mem_w8 (obj + 0xb, 0xf);
        break;
      case 0xc: case 0xd: case 0xe:
        c->mem_w16(obj + 0x56, 0x400);
        c->mem_w16(obj + 0x80, 0x3c);
        c->mem_w16(obj + 0x82, 0x78);
        c->mem_w16(obj + 0x84, 0x3e);
        c->mem_w8 (obj + 8, 0);
        c->mem_w16(obj + 0x86, 0x7c);
        break;
      case 0x11:
        if (c->mem_r16(0x800BFE56u) & 0x10)
          c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16(obj + 0x32) + 100));
        break;
      case 0x14:
        c->mem_w8 (obj + 8, 0);
        break;
      case 0x15: case 0x16: case 0x17: case 0x18:
        c->mem_w8 (obj + 0, 1);
        break;
      case 0x20:
        c->mem_w16(obj + 0x56, 0x4d0);
        break;
      default:
        break;
    }
    return;                                          // EPI
  }

  // ---- STATE 1: cull (result IGNORED), then the node[5] sub-state machine ----
  c->r[4] = obj; rec_dispatch(c, 0x8007778Cu);
  uint8_t sub = c->mem_r8(obj + 5);
  if (sub < 7) {                                     // JT 0x80016BE8 (node[5]); >=7 -> tail
    switch (sub) {
      case 4:                                        // jt[4]: enter active, then fall into case 0
        c->mem_w8(obj + 0, 1);
        c->mem_w8(obj + 5, 0);
        [[fallthrough]];
      case 0:                                        // jt[0]: on confirm, advance + per-type FUN_80040b48
        if (c->mem_r8(obj + 0x2b) == 3) {
          uint8_t n3 = c->mem_r8(obj + 3);
          c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
          if (n3 == 0xd)       confirm_set5(c, obj, 0x50);
          else if (n3 < 0xe) { if (n3 == 0xc) confirm_set5(c, obj, 0x4e); }
          else if (n3 == 0xe)  confirm_set5(c, obj, 0x4f);
        }
        break;
      case 1: case 5: {                              // jt[1]/jt[5]: pick scene id, FUN_8007e110 -> node+0x14
        int16_t sv4 = 0;
        uint8_t n3 = c->mem_r8(obj + 3);
        bool use_table;
        if (n3 != 2) {
          use_table = true;
        } else {
          sv4 = 0x16;
          if (c->mem_r8(0x800BF907u) != 0xff) {
            sv4 = 0x15;
            use_table = (c->mem_r8(0x800BF8C3u) == 0);
          } else {
            use_table = false;
          }
        }
        if (use_table) sv4 = c->mem_r16s(0x800A4CA8u + (uint32_t)n3 * 2);
        c->r[4] = (uint32_t)(int32_t)sv4; c->r[5] = 0;
        rec_dispatch(c, 0x8007E110u);
        c->mem_w32(obj + 0x14, c->r[2]);
        if (c->r[2] != 0) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
        break;
      }
      case 2: {                                      // jt[2]: pad-edge -> advance
        uint16_t v = (uint16_t)(c->mem_r16(0x800E7E68u) & c->mem_r16(0x1F800174u));
        if (v != 0) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
        break;
      }
      case 3: {                                      // jt[3]: release node+0x14, -> idle (state 2), advance
        uint32_t p = c->mem_r32(obj + 0x14);
        if (c->mem_r8(p + 4) < 2) {
          c->mem_w8 (p + 4, 2);
          c->mem_w32(obj + 0x14, 0);
        }
        c->mem_w8(obj + 0, 2);
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
        break;
      }
      case 6: {                                      // jt[6]: FUN_80042728 done -> node[5]=2
        rec_dispatch(c, 0x80042728u);
        if (c->r[2] != 0) c->mem_w8(obj + 5, 2);
        break;
      }
    }
  }

  // ---- tail (LAB switchD_80073ef8_caseD_7): special-area release, then render ----
  uint8_t cv = c->mem_r8(obj + 5);
  if (cv != 4) {
    uint8_t area = c->mem_r8(0x800BF870u);
    if (area == 2 || area == 7 || area == 0x14) {
      uint32_t p = c->mem_r32(obj + 0x14);
      if (p != 0 && c->mem_r8(0x800E7E85u) != 0x1f) {
        c->mem_w8 (p + 4, 2);
        c->mem_w32(obj + 0x14, 0);
        c->mem_w8 (obj + 0, 1);
        c->mem_w8 (obj + 5, 0);
      }
    }
  }
  c->mem_w8(obj + 0x2b, 0);
  c->r[4] = obj; c->engine.graphicsBind.renderUpdate();
}

void ov_beh_typed_init_scene_trigger(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("typed_init_scene_triggerverify") ? 1 : 0;
  if (!s_v) { beh_typed_init_scene_trigger(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_typed_init_scene_trigger(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[typed_init_scene_triggerverify] MISMATCH obj=%08x st=%u sub=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[typed_init_scene_triggerverify] %ld matches\n", ng);
}

}  // namespace

// Exported entry — the verify wrapper ov_beh_typed_init_scene_trigger is in the anonymous namespace above (internal linkage);
// the engine's per-object dispatch (engine_tomba2.cpp call_handler) calls THIS to run the owned behavior.
void ov_beh_typed_init_scene_trigger_run(Core* c) { ov_beh_typed_init_scene_trigger(c); }
