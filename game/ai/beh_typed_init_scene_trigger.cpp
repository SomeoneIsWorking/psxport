// game/ai/beh_typed_init_scene_trigger.cpp — PC-native BEHAVIOR handler FUN_80073CD8.
//
// SEMANTIC MODEL (RE'd from disas 0x80073CD8 + sibling comparison):
//   The SECOND resident, generic per-object behavior routine the field OBJECT-PLACEMENT driver
//   (FUN_80072A78, ov_place_objects) installs at node+0x1c. Sibling of FUN_800739AC / FUN_80133C14
//   with the same state-machine shape on Actor::state (0 init / 1 active / 2 idle / 3 despawn), but
//   with a nested TRIGGER SUB-STATE machine on Actor::triggerSub for the SCENE/UI push side:
//     - state 0 (INIT):   allocate a cull record + area-keyed size (recordInit with the area-specific
//                         cls param from TBL_A4C94[area]), then per-type (Actor::type()) seed box
//                         dimensions and rotY preset from a JT at 0x80016B68.
//     - state 1 (ACTIVE): cull tick (FUN_8007778C, result IGNORED); then dispatch triggerSub 0..6
//                         (JT at 0x80016BE8) which drives a PUSH-INTO-SCENE / area-transition
//                         handshake via three sub-behaviors (FUN_80040B48 confirm, FUN_8007E110
//                         scene-entity spawn -> Actor::sceneHandle, FUN_80042728 completion). Tail
//                         handles the special-area sceneHandle release + calls renderUpdate.
//     - state 2:          idle (no-op).
//     - state 3:          despawn.
//
// Fields the handler reads/writes are all named on class Actor (game/object/actor.h): state /
// type / alive / triggerSub / sceneHandle / subFlag / rotX / rotY / rotZ / oscBase / boxX/Y/Z/W. NB:
// unlike FUN_80133C14, this handler CONSUMES subFlag == 3 as a specific "confirm-pending" signal
// (not the ±nonzero turn trigger the background-actor tick reads); Actor::subFlag's docstring
// records both consumers.
//
// STILL OPAQUE (rec_dispatch by address until each is RE'd on its own arc):
//   - (FUN_80040B48  SCENE-EVENT-ARM — NOW NATIVE via SceneEvents::arm on Engine (scene_events.cpp).
//                    Bundles the FUN_80040A58 size-class helper as SceneEvents::classSize (public so
//                    the multiple substrate callers of func_80040A58 also share it). Six callsites
//                    all migrated: entity.cpp (arg=56), beh_typed_init_exit_poker (arg=5),
//                    beh_pickup_collect_trigger (0x39/0x3a — SFX), beh_area_transition_machine
//                    (0x42), and confirm_or_advance here (0x50/0x4E/0x4F per-type). Gated
//                    `sceneeventsarmverify`.
//   - (FUN_8007E110  SCENE-ENTITY SPAWN — NOW NATIVE via Spawn::sceneEntity, spawn.cpp. Inlined
//                    FUN_8007A5A8 (class-3 list-1 tail-insert allocator) into the same method.
//                    The per-frame handler FUN_8007DDE0 stays reachable via its address stored at
//                    node[+0x1C]. Gated `sceneentityverify`. Returns node ptr on success, 0 on
//                    freelist exhaustion (the earlier "returns 2 on fail" note in commit 0056101
//                    was wrong — the fail path's v0=2 in the branch-delay slot is overwritten by
//                    the j-delay's `addu v0, 0, 0` before reaching ret.)
//   - 0x800A4C94     area-keyed CULL-RECORD SIZE table (halfword, indexed by area)
//   - 0x800A4CA8     per-type SCENE-ID table (halfword, indexed by type)
//   - 0x800E7E68     pad-edge bitmask (state 1 case 2 — new-frame padedge)
//   - 0x1F800174     current pad state (masked against pad-edge for triggerSub advance)
//
// OWNERSHIP RULES (per project CLAUDE.md): CONTROL FLOW + every named-field write owned native,
// byte-for-byte; every still-opaque sub-behavior CALL stays a `rec_dispatch` leaf (a0/a1 set first).
// NO GTE, NO render packets here. Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) via
// channel "typed_init_scene_triggerverify".

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "graphics_bind.h"    // GraphicsBind::recordInit / renderUpdate
#include "spawn.h"            // class Spawn (eng(c).spawn.despawn)
#include "bg_scene_transition_sm.h"   // BgSceneTransitionSm::readyForProgress (FUN_80042728 native)
#include "object/actor.h"     // class Actor + named fields
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {
constexpr uint32_t BEH_FN = 0x80073CD8u;

enum class Sta : uint8_t { Init = 0, Active = 1, Idle = 2, Despawn = 3 };

// Un-RE'd sub-behaviors still called via rec_dispatch.
// (FUN_8007778C bounds-cull wrapper is NATIVE now via Actor::boundsCull; the 5-way FUN_8007712C
//  body it dispatches remains opaque and stays a rec_dispatch inside boundsCull.)
// (FUN_80040B48 = SceneEvents::arm — NOW NATIVE, callsites use eng(c).sceneEvents.arm.)
// (FUN_8007E110 = Spawn::sceneEntity — NOW NATIVE, callsites use eng(c).spawn.sceneEntity.)

// Data-table addresses (halfword strides, indexed by area/type).
constexpr uint32_t TBL_AREA_SIZE  = 0x800A4C94u;   // per-area cull-record size (state-0 recordInit cls)
constexpr uint32_t TBL_SCENE_ID   = 0x800A4CA8u;   // per-type scene-id passed to Spawn::sceneEntity

// Global inputs consulted by triggerSub cases.
constexpr uint32_t CUR_AREA         = 0x800BF870u;   // current field area byte
constexpr uint32_t AREA_MODE_BYTE   = 0x800BF873u;   // area-mode gate for the non-special-area alive() init
constexpr uint32_t SCENE_MASK_HW    = 0x800BFE56u;   // scene-mask halfword (bit 0x10 gates the type-0x11 oscBase bump)
constexpr uint32_t SCENE_STATE_87   = 0x800BF907u;   // pad-scene state (0xFF sentinel for the "no override" path)
constexpr uint32_t SCENE_STATE_43   = 0x800BF8C3u;   // pad-scene sub-state (== 0 gates use_table)
constexpr uint32_t PAD_EDGE_HW      = 0x800E7E68u;   // new-frame pad-edge bitmask
constexpr uint32_t PAD_CUR_HW       = 0x1F800174u;   // current pad state
constexpr uint32_t SPECIAL_AREA_TAG = 0x800E7E85u;   // guard on the special-area sceneHandle release (0x1F skips)

// The confirm helper flow (state-1 triggerSub==0 branch): arm the event; if the arm succeeded
// (fresh arm = 1, or events gate off = -1 → both nonzero), skip to triggerSub=5. Already-armed = 0
// means retry next tick.
inline void confirm_or_advance(Actor& a, uint32_t arg) {
  if (eng(a.core()).sceneEvents.arm((uint8_t)arg) != 0) a.setTriggerSub(5);
}

// Special-area set for two paths (state-0 alive() + box override, tail sceneHandle release):
inline bool is_special_area(uint8_t area) { return area == 2 || area == 7 || area == 0x14; }
}  // namespace

void beh_typed_init_scene_trigger(Core* c) {
  Actor a(c, c->r[4]);
  const Sta st = (Sta)a.state();

  if (st != Sta::Active) {
    if ((uint8_t)st >= (uint8_t)Sta::Idle) {
      if (st == Sta::Idle) return;                               // state 2: idle no-op
      if (st != Sta::Despawn) return;                            // state >=4: no-op
      eng(c).spawn.despawn(a.addr());                         // state 3: despawn
      return;
    }
    if (st != Sta::Init) return;

    // ---- STATE 0 (INIT): cull-record alloc with area-keyed size, then per-type seeding ----------
    uint8_t area = c->mem_r8(CUR_AREA);
    int16_t sz   = (int16_t)c->mem_r16(TBL_AREA_SIZE + (uint32_t)area * 2);
    c->r[4] = a.addr(); c->r[5] = 0xc; c->r[6] = (uint32_t)(int32_t)sz;
    eng(c).graphicsBind.recordInit();
    if (c->r[2] != 0) return;                                    // init busy/failed

    a.setSubFlag(0);
    a.setSceneHandle(0);
    a.setRotX(0);
    a.setRotZ(0);
    a.setState((uint8_t)(a.state() + 1));                        // -> ACTIVE

    // Area-driven box + alive() seed.
    area = c->mem_r8(CUR_AREA);
    uint16_t uv;
    if (is_special_area(area)) {
      a.setAlive(1);
      a.setBoxX(0xa0);
      a.setBoxY(0x140);
      uv = 0xed;
    } else {
      if (c->mem_r8(AREA_MODE_BYTE) == 0) a.setAlive(1);
      a.setBoxX(300);
      a.setBoxY(600);
      uv = 0xcb;
    }
    a.setBoxZ(uv);
    a.setBoxW(uv);

    // Per-type seed (guest JT 0x80016B68 on type - 2).
    switch (a.type()) {
      case 2:
        c->mem_w8(a.addr() + 8, 0);       // obj+8: un-RE'd byte (kept as raw)
        a.setRotY(0xc00);
        break;
      case 5: case 6: case 7: case 0x1d: case 0x1e:
        a.setRotY(0);
        break;
      case 8:
        c->mem_w8(a.addr() + 0xb, 0xf);   // obj+0xb: un-RE'd byte (kept as raw)
        break;
      case 0xc: case 0xd: case 0xe:
        a.setRotY(0x400);
        a.setBoxX(0x3c);
        a.setBoxY(0x78);
        a.setBoxZ(0x3e);
        c->mem_w8(a.addr() + 8, 0);
        a.setBoxW(0x7c);
        break;
      case 0x11:
        if (c->mem_r16(SCENE_MASK_HW) & 0x10)
          a.setOscBase((uint16_t)(a.oscBase_u() + 100));
        break;
      case 0x14:
        c->mem_w8(a.addr() + 8, 0);
        break;
      case 0x15: case 0x16: case 0x17: case 0x18:
        a.setAlive(1);
        break;
      case 0x20:
        a.setRotY(0x4d0);
        break;
      default:
        break;
    }
    return;
  }

  // ---- STATE 1 (ACTIVE): cull tick (result ignored), then triggerSub dispatch ------------------
  a.boundsCull();                                                // FUN_8007778C (thin wrapper native); result ignored per RE

  uint8_t sub = a.triggerSub();
  if (sub < 7) {                                                 // JT 0x80016BE8
    switch (sub) {
      case 4:                                                    // enter-active → fall into confirm-pending
        a.setAlive(1);
        a.setTriggerSub(0);
        [[fallthrough]];
      case 0:                                                    // confirm-pending: subFlag==3 → advance
        if (a.subFlag() == 3) {
          uint8_t n3 = a.type();
          a.setTriggerSub((uint8_t)(a.triggerSub() + 1));
          if      (n3 == 0xd)  confirm_or_advance(a, 0x50);
          else if (n3 < 0xe) { if (n3 == 0xc) confirm_or_advance(a, 0x4e); }
          else if (n3 == 0xe)  confirm_or_advance(a, 0x4f);
        }
        break;
      case 1: case 5: {                                          // pick scene-id → Spawn::sceneEntity(sid, 0) → sceneHandle
        int16_t sv4 = 0;
        uint8_t n3  = a.type();
        bool use_table;
        if (n3 != 2) {
          use_table = true;
        } else {
          sv4 = 0x16;
          if (c->mem_r8(SCENE_STATE_87) != 0xff) {
            sv4 = 0x15;
            use_table = (c->mem_r8(SCENE_STATE_43) == 0);
          } else {
            use_table = false;
          }
        }
        if (use_table) sv4 = (int16_t)c->mem_r16(TBL_SCENE_ID + (uint32_t)n3 * 2);
        // Spawn::sceneEntity — was rec_dispatch(0x8007E110); now native (spawn.cpp).
        uint32_t node = eng(c).spawn.sceneEntity((uint16_t)(int16_t)sv4, /*subtype=*/0);
        a.setSceneHandle(node);
        if (node != 0) a.setTriggerSub((uint8_t)(a.triggerSub() + 1));
        break;
      }
      case 2: {                                                  // pad-edge → advance
        uint16_t v = (uint16_t)(c->mem_r16(PAD_EDGE_HW) & c->mem_r16(PAD_CUR_HW));
        if (v != 0) a.setTriggerSub((uint8_t)(a.triggerSub() + 1));
        break;
      }
      case 3: {                                                  // release sceneHandle → idle
        uint32_t handle = a.sceneHandle();
        if (c->mem_r8(handle + 4) < 2) {
          c->mem_w8(handle + 4, 2);                              // release: linked entity → state 2
          a.setSceneHandle(0);
        }
        a.setAlive(2);
        a.setTriggerSub((uint8_t)(a.triggerSub() + 1));
        break;
      }
      case 6: {                                                  // completion → triggerSub = 2
        if (eng(c).bgSceneTransitionSm.readyForProgress()) a.setTriggerSub(2);   // FUN_80042728 (native)
        break;
      }
    }
  }

  // ---- tail: special-area sceneHandle release, then per-frame renderUpdate ---------------------
  if (a.triggerSub() != 4) {
    uint8_t area = c->mem_r8(CUR_AREA);
    if (is_special_area(area)) {
      uint32_t handle = a.sceneHandle();
      if (handle != 0 && c->mem_r8(SPECIAL_AREA_TAG) != 0x1f) {
        c->mem_w8(handle + 4, 2);
        a.setSceneHandle(0);
        a.setAlive(1);
        a.setTriggerSub(0);
      }
    }
  }
  a.setSubFlag(0);
  c->r[4] = a.addr(); eng(c).graphicsBind.renderUpdate();
}
