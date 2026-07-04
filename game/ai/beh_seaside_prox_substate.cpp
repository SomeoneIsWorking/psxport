// game/ai/beh_seaside_prox_substate.cpp — per-object behavior handler FUN_8013C1DC (A00 overlay).
//
// The LAST unowned handler in the seaside placement table (0x801469BC[62 records + terminator];
// this handler covers record 61 — cls=4 type=0x02 at world pos (16606, -4980, 11200) — a single
// deep-underwater actor in the seaside area). Every other placement handler in the seaside table
// is already native, so owning this closes the seaside placement-installed set 100%.
//
// STRUCTURAL SHAPE — proximity-gated 3-way substate router:
//   * node+4  (outer state): 0 = INIT / arming, 1 = RUNNING, 3 = DESPAWN.
//   * node+5  (inner INIT sub-state):
//       0 = stamp field-scoped constants (node+0xBF=0xE0, +0x7E=0x1B, +0x7C=0x400, +0x7A=0x0B,
//           +0x78=0x30) and arm mode 0 via FUN_8013C0BC(0), then advance sub-state and fall through
//           to the INIT draw call (FUN_8013B70C(node, DAT_8014ABE8)).
//       1 = keep issuing the INIT draw each frame.
//       2 = PROXIMITY ARM. Compute Chebyshev-ish distance from the camera position (scratch
//           0x1F800160/162/164) to the actor pos via FUN_80078240; if it's ≤ 1000 AND scratch
//           0x1F80027A == 0 AND master-G byte @0x800E7E80 == 6 AND 0x800BF80D == 0, transition to
//           RUNNING: node+4=1, node+5=0, node+0x5E=0. Otherwise this frame is idle.
//       any other = no-op (return).
//   * node+0x5E (RUNNING sub-state): 3-way switch → three sibling sub-behavior leaves in the A00
//     overlay:
//       0 → FUN_8013B868  (sub-behavior A)
//       1 → FUN_8013BAB0  (sub-behavior B)
//       2 → FUN_8013BCC8  (sub-behavior C — takes a data ptr DAT_8014AC08)
//       else → no side effects beyond clearing node+0x29.
//     Every RUNNING path clears node+0x29 (a per-frame flag) after its sub-behavior. Before the
//     sub-behavior dispatch, RUNNING runs Actor::boundsCull; if visible it fires FUN_8013C0BC(1)
//     (mode toggle) then FUN_80051844(node) (post-cull graphics/interaction update — sibling of
//     FUN_800518FC used by the beh_sop_intro_pilot chain).
//   * node+4 == 3 → standard pool return via Spawn::despawn (native).
//
// Ownership model (same shape as beh_scene_ui_trigger / beh_sop_intro_*): control flow + node
// writes owned native; every A00-overlay sub-behavior leaf (FUN_8013B70C, FUN_8013B868,
// FUN_8013BAB0, FUN_8013BCC8, FUN_8013C0BC, FUN_80051844, FUN_80078240) stays reachable via
// rec_dispatch. Actor::boundsCull and Spawn::despawn route to their native impls directly.
//
// Ghidra decomp: scratch/decomp/beh_8013c1dc.c.

#include "core.h"
#include "cfg.h"
#include "scene/engine.h"          // c->engine.spawn
#include "spawn.h"                 // c->engine.spawn.despawn (FUN_8007A624, native)
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN            = 0x8013C1DCu;

// -- guest-RAM addresses this handler reads for the proximity/arming gate ---------------------
constexpr uint32_t CAM_X_SPAD        = 0x1F800160u;   // camera pos scratchpad (s16)
constexpr uint32_t CAM_Y_SPAD        = 0x1F800162u;
constexpr uint32_t CAM_Z_SPAD        = 0x1F800164u;
constexpr uint32_t GATE_SPAD_27A     = 0x1F80027Au;   // per-frame gate byte
constexpr uint32_t MASTER_G_BYTE     = 0x800E7E80u;   // master-G first byte (mode selector)
constexpr uint32_t GATE_BF80D        = 0x800BF80Du;   // scene-transition guard byte

// A00-overlay data ptrs threaded into the sub-behaviors as fixed arguments.
constexpr uint32_t INIT_DRAW_DATA    = 0x8014ABE8u;   // arg to FUN_8013B70C (init draw)
constexpr uint32_t SUB_C_DATA        = 0x8014AC08u;   // arg to FUN_8013BCC8 (sub-behavior C)

constexpr int32_t  PROXIMITY_MAX     = 1000;          // FUN_80078240 result <= 1000 arms the trigger

// -- Substrate leaves (A00-overlay code kept dispatched — their own future sub-frontier) ------
inline void mode_arm       (Core* c, int32_t mode) { c->r[4] = (uint32_t)mode;                         rec_dispatch(c, 0x8013C0BCu); }
inline void draw_init      (Core* c, uint32_t o)   { c->r[4] = o; c->r[5] = INIT_DRAW_DATA;            rec_dispatch(c, 0x8013B70Cu); }
inline void sub_a          (Core* c, uint32_t o)   { c->r[4] = o;                                     rec_dispatch(c, 0x8013B868u); }
inline void sub_b          (Core* c, uint32_t o)   { c->r[4] = o;                                     rec_dispatch(c, 0x8013BAB0u); }
inline void sub_c          (Core* c, uint32_t o)   { c->r[4] = o; c->r[5] = SUB_C_DATA;                rec_dispatch(c, 0x8013BCC8u); }
inline void post_cull_upd  (Core* c, uint32_t o)   { c->r[4] = o;                                     rec_dispatch(c, 0x80051844u); }
inline int  bounds_cull    (Core* c, uint32_t o)   { c->r[4] = o; rec_dispatch(c, 0x8007778Cu); return (int)c->r[2]; }
inline int32_t cam_distance(Core* c, uint32_t o) {
  c->r[4] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_X_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x2E));
  c->r[5] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_Y_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x32));
  c->r[6] = (uint32_t)((int32_t)(int16_t)c->mem_r16(CAM_Z_SPAD) - (int32_t)(int16_t)c->mem_r16(o + 0x36));
  rec_dispatch(c, 0x80078240u);
  return (int32_t)c->r[2];
}

// -- State bodies -----------------------------------------------------------------------------
// INIT (outer 0) — internal sub-state at node+5. Only sub 0/1 issue the init-draw; sub 2 is the
// proximity ARM check that transitions the actor to RUNNING when in range.
void state_init(Core* c, uint32_t obj) {
  const uint8_t sub = c->mem_r8(obj + 5);
  if (sub == 2) {
    if (cam_distance(c, obj) > PROXIMITY_MAX)       return;
    if (c->mem_r8(GATE_SPAD_27A)     != 0)          return;
    if (c->mem_r8(MASTER_G_BYTE)     == 6)          return;   // recomp: `if (==6) return` — active outside mode 6
    if (c->mem_r8(GATE_BF80D)        != 0)          return;
    c->mem_w8(obj + 4,    1);   // → RUNNING
    c->mem_w8(obj + 5,    0);
    c->mem_w8(obj + 0x5E, 0);
    return;
  }
  if (sub == 0) {
    mode_arm(c, 0);
    c->mem_w8 (obj + 0xBF, 0xE0);
    c->mem_w16(obj + 0x7E, 0x1B);
    c->mem_w16(obj + 0x7C, 0x400);
    c->mem_w16(obj + 0x7A, 0x0B);
    c->mem_w16(obj + 0x78, 0x30);
    c->mem_w8 (obj + 5, (uint8_t)(sub + 1));        // advance sub 0 → 1
    // fall through into the shared INIT draw call below
  } else if (sub != 1) {
    return;                                          // out-of-range sub-state — no-op
  }
  draw_init(c, obj);                                 // shared sub-0 (after advance) + sub-1 tail
}

// RUNNING (outer 1) — cull → optional mode toggle + post-cull update → node[+0x5E] sub-dispatch,
// each sub-body clears node+0x29 on exit (matches the recomp's shared clear).
void state_running(Core* c, uint32_t obj) {
  if (bounds_cull(c, obj) != 0) {
    mode_arm(c, 1);
    post_cull_upd(c, obj);
  }
  const uint8_t sub = c->mem_r8(obj + 0x5E);
  if (sub == 0)      sub_a(c, obj);
  else if (sub == 1) sub_b(c, obj);
  else if (sub == 2) sub_c(c, obj);
  // else: no sub-behavior; still clears node+0x29 below
  c->mem_w8(obj + 0x29, 0);
}

}  // namespace

void beh_seaside_prox_substate(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t  st  = c->mem_r8(obj + 4);
  if (st == 1)      state_running(c, obj);
  else if (st == 0) state_init   (c, obj);
  else if (st == 3) c->engine.spawn.despawn(obj);
  // else: no-op (matches recomp's `bVar1 != 2 && bVar1 == 3` guard)
}
