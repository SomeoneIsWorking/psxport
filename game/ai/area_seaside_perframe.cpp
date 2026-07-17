// game/ai/area_seaside_perframe.cpp — seaside per-frame update fn FUN_80113C5C (A00 overlay).
//
// This is the PER-AREA PER-FRAME HANDLER `Engine::modePerFrameDispatch` fires for the seaside
// field (area 0). The 24-entry function-pointer table at 0x8009D1D4 (indexed by 0x800BF870) picks
// which of 24 area-specific per-frame handlers runs; area[0] = seaside = this function. It reads
// the master G MODE byte at G+0 (= 0x800E7E80, u8) to gate which sub-behaviors run, then walks
// the aux render list at *0x1F800154 (count *0x1F80015C) for per-item collision-style processing.
//
// ★ G IS TOMBA'S NODE (verified this pass — see docs/port-progress.md's "KEY FINDING"). So this
// function is the "seaside Tomba per-frame tick" — it runs Tomba's mode-specific pre-update
// (FUN_80022760(G) or FUN_8011334C(G) or FUN_801130C4(G) depending on G[0] mode byte), a fixed
// mid-update trio (FUN_80022554 / FUN_80113700 / FUN_801138E8), a variable aux-list walk
// (FUN_80112A60), and a fixed post-update pair (FUN_80112C0C / FUN_80112F14) + FUN_8010E904(G).
//
// STATE FLOW (mode = G[0]):
//   mode == 0 or 6            : ONLY the fixed trio + aux walk + post pair.
//   mode == 2                 : if *0x800E7FD8 < 2 also run FUN_8011334C(G) first, then falls
//                               through to the fixed trio + aux walk + post pair.
//   any other value           : run FUN_80022760(G) + FUN_801130C4(G), then falls through to the
//                               "case 2" branch (may also fire FUN_8011334C(G)) and onward.
//
// AUX-LIST WALK — pull items in reverse from the shared aux render list at *0x1F800154 (max
// count comes from *0x1F80015C, stored to DAT_1F800183 as loop counter). For each item ptr:
//   * skip if item[0] & 2 (already-processed guard)
//   * item[2] is the type: 0/1 → FUN_80112A60(item) (default interaction leaf),
//     4 → the same leaf UNLESS item[3] == 1 (guarded skip), 7 → same leaf, other → skip.
//
// SUB-BEHAVIOR LEAVES kept substrate (top-down: own the CONTROL FLOW here, descend into each
// callee's own tree only after their parent (this fn) is on the live path):
//     0x8002288C, 0x80022760, 0x80022554, 0x801130C4, 0x80113700, 0x801138E8, 0x8011334C,
//     0x80112A60, 0x80112C0C, 0x80112F14, 0x8010E904.
//
// Ghidra decomp scratch/decomp/seaside_perframe_113c5c.c.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "behaviors.h"
#include "core/engine.h"
void rec_dispatch(Core*, uint32_t);
#include "player/actor_tomba.h"                // eng(c).actorTomba.interactWalk (FUN_80022760)

namespace {

constexpr uint32_t G                       = 0x800E7E80u;   // master G block (= Tomba's node)
constexpr uint32_t AUX_LIST_HEAD_SPAD      = 0x1F800154u;   // ptr to aux-list u32 array
constexpr uint32_t AUX_LIST_COUNT_SPAD     = 0x1F80015Cu;   // u8 count of items in that array
constexpr uint32_t AUX_WALK_COUNTER_SPAD   = 0x1F800183u;   // per-frame counter DAT_1F800183

// Guest addresses of sub-behavior leaves this handler dispatches.
constexpr uint32_t LEAF_G_PRE_2288C        = 0x8002288Cu;   // pre-tick on G (unconditional)
constexpr uint32_t LEAF_G_TOMBA_TICK_22760 = 0x80022760u;   // Tomba tick on G (default mode)
// (LEAF_G_POST_130C4 = FUN_801130C4 → moved to ActorTomba::postInteractWalk)
constexpr uint32_t LEAF_G_MODE2_1334C      = 0x8011334Cu;   // mode-2-only tick on G (gated by 800E7FD8<2)
constexpr uint32_t LEAF_TRIO_22554         = 0x80022554u;   // fixed trio 1/3 (no-arg engine tick)
constexpr uint32_t LEAF_TRIO_113700        = 0x80113700u;   // fixed trio 2/3
constexpr uint32_t LEAF_TRIO_1138E8        = 0x801138E8u;   // fixed trio 3/3
constexpr uint32_t LEAF_AUX_ITEM_112A60    = 0x80112A60u;   // per-aux-item leaf (interaction)
constexpr uint32_t LEAF_POST_112C0C        = 0x80112C0Cu;   // fixed post 1/2
constexpr uint32_t LEAF_POST_112F14        = 0x80112F14u;   // fixed post 2/2
constexpr uint32_t LEAF_G_POSTFRAME_10E904 = 0x8010E904u;   // Tomba post-frame tick on G

// Dispatch a G-arg sub-leaf.
inline void gLeaf(Core* c, uint32_t leaf) { c->r[4] = G; rec_dispatch(c, leaf); }

// Walk the aux render list, dispatching FUN_80112A60(item) per item type-gate.
// Returns after the counter reaches 0 or an item's dispatch decides not to continue.
inline void aux_list_walk(Core* c) {
  const uint32_t listPtr = c->mem_r32(AUX_LIST_HEAD_SPAD);
  c->mem_w8(AUX_WALK_COUNTER_SPAD, c->mem_r8(AUX_LIST_COUNT_SPAD));
  uint32_t cursor = listPtr;
  while (c->mem_r8(AUX_WALK_COUNTER_SPAD) != 0) {
    const uint32_t item = c->mem_r32(cursor);
    c->mem_w8(AUX_WALK_COUNTER_SPAD, (uint8_t)(c->mem_r8(AUX_WALK_COUNTER_SPAD) - 1));
    cursor += 4;
    if (c->mem_r8(item) & 2) continue;                    // already-processed guard
    const uint8_t typ = c->mem_r8(item + 2);
    if (typ == 1) {                                        // guarded skip on subtype==1
      if (c->mem_r8(item + 3) != 1) continue;
      // fall through into leaf dispatch (matches the recomp's LAB_80113D8C join point)
    } else if (typ >= 2) {
      if (typ != 4 && typ != 7) continue;                  // only types 4/7 dispatch beyond
    } // typ == 0 → dispatch below
    c->r[4] = item;
    rec_dispatch(c, LEAF_AUX_ITEM_112A60);
  }
}

}  // namespace

void Behaviors::areaSeasidePerframe(Core* c) {
  gLeaf(c, LEAF_G_PRE_2288C);                              // always run the pre-tick on G

  const uint8_t mode = c->mem_r8(G);
  const bool defaultMode = !(mode == 0 || mode == 2 || mode == 6);
  bool runMode2Tick = (mode == 2);

  if (defaultMode) {
    eng(c).actorTomba.interactWalk();                   // native FUN_80022760 (Tomba interaction walk)
    eng(c).actorTomba.postInteractWalk();               // native FUN_801130C4 (default-mode post-tick)
    runMode2Tick = true;                                   // recomp falls through into `case 2`
  }
  if (runMode2Tick && c->mem_r16s(0x800E7FD8u) < 2) {
    gLeaf(c, LEAF_G_MODE2_1334C);                          // mode-2 sub-tick (gated)
  }

  // Fixed mid-update trio (fires in every mode 0/2/6 + default's fall-through).
  rec_dispatch(c, LEAF_TRIO_22554);                        // no-args engine tick
  rec_dispatch(c, LEAF_TRIO_113700);                       // no-args
  rec_dispatch(c, LEAF_TRIO_1138E8);                       // no-args

  aux_list_walk(c);

  // Fixed post-update pair + Tomba post-frame tick.
  rec_dispatch(c, LEAF_POST_112C0C);
  rec_dispatch(c, LEAF_POST_112F14);
  eng(c).actorTomba.postFrameWaterCheck();                        // native FUN_8010E904 (Tomba post-frame water/sea)
}
