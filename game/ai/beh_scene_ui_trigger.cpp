// game/ai/beh_scene_ui_trigger.cpp — PC-native per-object BEHAVIOR handler FUN_800739AC.
//
// One of the resident, GENERIC per-object behavior routines the field OBJECT-PLACEMENT DRIVER
// (FUN_80072A78, ov_place_objects) installs at node+0x1c. The seaside field installs it on its type-01
// objects; it is called every frame by the entity walk (ov_entity_walk_7a904) with the node in a0. It is
// a small state machine on the node's state byte node[4] (0 init / 1 active / 2 idle / 3 despawn) with a
// 6-way sub-state machine on node[5] (jump table 0x80016B50) for the active state — a scene/UI TRIGGER:
// on confirm it pushes sub-state into 0x800BF871 + calls the area transition FUN_800782F0, plays SFX
// (FUN_80074590), seeds camera/save globals (case 3), etc.
//
// Ownership model (same as actor_sm_24448 / script_vm): CONTROL FLOW + node/global memory writes are
// owned native; every sub-behavior CALL stays reachable by address via rec_dispatch (each honors its own
// override identically). NO GTE, NO render packets here. RE'd 1:1 from disas 0x800739AC (+ jump table
// 0x80016B50 = {b20,b60,bbc,c1c,c90,b14}). It WRITES guest node state the still-recomp content reads →
// content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs rec_super_call). The IDLE field path
// (state0 init, then state1 cull→render with node[5]==0 / node[0x2b]!=3) is fully exercised by the gate;
// the input-driven transition sub-states (node[5] 1..5, the warp/confirm paths) are faithfully
// transcribed and verify when a scene drives them (same caveat as the camera alt-mode orchestrators).

#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C — thin wrapper native)
#include "cfg.h"
#include "graphics_bind.h"   // ov_obj_record_init — native graphics-bind (game/world)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x800739ACu;
constexpr uint32_t S1     = 0x800E7E80u;   // the prologue's s1 base (0x800e0000 + 0x7e80)

// Set node[0x2b]=0 and dispatch the per-object render-state update FUN_800517F8 (owned). Every active-state
// path converges here (labels ca4 → ca8); only the despawn path and the cull-miss early-out skip it.
inline void render_and_return(Core* c, uint32_t obj) {
  c->mem_w8(obj + 0x2b, 0);
  c->r[4] = obj;
  eng(c).graphicsBind.renderUpdate();
}

// LAB_80073be0 — shared "SFX + advance" tail of cases 1/2: node[5]++, FUN_80074590(0x11, 0, 0).
inline void sfx_advance(Core* c, uint32_t obj) {
  uint8_t v0 = c->mem_r8(obj + 5);
  c->mem_w8(obj + 5, (uint8_t)(v0 + 1));
  eng(c).sfx.trigger(0x11, 0, 0);              // FUN_80074590 (native)
}

}  // namespace

void beh_scene_ui_trigger(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t st = c->mem_r8(obj + 4);

  if (st != 1) {
    if (st >= 2) {                                   // state 2 (idle) / 3 (despawn) / other
      if (st == 3) { eng(c).spawn.despawn(obj); }  // despawn
      return;                                        // EPI
    }
    if (st != 0) return;                             // (dead path; only 0 left here)
    // ---- STATE 0: cull-record init + size/box setup ----
    uint8_t area = c->mem_r8(0x800BF870u);
    int16_t tv = c->mem_r16s(0x800A4C80u + (uint32_t)area * 2);
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = (uint32_t)(int32_t)tv;
    eng(c).graphicsBind.recordInit();   // OWNED native graphics-bind (render-record alloc + geomblk resolve into node+0xC0)
    if (c->r[2] != 0) return;                        // init busy/failed -> EPI
    c->mem_w8(obj + 0, 1);
    c->mem_w8(obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // -> state 1
    area = c->mem_r8(0x800BF870u);
    if (area == 2 || area == 7 || area == 0x14) { c->mem_w16(obj + 0x80, 0xa0); c->mem_w16(obj + 0x82, 0x140); }
    else                                        { c->mem_w16(obj + 0x80, 300);  c->mem_w16(obj + 0x82, 600);   }
    c->mem_w16(obj + 0x84, 0xed); c->mem_w16(obj + 0x86, 0xed);
    c->mem_w8(obj + 0x2b, 0);
    area = c->mem_r8(0x800BF870u);
    if (area == 5 && c->mem_r8(obj + 3) == 5) c->mem_w8(obj + 0xb, 0xf);
    return;                                          // EPI
  }

  // ---- STATE 1: cull, then the node[5] sub-state machine ----
  Actor(c, obj).boundsCull();                        // FUN_8007778C — Actor::boundsCull (thin wrapper native)
  if (c->r[2] == 0) { c->mem_w8(obj + 0x2b, 0); return; }   // culled -> no render
  c->mem_w8(obj + 0x2b, 0);
  uint8_t sub = c->mem_r8(obj + 5);
  if (sub >= 6) { render_and_return(c, obj); return; }      // sub>=6 -> ca4/render

  switch (sub) {
    case 5:                                          // jt[5]=0x80073b14: enter active, then shared tail
      c->mem_w8(obj + 0, 1);
      c->mem_w8(obj + 5, 0);
      // fall through to the case-0 shared tail (0x80073b20)
    case 0:                                          // jt[0]=0x80073b20: confirm -> push sub-state + warp
      if (c->mem_r8(obj + 0x2b) == 3) {
        uint8_t v0 = c->mem_r8(obj + 5), v1 = c->mem_r8(obj + 3);
        c->mem_w8(obj + 5, (uint8_t)(v0 + 1));
        c->mem_w8(0x800BF871u, v1);
        eng(c).sceneTransition.areaMaskTrigger(c->mem_r8(0x800BF870u), (uint8_t)v1);   // was rec_dispatch 0x800782F0
      }
      break;
    case 1:                                          // jt[1]=0x80073b60
      {
        uint8_t area = c->mem_r8(0x800BF870u);
        bool to_ca0 = (area == 2 || area == 7) && c->mem_r8(0x800E7E85u) != 0x1f;
        if (to_ca0) { c->mem_w8(obj + 5, 4); break; } // ca0: node[5]=4 -> render
        c->r[4] = c->mem_r8(obj + 3); rec_dispatch(c, 0x800737F8u);
        if (c->mem_r16(0x800E7E68u) & 0x2000) sfx_advance(c, obj);   // confirm edge -> SFX + advance
      }
      break;
    case 2:                                          // jt[2]=0x80073bbc
      {
        rec_dispatch(c, 0x800738B0u);
        uint16_t pad = c->mem_r16(0x800E7E68u);
        if (pad & 0x4000) { sfx_advance(c, obj); }
        else if (pad & 0x2000) { c->mem_w8(obj + 5, 4); c->r[4] = 1; rec_dispatch(c, 0x80074BF8u); }
      }
      break;
    case 3:                                          // jt[3]=0x80073c1c: seed camera/save globals + FUN_8005082C
      {
        c->mem_w8(0x1F800136u, 2);
        c->mem_w8(0x800BF84Au, 0);
        uint32_t a2 = c->mem_r32(S1 + 0x2c), a3 = c->mem_r32(S1 + 0x30), t0v = c->mem_r32(S1 + 0x34);
        uint8_t  t1 = c->mem_r8(S1 + 0x2a);
        uint32_t taskp = c->mem_r32(0x1F800138u);
        c->mem_w32(0x800BF890u, a2);
        c->mem_w32(0x800BF894u, a3);
        c->mem_w32(0x800BF898u, t0v);
        c->mem_w8 (0x800BFE38u, t1);
        c->mem_w16(taskp + 0x50, 0);
        c->mem_w8 (taskp + 0x6b, 8);
        uint8_t v0 = c->mem_r8(obj + 5);
        c->mem_w8(obj + 5, (uint8_t)(v0 + 1));
        eng(c).modeStateArm.arm();                  // native — was rec_dispatch 0x8005082C(0,0,0)
      }
      break;
    case 4:                                          // jt[4]=0x80073c90: -> state 2, advance sub
      {
        uint8_t v0 = c->mem_r8(obj + 5);
        c->mem_w8(obj + 0, 2);
        c->mem_w8(obj + 5, (uint8_t)(v0 + 1));
      }
      break;
  }
  render_and_return(c, obj);
}
