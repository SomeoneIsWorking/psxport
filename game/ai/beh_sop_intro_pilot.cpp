// game/ai/beh_sop_intro_pilot.cpp — SOP intro-cutscene per-object handler FUN_8010ACFC.
//
// One of the THREE scene actors Sop::fieldMode spawns at sm[0x50]==0 LOAD (game/scene/sop.cpp:505-514,
// table @0x8010C98C — this handler is record 0: model id 0x11, initial world pos (X=0x0FD2, Y=0xFA24,
// Z=0x4602)). Dispatched every field-mode frame by ObjectList::walkList2 through BehaviorDispatch. This
// actor is the CUTSCENE-SCRIPT-DRIVEN one — its per-frame motion comes from the cutscene VM
// (ScriptInterp::step, FUN_80041098) rather than a hand-rolled state body, and it also drives the shared
// scene-master state block G (0x800E7E80) via FUN_80067DA8 on every init.
//
// State byte at node+4 (standard SM shape):
//   * 0 = INIT: master-G bookkeeping FUN_80067DA8(0x800E7E80); try model attach 0x11 via FUN_800519E0.
//     On success stamp node+0x47=0xFF, node+0x3C = *0x800ECF68 (per-area anim table), advance state,
//     seed anim (FUN_80040CDC → 0x8010CA28), start walk (FUN_80054D14 mode 8), stamp node+0x70=2 /
//     node+0x84=0x8C, and fire the one-shot SOP-overlay init helper FUN_8010AE30. If model attach
//     fails the whole init retries next frame.
//   * 1 = RUNNING: visibility check (Actor::boundsCull); if visible, run post-cull update FUN_800518FC.
//     Then always tick the script VM (ScriptInterp::step — native) and the animation/graphics
//     leaf FUN_8004190C.
//   * 3 = DESPAWN: standard pool return via Spawn::despawn (native).
//   * anything else: no-op (matches the recomp's `bVar1 != 2 && bVar1 == 3` guard).
//
// Ownership model (same as beh_scene_ui_trigger): CONTROL FLOW + node writes owned native; every
// sub-behavior CALL stays reachable via rec_dispatch OR routes to its owned equivalent. Ghidra decomp:
// scratch/decomp/sop_scene_actors.c (FUN_8010ACFC) + scratch/decomp/sop_intro_helpers.c (leaves).

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "core/engine.h"          // eng(c).script / eng(c).spawn
#include "object/actor.h"          // Actor::boundsCull (FUN_8007778C, native)
#include "render/render.h"        // rend(c)->mNodeXform.buildWithOffset (FUN_800518FC)
#include "spawn.h"                 // eng(c).spawn.despawn (FUN_8007A624, native)
void rec_dispatch(Core*, uint32_t);
uint32_t native_sop_overlay_shadow_spawn(Core* c, uint32_t parent);   // FUN_8010AE30, native (sop_overlay_shadow.cpp)

namespace {

constexpr uint32_t BEH_FN         = 0x8010ACFCu;
constexpr uint32_t MASTER_G       = 0x800E7E80u;   // scene-master state block (camera/world anchor)
constexpr uint32_t G_MODEL_TABLE  = 0x800ED014u;   // per-area model-descriptor table ptr (deref)
constexpr uint32_t G_ANIM_TABLE   = 0x800ECF68u;   // per-area animation-table ptr (deref)

constexpr uint32_t MODEL_ID       = 0x11u;
constexpr uint32_t MODEL_META_PTR = 0x800A3FA8u;   // meta record (arg 3 to FUN_800519E0 model attach)
constexpr uint32_t ANIM_ENV_PTR   = 0x80017FE8u;   // resident anim env  (arg 1 to FUN_80040CDC)
constexpr uint32_t ANIM_DATA_PTR  = 0x8010CA28u;   // SOP-overlay anim data (arg 2 to FUN_80040CDC)

// -- Substrate helpers kept dispatched (their leaves are their own future frontier) -------------
// FUN_80067DA8 = Engine::uploadModeSprites (mode-selected VRAM upload; ignores the a0 the guest
// caller passes — MASTER_G stays as documentation of the recomp call site).
inline void master_g_tick   (Core* c) { eng(c).uploadModeSprites(); (void)MASTER_G; }
inline int  try_model_attach(Core* c, uint32_t obj) {
  c->r[4] = obj; c->r[5] = MODEL_ID;
  c->r[6] = c->mem_r32(G_MODEL_TABLE);
  c->r[7] = MODEL_META_PTR;
  rec_dispatch(c, 0x800519E0u);
  return (int)c->r[2];               // 0 = success, non-zero = retry
}
inline void anim_env_setup   (Core* c, uint32_t obj) { eng(c).script.init(obj, ANIM_ENV_PTR, ANIM_DATA_PTR); }    // FUN_80040CDC = ScriptInterp::init (init obj as script-driven: tableA=ANIM_ENV_PTR, script=ANIM_DATA_PTR)
inline void walk_start       (Core* c, uint32_t obj) { eng(c).walkStart   (obj, 8, 0); }                        // native FUN_80054D14
inline void overlay_oneshot  (Core* c, uint32_t obj)  { (void)native_sop_overlay_shadow_spawn(c, obj); }
inline int  bounds_cull      (Core* c, uint32_t obj) { c->r[4] = obj;             rec_dispatch(c, 0x8007778Cu); return (int)c->r[2]; }
inline void post_cull_update (Core* c, uint32_t obj) { rend(c)->mNodeXform.buildWithOffset(obj); }              // native FUN_800518FC (NodeXform::buildWithOffset)
inline void anim_graphics_tick(Core* c, uint32_t obj){ (void)eng(c).animTick(obj); }                            // native FUN_8004190C

// -- State bodies ------------------------------------------------------------------------------
void state_init(Core* c, uint32_t obj) {
  master_g_tick(c);
  if (try_model_attach(c, obj) != 0) return;                  // model not ready — retry next frame

  c->mem_w8 (obj + 0x47, 0xFF);
  c->mem_w32(obj + 0x3C, c->mem_r32(G_ANIM_TABLE));
  c->mem_w8 (obj + 4,    (uint8_t)(c->mem_r8(obj + 4) + 1));   // advance to RUNNING

  anim_env_setup(c, obj);
  walk_start(c, obj);

  c->mem_w8 (obj + 0x70, 2);
  c->mem_w16(obj + 0x84, 0x8C);

  overlay_oneshot(c, obj);
}

void state_running(Core* c, uint32_t obj) {
  if (bounds_cull(c, obj) != 0) post_cull_update(c, obj);
  eng(c).script.step(obj);                                  // native (was rec_dispatch 0x80041098)
  anim_graphics_tick(c, obj);
}

}  // namespace

void beh_sop_intro_pilot(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t  st  = c->mem_r8(obj + 4);
  if (st == 1)      state_running(c, obj);
  else if (st == 0) state_init   (c, obj);
  else if (st == 3) eng(c).spawn.despawn(obj);
  // else: no-op
}
