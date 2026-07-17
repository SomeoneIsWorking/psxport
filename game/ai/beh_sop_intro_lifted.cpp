// game/ai/beh_sop_intro_lifted.cpp — SOP intro-cutscene per-object handler FUN_8010B798.
//
// Record 1 of the 3 scene actors Sop::fieldMode spawns at sm[0x50]==0 LOAD (game/scene/sop.cpp:505-514,
// table @0x8010C98C — this handler is record 1: model id 0x0F, initial world pos (X=0x122A, Y=0xFA56,
// Z=0x44D6)). Model 0x0F attached via FUN_800519E0 with the alternate anim-descriptor path FUN_80077C40
// (register-based signature instead of table+data). On INIT this actor snaps its own Y position UP by
// 0x60 (visual "lift" — hence the file name) and latches node+3=1, a per-actor flag another SOP-overlay
// leaf (0x8010B588, the sub-behavior tick) reads on the RUNNING state.
//
// State byte at node+4 (standard SM shape):
//   * 0 = INIT: try model attach 0x0F via FUN_800519E0. On success stamp node+0x3C = *0x800ECF68
//     (per-area anim table), install animation via FUN_80077C40(node, 0x80017FE8, 2), stamp
//     node+0x84 = 0x60, node+3 = 1, advance state to 1, lift Y (node+0x32 -= 0x60), and fire the
//     one-shot SOP-overlay init helper FUN_8010AE30. If model attach fails, retry next frame.
//   * 1 = RUNNING: visibility check (Actor::boundsCull — result unused here, matches the recomp);
//     run the SOP-overlay sub-tick FUN_8010B588 (the actor-specific per-frame behavior); then the
//     animation/graphics leaf FUN_8004190C; then post-cull visibility update FUN_800518FC.
//   * 3 = DESPAWN: standard pool return via Spawn::despawn (native).
//   * anything else: no-op (matches the recomp's `bVar1 != 2 && bVar1 == 3` guard).
//
// Ghidra decomp: scratch/decomp/sop_scene_actors.c (FUN_8010B798) + scratch/decomp/sop_intro_helpers.c
// (leaves). Faithful to the recomp — sub-behavior calls stay dispatched.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "core/engine.h"          // eng(c).spawn
#include "render/render.h"        // rend(c)->mNodeXform.buildWithOffset (FUN_800518FC)
#include "spawn.h"                 // eng(c).spawn.despawn (FUN_8007A624, native)
void rec_dispatch(Core*, uint32_t);
uint32_t native_sop_overlay_shadow_spawn(Core* c, uint32_t parent);   // FUN_8010AE30, native (sop_overlay_shadow.cpp)

namespace {

constexpr uint32_t BEH_FN         = 0x8010B798u;
constexpr uint32_t G_ANIM_TABLE   = 0x800ECF68u;   // per-area animation-table ptr (deref)

constexpr uint32_t MODEL_ID       = 0x0Fu;
constexpr uint32_t MODEL_META_PTR = 0x800ECFA0u;   // per-area model-descriptor slot
constexpr uint32_t MODEL_DATA_PTR = 0x8010CB08u;   // SOP-overlay model data
constexpr uint32_t ANIM_ENV_PTR   = 0x80017FE8u;   // resident anim env (arg 1 to FUN_80077C40)

constexpr int16_t  Y_LIFT         = 0x60;          // node+0x32 -= 0x60 on init (visual lift)

// -- Substrate helpers (their leaves stay their own future frontier) ---------------------------
inline int  try_model_attach(Core* c, uint32_t obj) {
  c->r[4] = obj; c->r[5] = MODEL_ID;
  c->r[6] = c->mem_r32(MODEL_META_PTR);
  c->r[7] = MODEL_DATA_PTR;
  rec_dispatch(c, 0x800519E0u);
  return (int)c->r[2];
}
inline void anim_env_setup(Core* c, uint32_t obj) {
  c->r[4] = obj; c->r[5] = ANIM_ENV_PTR; c->r[6] = 2;
  rec_dispatch(c, 0x80077C40u);
}
inline void overlay_oneshot   (Core* c, uint32_t obj) {                        (void)native_sop_overlay_shadow_spawn(c, obj); }
// FUN_8010B588 (sopLiftedSubtick, sop_intro_events.cpp) is VERIFIED + WIRED (2026-07-10, frontier
// convergence pass — docs/findings/scene.md). It exposed a pre-existing ScriptInterp::step
// divergence (obj+0x71 flags byte, RET_PAUSE mask mistranscribed as 0x02 instead of 0x01) on THIS
// specific SOP intro script content — now fixed in game/scene/script_interp.cpp. This call site
// stays on rec_dispatch(c, ...) per CLAUDE.md's dispatch-routing preference; it now transparently
// runs the native body since sopLiftedSubtick is installed in the shared override registry.
inline void overlay_subtick   (Core* c, uint32_t o) { c->r[4] = o;           rec_dispatch(c, 0x8010B588u); }
inline void bounds_cull       (Core* c, uint32_t o) { c->r[4] = o;           rec_dispatch(c, 0x8007778Cu); }
inline void anim_graphics_tick(Core* c, uint32_t o) { (void)eng(c).animTick(o); }                              // native FUN_8004190C
inline void post_cull_update  (Core* c, uint32_t o) { rend(c)->mNodeXform.buildWithOffset(o); }                // native FUN_800518FC (NodeXform::buildWithOffset)

// -- State bodies ------------------------------------------------------------------------------
void state_init(Core* c, uint32_t obj) {
  if (try_model_attach(c, obj) != 0) return;

  c->mem_w32(obj + 0x3C, c->mem_r32(G_ANIM_TABLE));
  anim_env_setup(c, obj);
  c->mem_w16(obj + 0x84, 0x60);
  c->mem_w8 (obj + 3,    1);
  c->mem_w8 (obj + 4,    (uint8_t)(c->mem_r8(obj + 4) + 1));   // advance to RUNNING

  const int16_t y = (int16_t)c->mem_r16(obj + 0x32);
  c->mem_w16(obj + 0x32, (uint16_t)(y - Y_LIFT));              // visual lift

  overlay_oneshot(c, obj);
}

void state_running(Core* c, uint32_t obj) {
  bounds_cull(c, obj);                                          // result unused here (faithful)
  overlay_subtick(c, obj);
  anim_graphics_tick(c, obj);
  post_cull_update(c, obj);
}

}  // namespace

void beh_sop_intro_lifted(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t  st  = c->mem_r8(obj + 4);
  if (st == 1)      state_running(c, obj);
  else if (st == 0) state_init   (c, obj);
  else if (st == 3) eng(c).spawn.despawn(obj);
  // else: no-op
}
