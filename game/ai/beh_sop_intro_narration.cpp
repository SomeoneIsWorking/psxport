// game/ai/beh_sop_intro_narration.cpp — SOP intro-cutscene per-object handler FUN_8010B990.
//
// Record 2 of the 3 scene actors Sop::fieldMode spawns at sm[0x50]==0 LOAD (game/scene/sop.cpp:505-514,
// table @0x8010C98C — this handler is record 2: model id 0x0F, initial world pos (X=0x0FA0, Y=0xEC78 =
// -5000 signed, Z=0x4650)). Only this actor gates its RUNNING body on the SOP scene-beat byte
// (0x800BF9B4 == 5, the narration VOID beat — the "dark 2D swirl + text" cutscene beat). During any
// other beat the actor is passive after init; during beat 5 it runs a tiny 2-phase spawn+scroll body
// that spawns a NARRATION prop (spawn type 0x2B) at a Z offset of +0x76C from this actor's position,
// then in phase 1 slowly scrolls its own Z by +10 per frame while ticking the animation/graphics leaves.
//
// State byte at node+4 (standard SM shape):
//   * 0 = INIT: try model attach 0x0F via FUN_800519E0. On success stamp node+0x3C = *0x800ECFA8
//     (per-area anim-alt table), install animation via FUN_80077C40(node, 0x8010D39C, 0), stamp
//     node+0x56 = 0x400 (facing), node+0x54 = 0x200 (size / half-extents), advance state to 1. If model
//     attach fails, retry next frame.
//   * 1 = RUNNING (gated on SCENE_BEAT == 5):
//         * sub-state at node+5 == 0 : build a 3-field spawn arg struct on the stack (X = node+0x2E,
//             Y = node+0x32, Z = node+0x36 + 0x76C) and spawn type 0x2B via FUN_8003116C. Advance
//             sub-state to 1.
//         * sub-state at node+5 == 1 : node+0x36 += 10 (slow Z creep), run animation/graphics leaves +
//             the post-cull visibility update + bounds cull. (Scroll direction matches the recomp: Z
//             increases; the recomp doesn't test overflow — the beat ends by external scene advance.)
//     For beats != 5 the running body is a no-op, matching the recomp's outermost gate.
//   * 2 or 3 = DESPAWN: standard pool return via Spawn::despawn (native). NB: unlike the other two
//     SOP intro handlers this one accepts BOTH 2 AND 3 as "cleanup" (recomp: `bVar1 < 4`).
//   * anything else: no-op.
//
// Ghidra decomp: scratch/decomp/sop_scene_actors.c (FUN_8010B990) + scratch/decomp/sop_intro_helpers.c
// (leaves).

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "core/engine.h"          // eng(c).spawn
#include "render/render.h"        // rend(c)->mNodeXform.buildWithOffset (FUN_800518FC)
#include "spawn.h"                 // eng(c).spawn.despawn (FUN_8007A624, native)
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN         = 0x8010B990u;
constexpr uint32_t SCENE_BEAT     = 0x800BF9B4u;   // shared SOP scene-beat byte (5 = narration void)
constexpr uint8_t  NARRATION_BEAT = 5;

constexpr uint32_t MODEL_ID       = 0x0Fu;
constexpr uint32_t MODEL_META_PTR = 0x800ECFA4u;   // per-area model-descriptor slot (alt of the "lifted" one)
constexpr uint32_t MODEL_DATA_PTR = 0x8010C9B0u;   // SOP-overlay model data
constexpr uint32_t G_ANIM_ALT_PTR = 0x800ECFA8u;   // per-area alt anim-table ptr (deref)
constexpr uint32_t ANIM_ENV_PTR   = 0x8010D39Cu;   // SOP-overlay anim env (arg 1 to FUN_80077C40)

constexpr uint32_t SPAWN_TYPE     = 0x2Bu;         // narration prop type (spawn.dispatch id)
constexpr int16_t  SPAWN_Z_OFFSET = 0x76C;         // narration prop is spawned +0x76C in Z from this actor
constexpr int16_t  Z_SCROLL_STEP  = 10;

// -- Substrate helpers -------------------------------------------------------------------------
inline int  try_model_attach(Core* c, uint32_t obj) {
  c->r[4] = obj; c->r[5] = MODEL_ID;
  c->r[6] = c->mem_r32(MODEL_META_PTR);
  c->r[7] = MODEL_DATA_PTR;
  rec_dispatch(c, 0x800519E0u);
  return (int)c->r[2];
}
inline void anim_env_setup(Core* c, uint32_t obj) {
  c->r[4] = obj; c->r[5] = ANIM_ENV_PTR; c->r[6] = 0;
  rec_dispatch(c, 0x80077C40u);
}
inline void anim_graphics_tick(Core* c, uint32_t o) { (void)eng(c).animTick(o); }                              // native FUN_8004190C
inline void post_cull_update  (Core* c, uint32_t o) { rend(c)->mNodeXform.buildWithOffset(o); }                // native FUN_800518FC (NodeXform::buildWithOffset)
inline void bounds_cull       (Core* c, uint32_t o) { c->r[4] = o; rec_dispatch(c, 0x8007778Cu); }

// Spawn a narration prop at (X, Y, Z+0x76C). FUN_8003116C(a0=type, a1=spawn-arg-block-ptr, a2=0) —
// stays substrate. We stage the block on the guest stack and restore sp after the dispatch.
//
// BLOCK LAYOUT (fixed 2026-07-10, prologue-vortex): ov_sop_gen_8010B990 stages the halfwords at
// sp+18/+22/+26 and passes a1 = sp+16 — i.e. the callee reads X at a1+2, Y at a1+6, Z at a1+10
// (stride-4 SVECTOR-style slots), NOT a packed +0/+2/+4 triple. The original packed layout fed
// FUN_8003116C garbage coordinates, so the narration prop (the vortex/text visual) spawned at a
// junk position.
void spawn_narration_prop(Core* c, uint32_t obj) {
  const uint32_t sp_save = c->r[29];
  const uint32_t ra_save = c->r[31];
  c->r[29] = sp_save - 0x30u;                          // mirrors the gen handler's own sp-48 frame
  const uint32_t blk = c->r[29] + 16u;                 // a1 = sp+16, as in the gen body

  const uint16_t sx = c->mem_r16(obj + 0x2E);
  const uint16_t sy = c->mem_r16(obj + 0x32);
  const int16_t  z  = (int16_t)c->mem_r16(obj + 0x36) + SPAWN_Z_OFFSET;

  c->mem_w16(blk + 0x02u, sx);                         // gen: sw16 @ sp+18
  c->mem_w16(blk + 0x06u, sy);                         // gen: sw16 @ sp+22
  c->mem_w16(blk + 0x0Au, (uint16_t)z);                // gen: sw16 @ sp+26

  c->r[4] = SPAWN_TYPE;
  c->r[5] = blk;
  c->r[6] = 0;
  rec_dispatch(c, 0x8003116Cu);                        // narration-prop spawn (substrate)

  c->r[29] = sp_save;
  c->r[31] = ra_save;
}

// -- State bodies ------------------------------------------------------------------------------
void state_init(Core* c, uint32_t obj) {
  if (try_model_attach(c, obj) != 0) return;

  c->mem_w32(obj + 0x3C, c->mem_r32(G_ANIM_ALT_PTR));
  anim_env_setup(c, obj);
  c->mem_w16(obj + 0x56, 0x400);
  c->mem_w16(obj + 0x54, 0x200);
  c->mem_w8 (obj + 4,    (uint8_t)(c->mem_r8(obj + 4) + 1));   // advance to RUNNING
}

void state_running(Core* c, uint32_t obj) {
  if (c->mem_r8(SCENE_BEAT) != NARRATION_BEAT) return;         // active only during the void beat

  const uint8_t sub = c->mem_r8(obj + 5);
  if (sub == 0) {
    spawn_narration_prop(c, obj);
    c->mem_w8(obj + 5, (uint8_t)(sub + 1));
    return;
  }
  if (sub == 1) {
    const int16_t z = (int16_t)c->mem_r16(obj + 0x36);
    c->mem_w16(obj + 0x36, (uint16_t)(z + Z_SCROLL_STEP));
    anim_graphics_tick(c, obj);
    post_cull_update(c, obj);
    bounds_cull(c, obj);
  }
}

}  // namespace

void beh_sop_intro_narration(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t  st  = c->mem_r8(obj + 4);
  if (st == 1)      state_running(c, obj);
  else if (st == 0) state_init   (c, obj);
  else if (st == 2 || st == 3) eng(c).spawn.despawn(obj);   // recomp: bVar1 < 4
  // else: no-op
}
