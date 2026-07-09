// class ActorTomba — implementation. See actor_tomba.h for the class overview.
//
// This file consolidates every previously-owned Tomba primitive under one class:
//   * interactWalk + 3 collision helpers (was game/player/tomba_interact.cpp)
//   * velocityIntegrate  (was game/player/engine_player.cpp's static player_move_56b48)
//   * growthStep         (was Engine::playerGrowthStep in game/core/engine.cpp)
//
// Ghidra decomp references:
//   scratch/decomp/tomba_perframe_22760.c   (interactWalk = FUN_80022760)
//   scratch/decomp/tomba_interact_subs.c    (proximityCheck / subHitboxCheck)
//   scratch/decomp/fun_80114e74.c           (type4GuardedCheck)
//   scratch/decomp/batch_leaves.c           (growthStep = FUN_80057DC0)
//   disas.py 0x80056B48                    (velocityIntegrate)

#include "actor_tomba.h"
#include "core.h"
#include "cfg.h"
#include "core/engine.h"
#include "engine_overrides.h"
#include "game.h"
void rec_dispatch(Core*, uint32_t);

namespace {

// -- Shared constants (guest addresses) -------------------------------------------------------
constexpr uint32_t AUX_LIST_HEAD_SPAD   = 0x1F800154u;
constexpr uint32_t AUX_LIST_COUNT_SPAD  = 0x1F80015Cu;
constexpr uint32_t AUX_WALK_COUNTER     = 0x1F800182u;
constexpr uint32_t GATE_BF80C_hi        = 0x800BF80Du;

// Substrate leaves (kept dispatched).
constexpr uint32_t LEAF_ISQRT           = 0x80084080u;
constexpr uint32_t LEAF_ATAN2           = 0x80085690u;
constexpr uint32_t LEAF_COLL_CB         = 0x8004D19Cu;
constexpr uint32_t LEAF_PROX_F04        = 0x80022F04u;
// (FUN_80054650 moved to ActorTomba::settleStep — no longer needed as a substrate leaf.)

// Sub-hitbox parameter table (u8[16], 2 bytes per hitbox: (size_xz, size_y)) — MAIN.EXE .rodata.
constexpr uint32_t SUB_HITBOX_PARAMS    = 0x800A29D0u;

// Shared scratchpad outputs.
constexpr uint32_t OUT_DIST_SPAD        = 0x1F80008Cu;
constexpr uint32_t OUT_HEADING_SPAD     = 0x1F80009Cu;

// Growth-mode rescale target (a shared BSS halfword read by scenery/props).
constexpr uint32_t GROWTH_MIRROR_HW     = 0x800E802Au;

inline void mark_item_consumed(Core* c, uint32_t item) {
  c->mem_w8(item + 0, 2);
  c->mem_w8(item + 4, 2);
  c->mem_w8(item + 5, 0);
  c->mem_w8(item + 6, 0);
}

}  // namespace

// =================================================================================
// Per-frame interaction walk (FUN_80022760)
// =================================================================================
void ActorTomba::interactWalk() {
  Core* c = core;
  const uint32_t G = G_ADDR;

  // Early-outs.
  if (c->mem_r16(G + 0x16Eu) == 0)                       return;
  if (c->mem_r8 (GATE_BF80C_hi) != 0)                    return;
  if (c->mem_r16(G + 0x17Eu) & 0x200)                    return;

  const uint32_t listBase = c->mem_r32(AUX_LIST_HEAD_SPAD);
  const uint8_t  count0   = c->mem_r8 (AUX_LIST_COUNT_SPAD);
  c->mem_w8(GATE_BF80C_hi, 0);                            // recomp's initial `uVar2=0` write
  c->mem_w8(AUX_WALK_COUNTER, count0);

  uint32_t cursor = listBase;
  while (c->mem_r8(AUX_WALK_COUNTER) != 0) {
    const uint32_t item = c->mem_r32(cursor);
    c->mem_w8(AUX_WALK_COUNTER, (uint8_t)(c->mem_r8(AUX_WALK_COUNTER) - 1));
    cursor += 4;
    if (c->mem_r8(item) != 1) continue;                   // item[0]!=1 → skip

    const uint8_t typ = c->mem_r8(item + 2u);
    switch (typ) {
      case 0: case 1: case 2: case 3: case 7:
        proximityCheck(item);
        break;
      case 4:
        if (c->mem_r8(item + 0x5Eu) == 2) type4GuardedCheck(item);
        else                              proximityCheck   (item);
        break;
      case 6:
        subHitboxCheck(item);
        break;
      default:
        break;
    }
  }
}

// FUN_80022060 — cylinder proximity + Y-band check.
void ActorTomba::proximityCheck(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  if (c->mem_r8(0x1F80027Au) != 0) return;

  const int32_t dx = (int32_t)(int16_t)(c->mem_r16(G + 0x2Eu) - c->mem_r16(item + 0x2Eu));
  const int32_t dz = (int32_t)(int16_t)(c->mem_r16(G + 0x36u) - c->mem_r16(item + 0x36u));
  c->r[4] = (uint32_t)(dx * dx + dz * dz);
  rec_dispatch(c, LEAF_ISQRT);
  const int32_t dist = (int32_t)(int16_t)(uint16_t)c->r[2];

  const int32_t rxz = (int32_t)c->mem_r16s(G + 0x80u) + (int32_t)c->mem_r16s(item + 0x80u);
  if (dist > rxz) return;

  const int32_t vbandRaw = (int32_t)(int16_t)(uint16_t)(
      (c->mem_r16(G + 0x32u) - c->mem_r16(item + 0x32u))
      + c->mem_r16(G + 0x84u) + c->mem_r16(item + 0x84u));
  const int32_t vbandLim = (int32_t)c->mem_r16s(G + 0x86u) + (int32_t)c->mem_r16s(item + 0x86u);
  if (vbandRaw > vbandLim) return;

  c->mem_w32(OUT_DIST_SPAD, (uint32_t)dist);
  c->r[4] = (uint32_t)(-dz); c->r[5] = (uint32_t)dx;
  rec_dispatch(c, LEAF_ATAN2);
  c->mem_w32(OUT_HEADING_SPAD, c->r[2]);
  mark_item_consumed(c, item);
  c->mem_w8(0x800BF81Eu, 0);
}

// FUN_80114E74 — type-4 guarded proximity.
void ActorTomba::type4GuardedCheck(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  if (c->mem_r8(G + 0x164u) == 5 &&
      c->mem_r8(G + 0x147u) == c->mem_r8(item + 0x47u)) {
    return;
  }
  c->r[4] = G; c->r[5] = item;
  rec_dispatch(c, LEAF_PROX_F04);
  if (c->r[2] == 0) return;
  mark_item_consumed(c, item);
}

// FUN_80022190 — per-sub-hitbox collision variant.
void ActorTomba::subHitboxCheck(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  const int16_t hitboxCount = (int16_t)c->mem_r16(item + 0x6Au);
  if (hitboxCount <= 0) return;
  uint32_t hitboxArr = c->mem_r32(item + 0x6Cu);

  for (int32_t i = 0; i < hitboxCount; i++, hitboxArr += 0x10u) {
    const uint32_t mask = 1u << (i & 0x1F);
    if ((c->mem_r32(item + 0x70u) & mask) == 0) continue;

    const uint32_t typeParam = (uint32_t)c->mem_r8(hitboxArr + 3u) * 8u;
    const int32_t dx = (int32_t)(int16_t)(c->mem_r16(G + 0x2Eu) - c->mem_r16(hitboxArr + 4u));
    const int32_t dz = (int32_t)(int16_t)(c->mem_r16(G + 0x36u) - c->mem_r16(hitboxArr + 8u));
    c->r[4] = (uint32_t)(dx * dx + dz * dz);
    rec_dispatch(c, LEAF_ISQRT);
    const int32_t dist = (int32_t)(uint32_t)(c->r[2] & 0xFFFFu);

    const int32_t rxz = (int32_t)c->mem_r16s(G + 0x80u)
                       + (int32_t)c->mem_r8(SUB_HITBOX_PARAMS + typeParam + 0u);
    if (dist > rxz) continue;

    const uint32_t vbandRaw = (uint32_t)(
        (c->mem_r16(G + 0x32u) - c->mem_r16(hitboxArr + 6u))
        + c->mem_r16(G + 0x84u) + c->mem_r8(SUB_HITBOX_PARAMS + typeParam + 1u));
    const int32_t vbandLim = (int32_t)c->mem_r16s(G + 0x86u)
                            + (int32_t)c->mem_r8(SUB_HITBOX_PARAMS + typeParam + 1u) * 2;
    if ((int32_t)(uint16_t)vbandRaw > vbandLim) continue;

    c->mem_w32(item + 0x74u, c->mem_r32(item + 0x74u) | mask);
    c->mem_w32(item + 0x70u, c->mem_r32(item + 0x70u) & ~mask);
    c->r[4] = item; c->r[5] = hitboxArr; c->r[6] = 0;
    rec_dispatch(c, LEAF_COLL_CB);
    return;
  }
}

// =================================================================================
// Post-interact walk (FUN_801130C4) — the default-mode "post-tick" that runs after interactWalk
// =================================================================================
void ActorTomba::postInteractWalk() {
  Core* c = core;
  const uint32_t G = G_ADDR;

  // This walker uses a DIFFERENT aux list than interactWalk: the render/interaction queue at
  // *0x1F80013C with count *0x1F800144 (vs 0x1F800154 / 0x1F80015C for interactWalk).
  constexpr uint32_t LIST_HEAD_SPAD  = 0x1F80013Cu;
  constexpr uint32_t LIST_COUNT_SPAD = 0x1F800144u;

  // Sub-handler leaves (all substrate — the type-dispatch tree is its own future frontier).
  constexpr uint32_t LEAF_TYPE_9_SPECIAL   = 0x80111304u;   // item[0xC]==9 guarded handler
  constexpr uint32_t LEAF_TYPE_3           = 0x8010E258u;
  constexpr uint32_t LEAF_TYPE_4_PROX_STEP = 0x8001F40Cu;   // case-4 proximity + return code
  constexpr uint32_t LEAF_TYPE_4_TAG_SET   = 0x8001FDB4u;   // case-4 alt-tag write
  constexpr uint32_t LEAF_TYPE_7           = 0x800235A0u;
  constexpr uint32_t LEAF_TYPE_8           = 0x800205CCu;
  constexpr uint32_t LEAF_TYPE_0F_14_56    = 0x80020364u;
  constexpr uint32_t LEAF_TYPE_13          = 0x8010EA80u;

  uint32_t cursor = c->mem_r32(LIST_HEAD_SPAD);
  c->mem_w8(AUX_WALK_COUNTER, c->mem_r8(LIST_COUNT_SPAD));

  while (c->mem_r8(AUX_WALK_COUNTER) != 0) {
    const uint32_t item = c->mem_r32(cursor);
    c->mem_w8(AUX_WALK_COUNTER, (uint8_t)(c->mem_r8(AUX_WALK_COUNTER) - 1));
    cursor += 4;
    if ((c->mem_r8(item) & 1) == 0) continue;                    // active-flag gate
    if (c->mem_r8(item + 0xCu) == 9) {                           // special-type items
      if ((c->mem_r16(G + 0x17Eu) & 0x8200u) == 0) {
        c->r[4] = G; c->r[5] = item; rec_dispatch(c, LEAF_TYPE_9_SPECIAL);
      }
      continue;                                                   // keep walking
    }
    const uint8_t typ = c->mem_r8(item + 2u);
    switch (typ) {
      case 3:
        c->r[4] = G; c->r[5] = item; rec_dispatch(c, LEAF_TYPE_3);
        break;
      case 4: {
        // Detailed guarded state-transition (case 4). Faithful to the recomp:
        //   * dispatch the proximity/step leaf with a2=1; if v0 < 0 → skip (no interaction).
        //   * stop the walk (WALK_COUNTER = 0) unconditionally after we enter case 4.
        //   * bonus-tag path: DAT_800BF9E5 == 6 && G+0x144 == 1 && v0 < 2 →
        //         FUN_8001FDB4(item, 0xFFFF8001, 0x10, 0x20); continue walking.
        //   * silence path: skip if 0x1F800137 != 0 OR G[0] & 6 OR G+0x144 > 1 OR G+0x164 != 0.
        //   * else: DAT_800BF9E5 != 6 → announcer cue 0x2A/0x41; stamp G/item state as the
        //     "type-4 hit" transition (see writes below).
        c->r[4] = G; c->r[5] = item; c->r[6] = 1;
        rec_dispatch(c, LEAF_TYPE_4_PROX_STEP);
        const int32_t v0 = (int32_t)c->r[2];
        if (v0 < 0) break;                                        // no hit
        c->mem_w8(AUX_WALK_COUNTER, 0);                           // stop the walk
        const uint8_t bf9e5 = c->mem_r8(0x800BF9E5u);
        const uint8_t g144  = c->mem_r8(G + 0x144u);
        if (bf9e5 == 6 && g144 == 1 && v0 < 2) {
          c->r[4] = item; c->r[5] = 0xFFFF8001u; c->r[6] = 0x10u; c->r[7] = 0x20u;
          rec_dispatch(c, LEAF_TYPE_4_TAG_SET);
          break;                                                  // continue at loop top (via while)
        }
        if (c->mem_r8(0x1F800137u) != 0)                          break;
        if ((c->mem_r8(G) & 6) != 0)                              break;
        if (g144 > 1)                                             break;
        if (c->mem_r8(G + 0x164u) != 0)                           break;
        if (bf9e5 != 6) {
          c->engine.announcerCue(0x2A, 0x41);                     // native FUN_8004ED94
        }
        // Type-4 hit state transition on G + item.
        c->mem_w8(G + 4, 2);
        c->mem_w8(G + 5, 2);
        c->mem_w8(G + 0, 3);
        c->mem_w8(G + 6, 0);
        c->mem_w8(G + 0x172u, 0x78);
        c->mem_w8(G + 0x173u, 0);
        c->mem_w8(G + 0x2Bu, (uint8_t)((int32_t)c->mem_r32(OUT_HEADING_SPAD) >> 4));
        break;
      }
      case 7:
        c->r[4] = G; c->r[5] = item; rec_dispatch(c, LEAF_TYPE_7);
        break;
      case 8:
        c->r[4] = G; c->r[5] = item; rec_dispatch(c, LEAF_TYPE_8);
        break;
      case 0x0F: case 0x14: case 0x56:
        c->r[4] = G; c->r[5] = item; c->r[6] = 0;
        rec_dispatch(c, LEAF_TYPE_0F_14_56);
        break;
      case 0x13:
        c->r[4] = G; c->r[5] = item; rec_dispatch(c, LEAF_TYPE_13);
        break;
      case 0x2F:
        c->r[4] = G; c->r[5] = item; c->r[6] = 2;
        rec_dispatch(c, LEAF_TYPE_0F_14_56);
        break;
      default:
        break;                                                    // no interaction for other types
    }
  }
}

// =================================================================================
// Growth / shrink transformation (FUN_80057DC0)
// =================================================================================
void ActorTomba::growthStep(int32_t mode) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  const uint16_t f17E = c->mem_r16(G + 0x17Eu);
  const int16_t  posY = (int16_t)c->mem_r16(G + 0x32u);
  uint16_t newFlag;
  if (mode == 0) {
    if (f17E & 0x8000) c->mem_w16(G + 0x32u, (uint16_t)(posY - 0x46));   // shrink → drop feet
    newFlag = (uint16_t)(f17E & 0x7FFF);
  } else {
    if ((f17E & 0x8000) == 0) c->mem_w16(G + 0x32u, (uint16_t)(posY + 0x46));  // grow → raise feet
    newFlag = (uint16_t)(f17E | 0x8000);
  }
  c->mem_w16(G + 0x17Eu, newFlag);

  const int32_t divisor = mode + 1;
  const int16_t s1000 = (int16_t)(0x1000 / divisor);
  const int16_t s32_  = (int16_t)(0x32   / divisor);
  const int16_t s100  = (int16_t)(100    / divisor);
  const int16_t s8C   = (int16_t)(0x8C   / divisor);
  const int16_t s10E  = (int16_t)(0x10E  / divisor);
  const int16_t s1E   = (int16_t)(0x1E   / divisor);
  const int16_t sF0   = (int16_t)(0xF0   / divisor);
  c->mem_w16(G + 0xB8u, (uint16_t)s1000);
  c->mem_w16(G + 0xBAu, (uint16_t)s1000);
  c->mem_w16(G + 0xBCu, (uint16_t)s1000);
  c->mem_w16(G + 0x80u, (uint16_t)s32_);
  c->mem_w16(G + 0x82u, (uint16_t)s100);
  c->mem_w16(G + 0x84u, (uint16_t)s8C);
  c->mem_w16(G + 0x86u, (uint16_t)s10E);
  c->mem_w16(G + 0x62u, (uint16_t)s8C);
  c->mem_w16(G + 0x64u, (uint16_t)s8C);
  c->mem_w16(G + 0x66u, (uint16_t)s100);
  c->mem_w16(G + 0x68u, (uint16_t)s1E);
  c->mem_w16(GROWTH_MIRROR_HW, (uint16_t)sF0);
}

// =================================================================================
// Post-frame water/sea check (FUN_8010E904 — final call in area_seaside_perframe)
// =================================================================================
void ActorTomba::postFrameWaterCheck() {
  Core* c = core;
  const uint32_t G = G_ADDR;

  constexpr uint32_t WATER_MODE_BYTE   = 0x800BF816u;
  constexpr uint32_t WATER_LEVEL_S16   = 0x800BF812u;   // water surface Y (s16)
  constexpr uint32_t WATER_STATE_BYTE  = 0x800BF817u;
  constexpr uint32_t PAUSE_FLAG_SPAD   = 0x1F800137u;
  constexpr uint32_t LEAF_DRY_TICK     = 0x8010E408u;   // per-frame Tomba tick when not in water
  constexpr uint32_t LEAF_WATER_SPLASH = 0x80022C78u;   // Y-snap tail (particle spawn?)

  const int16_t waterLevel = (int16_t)c->mem_r16(WATER_LEVEL_S16);
  const uint8_t waterMode  = c->mem_r8(WATER_MODE_BYTE);
  const uint8_t waterState = c->mem_r8(WATER_STATE_BYTE);

  if (waterMode == 0) {
    // Dry land: run the per-frame Tomba tick if not paused.
    if (c->mem_r8(PAUSE_FLAG_SPAD) == 0) {
      c->r[4] = G; rec_dispatch(c, LEAF_DRY_TICK);
    }
  } else {
    // Water/sea mode. When water-state is 2 with a specific 800E7FEB (== 8) config, clamp
    // Tomba's Z to the water-region edge — matches the recomp's `< 0x1a05 → 0x1a04` snap.
    bool skipYSnap = false;
    if (waterState > 1) {
      if (waterState == 2 && c->mem_r8(0x800E7FEBu) == 8) {
        if ((int16_t)c->mem_r16(G + 0x36u) < 0x1A05) c->mem_w16(G + 0x36u, 0x1A04);
      } else {
        skipYSnap = true;                                 // recomp: `goto LAB_8010E9D4;` skips the Y block
      }
    }
    if (!skipYSnap) {
      if ((c->mem_r8(G + 0x145u) & 1) == 0 &&
          (int32_t)waterLevel - (int32_t)c->mem_r16s(G + 0x62u) <= (int32_t)c->mem_r16s(G + 0x32u)) {
        c->mem_w16(G + 0x32u, (uint16_t)(waterLevel - c->mem_r16(G + 0x62u)));   // Y = waterLevel - G+0x62
        c->r[4] = G; rec_dispatch(c, LEAF_WATER_SPLASH);
      }
    }
  }

  // Area-exit trigger — fires only in water-mode 2 when Tomba is off-map.
  if (c->mem_r8(G + 0x17Bu) != 0) return;
  if (c->mem_r8(0x800BF80Du) != 0) return;
  if (c->mem_r8(0x800BF839u) != 0) return;
  if (waterMode == 0) return;
  if (waterState != 2) return;
  if ((int16_t)c->mem_r16(G + 0x32u) >= -0xE74) return;
  if ((int16_t)c->mem_r16(G + 0x36u) >= 0x1451) return;

  c->mem_w8 (PAUSE_FLAG_SPAD,   waterState);
  c->mem_w8 (0x800BF80Fu,       waterState);
  c->mem_w16(0x800BF83Au,       0x100);
  c->mem_w8 (0x800BF839u,       1);
  c->mem_w8 (0x1F800236u,       1);
}

// =================================================================================
// postInteractWalk sub-handlers — band 0x80020000-0x8002FFFF. RE'd + drafted 2026-07-08 from
// Ghidra headless (scratch/decomp/region_8002.c) cross-checked against generated/shard_*.c
// (ground truth for the guest-stack frame + jal-site `ra` constants). UNWIRED: postInteractWalk
// above still reaches these via rec_dispatch(c, LEAF_TYPE_*) — wiring these methods in requires
// adding EngineOverrides/shard_set_override entries, deliberately left for the next frontier
// pass so this draft compiles as dead code only.
// =================================================================================
namespace {
// Leaves called by these 4 handlers that stay substrate (out of the 0x8002xxxx band; not
// RE'd by this pass). Named per the file's existing LEAF_* convention.
constexpr uint32_t LEAF_PROX_STEP        = 0x8001F40Cu;   // FUN_8001F40C — shared proximity+step (== postInteractWalk's LEAF_TYPE_4_PROX_STEP)
constexpr uint32_t LEAF_ALT_TAG_SET      = 0x8001FDB4u;   // FUN_8001FDB4 — alt-tag stamp (== postInteractWalk's LEAF_TYPE_4_TAG_SET)
constexpr uint32_t LEAF_GROWN_PUSH       = 0x8001F054u;   // FUN_8001F054 — grown-state push (stepModeInteract's 0x8000-set/mode&3 branch)
constexpr uint32_t LEAF_NILADIC_CUE      = 0x8001F830u;   // FUN_8001F830 — niladic substrate cue (type8Interact's item[0]==5 branch)
constexpr uint32_t LEAF_GROWN_DELEGATE   = 0x8001EC3Cu;   // FUN_8001EC3C — whole-hog grown-state delegate (type8Interact's 0x8000-set branch)
constexpr uint32_t LEAF_STEP_MODE_FLAG   = 0x8001FF7Cu;   // FUN_8001FF7C — type7Interact's mode/flag call
}  // namespace

// FUN_80020364 — postInteractWalk case 0xF/0x14/0x56 (mode=0) / 0x2F (mode=2).
uint8_t ActorTomba::stepModeInteract(uint32_t item, uint32_t mode) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  // Guest frame: addiu sp,-40; spill s0,s1,s2,s3,ra (mirrored for completeness though this
  // draft has no re-entrant native call that would observe the guest stack bytes yet).
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 40;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 32, c->r[31]);
  c->mem_w32(c->r[29] + 28, c->r[19]);
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[17] = G; c->r[18] = item; c->r[19] = mode;

  uint8_t result;
  if (c->mem_r16(G + 0x17Eu) & 0x200u) {
    result = 0;                                              // paused — no interaction
  } else {
    c->r[4] = G; c->r[5] = item; c->r[6] = 1;
    c->r[31] = 0x800203A8u;
    rec_dispatch(c, LEAF_PROX_STEP);
    const int32_t v0 = (int32_t)c->r[2];
    if (v0 < 0) {
      result = 0;                                            // no hit
    } else if (c->mem_r8(G + 0x144u) == 1 && v0 < 2) {
      // Just-transitioned state.
      if ((c->mem_r16(G + 0x17Eu) & 0x8000u) == 0) {
        c->r[4] = item; c->r[5] = 1; c->r[6] = 0x10; c->r[7] = 0x20;
        c->r[31] = 0x80020418u;
        rec_dispatch(c, LEAF_ALT_TAG_SET);
        result = 1;
      } else if (mode & 3u) {
        c->r[4] = G; c->r[5] = item;
        c->r[31] = 0x800203FCu;
        rec_dispatch(c, LEAF_GROWN_PUSH);
        result = 1;
      } else {
        result = 1;
      }
    } else {
      // Steady-state: optional trig-offset separation, then a mode-bit-keyed result/state stamp.
      // Heading is the FULL 32-bit word proximityCheck stamped into OUT_HEADING_SPAD (a raw
      // Trig::ratan2 result register width, not a 16-bit angle) — read as mem_r32 throughout.
      if (mode & 0x3Fu) {
        const int32_t heading = (int32_t)c->mem_r32(0x1F80009Cu);   // OUT_HEADING_SPAD
        const int32_t cosv = c->trig.rcos(heading);
        const int32_t sinv = c->trig.rsin(heading);
        const int32_t sum80 = (int32_t)c->mem_r16s(G + 0x80u) + (int32_t)c->mem_r16s(item + 0x80u);
        const int16_t dx = (int16_t)((cosv * sum80) >> 12);
        const int16_t dz = (int16_t)((sinv * sum80) >> 12);
        if ((mode & 0x7Fu) == 1) {
          c->mem_w16(item + 0x2Eu, (uint16_t)((int16_t)c->mem_r16(G + 0x2Eu) - dx));
          c->mem_w16(item + 0x36u, (uint16_t)((int16_t)c->mem_r16(G + 0x36u) + dz));
        } else if ((c->mem_r8(G) & 4u) == 0) {
          c->mem_w16(G + 0x2Eu, (uint16_t)((int16_t)c->mem_r16(item + 0x2Eu) + dx));
          c->mem_w16(G + 0x36u, (uint16_t)((int16_t)c->mem_r16(item + 0x36u) - dz));
        }
      }
      // bVar6 (gen: `(byte)(_DAT_1f80009c >> 4)`) — truncate the 32-bit heading word, not a byte load.
      const uint8_t bVar6 = (uint8_t)((uint32_t)c->mem_r32(0x1F80009Cu) >> 4);
      if ((mode & 0x40u) == 0) {
        // mode&0x80 ladder — gate 0x1F80027A (proximityCheck's own "already consumed" guard).
        if (mode & 0x80u) {
          if (c->mem_r8(0x1F80027Au) != 0) { result = 2; goto done; }
          if (c->mem_r8(G + 4u) != 1)      { result = 2; goto done; }
          if (c->mem_r8(G + 5u) != 0x13) {
            c->mem_w8(G + 5u, 0x13);
            c->mem_w8(G + 6u, 0);
            c->mem_w8(G + 7u, 0);
            c->mem_w8(G + 0x2Bu, bVar6);
            result = 3; goto done;
          }
        }
        result = 2;
      } else {
        // mode&0x40 ladder — cascading result value: each gate's masked read IS the return code
        // on early-exit (gen's `bVar3 = X; if (X==0) {...}` idiom — no early exit re-derives a
        // fresh value, the failing mask itself is the result).
        uint8_t bVar3 = c->mem_r8(0x1F800137u);                       // PAUSE_FLAG_SPAD
        if (bVar3 == 0) {
          bVar3 = c->mem_r8(G) & 6u;
          if (bVar3 == 0) {
            bVar3 = c->mem_r8(item) & 2u;
            if (bVar3 == 0) {
              bVar3 = 4;
              c->mem_w8(G + 4u, 2);
              c->mem_w8(G + 5u, 2);
              c->mem_w8(G, 3);
              c->mem_w8(G + 6u, 0);
              c->mem_w16(G + 0x172u, 0x78u);   // single u16 store covers both G+0x172(=0x78)/G+0x173(=0)
              c->mem_w8(G + 0x2Bu, bVar6);
            }
          }
        }
        result = bVar3;
      }
    }
  }
done:
  c->r[31] = c->mem_r32(c->r[29] + 32);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
  return result;
}

// FUN_800205CC — postInteractWalk case 8.
void ActorTomba::type8Interact(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 28, c->r[31]);
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[17] = G; c->r[18] = item;

  if (c->mem_r8(item) == 5) {
    if ((c->mem_r16(G + 0x17Eu) & 0x200u) == 0 && c->mem_r8(G + 0x78u) == 0) {
      c->r[31] = 0x80020620u;
      rec_dispatch(c, LEAF_NILADIC_CUE);
    }
  } else if (c->mem_r16(G + 0x17Eu) & 0x8000u) {
    c->r[4] = G; c->r[5] = item;
    c->r[31] = 0x80020644u;
    rec_dispatch(c, LEAF_GROWN_DELEGATE);
  } else {
    c->r[4] = G; c->r[5] = item; c->r[6] = 0;
    c->r[31] = 0x80020658u;
    rec_dispatch(c, LEAF_PROX_STEP);
    const int32_t v0 = (int32_t)c->r[2];
    if (v0 >= 0) {
      if (c->mem_r8(item) == 1) {
        if (c->mem_r8(G + 0x144u) == 1 && v0 < 2) {
          c->r[4] = item; c->r[5] = (uint32_t)-32766; c->r[6] = 3; c->r[7] = 30;
          c->r[31] = 0x8002069Cu;
          rec_dispatch(c, LEAF_ALT_TAG_SET);
        } else if ((c->mem_r16(G + 0x17Eu) & 0x200u) == 0) {
          if ((v0 & 1) == 0) {
            if ((c->mem_r8(G) & 4u) == 0) {
              const int32_t heading = (int32_t)c->mem_r32(0x1F80009Cu);   // full 32-bit word
              const int32_t cosv = c->trig.rcos(heading);
              const int32_t sinv = c->trig.rsin(heading);
              const int32_t sum80 = (int32_t)c->mem_r16s(G + 0x80u) + (int32_t)c->mem_r16s(item + 0x80u);
              c->mem_w16(G + 0x2Eu, (uint16_t)((int16_t)c->mem_r16(item + 0x2Eu) + (int16_t)((cosv * sum80) >> 12)));
              c->mem_w16(G + 0x36u, (uint16_t)((int16_t)c->mem_r16(item + 0x36u) - (int16_t)((sinv * sum80) >> 12)));
            }
            c->mem_w8(G + 0x60u, 1);
            // Heading arg is the full 32-bit OUT_HEADING_SPAD word (Ghidra: `iVar7 = (int)_DAT_1f80009c`
            // — a straight int cast, no 16-bit truncation, matching stepModeInteract's bVar6 fix).
            const int32_t cmp = Trig::angleCmp((int32_t)c->mem_r32(0x1F80009Cu),
                                                (int32_t)(int16_t)c->mem_r16(G + 0x140u), 1);
            c->mem_w8(G + 0x5Fu, (uint8_t)(cmp + 2));
          } else if (v0 == 1 && (c->mem_r8(G + 0x145u) & 1u) == 0) {
            // G+0x32 = item[0x32] - (G[0x84] + item[0x84]) (all u16, unsigned per gen), THEN
            // growthYSnap()'s own reset+gated-Y-resnap tail — this branch's G+0x29/0x145/0x4A/
            // 0x50/0x148 reset (v0==1 here) plus the G+0x78/DAT_800BF816-gated const-140/70 snap
            // on G+0x32 are BYTE-IDENTICAL to guest FUN_80022C78 (growthYSnap), reused rather than
            // duplicated (generated/shard_0.c:1112 lines 1-19 == generated/shard_0.c:1466 lines 1-16).
            const uint32_t gOff84   = c->mem_r16(G + 0x84u);
            const uint32_t itOff84  = c->mem_r16(item + 0x84u);
            const uint32_t itemY    = c->mem_r16(item + 0x32u);
            c->mem_w16(G + 0x32u, (uint16_t)(itemY - (gOff84 + itOff84)));
            growthYSnap();
          }
        }
      } else if ((c->mem_r16(G + 0x17Eu) & 0x200u) == 0 && (c->mem_r8(G + 0x145u) & 1u) == 0) {
        c->mem_w8(item + 0x29u, 1);
      }
    }
  }

  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

// FUN_800235A0 — postInteractWalk case 7.
uint8_t ActorTomba::type7Interact(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->r[16] = G; c->r[17] = item;

  c->r[4] = G; c->r[5] = item; c->r[6] = 1;
  c->r[31] = 0x800235C0u;
  rec_dispatch(c, LEAF_PROX_STEP);
  const int32_t v0 = (int32_t)c->r[2];
  uint8_t result = 0;
  if (v0 >= 0) {
    // BUG FIX (RE cross-check against generated/shard_4.c:1267 gen_func_800235A0): the ground
    // truth does NOT reload r6 (a2) before this second call — a2 is a caller-saved scratch
    // register left as whatever LEAF_PROX_STEP's OWN body last wrote to it (a2 is never one of
    // that leaf's declared inputs/outputs, so its post-call value is an incidental side effect,
    // but it's a REAL, deterministic value since LEAF_PROX_STEP is the same substrate body on
    // both SBS cores). The original draft clobbered it with `item`, which is byte-exact ONLY if
    // it happens to equal what LEAF_PROX_STEP leaves behind — a latent SBS divergence. Leave r6
    // untouched here so it carries through exactly like the recompiled body.
    const uint32_t flag = (c->mem_r8(G + 0x164u) == 0x0Cu) ? 4u : 1u;
    c->r[4] = G; c->r[5] = item; c->r[7] = flag;
    c->r[31] = 0x80023600u;
    rec_dispatch(c, LEAF_STEP_MODE_FLAG);
    result = 1;
  }

  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
  return result;
}

// FUN_80022C78 — leaf, no guest-stack frame. Operates on G (postFrameWaterCheck's
// LEAF_WATER_SPLASH call site).
void ActorTomba::growthYSnap() {
  Core* c = core;
  const uint32_t G = G_ADDR;

  c->mem_w8 (G + 0x29u, 1);
  c->mem_w8 (G + 0x145u, 0);
  c->mem_w16(G + 0x4Au, 0);
  c->mem_w16(G + 0x50u, 0);
  c->mem_w8 (G + 0x148u, 0);

  if (c->mem_r8(G + 0x78u) != 0) return;
  if (c->mem_r8(0x800BF816u) != 0) return;

  // BUG FIX (RE cross-check against generated/shard_0.c:1466 gen_func_80022C78): the ground
  // truth's `if (g17E<0) goto L_80022CD8` branch jumps to a block that explicitly re-sets r3=70
  // (0x46) for the comparison/subtraction constant; the FALLTHROUGH (g17E>=0) keeps r3=140
  // (0x8C) from the branch's own delay-slot preset. So g17E<0 -> 0x46, g17E>=0 -> 0x8C — the
  // original draft had this backwards. Same polarity bug was inlined at type8Interact's "just
  // left growth" tail (which reuses this constant indirectly via growthYSnap()), so this one fix
  // corrects both call sites.
  const int16_t g17E = (int16_t)c->mem_r16(G + 0x17Eu);
  const int16_t k    = (g17E < 0) ? 0x46 : 0x8C;
  const int16_t g84  = (int16_t)c->mem_r16(G + 0x84u);
  if (g84 == k) return;                                        // no-op — already at the snap point
  c->mem_w16(G + 0x32u, (uint16_t)(g84 + ((int16_t)c->mem_r16(G + 0x32u) - k)));
}

// =================================================================================
// Settle helper — velocityIntegrate's tail dispatch (FUN_80054650)
// =================================================================================
uint32_t ActorTomba::settleStep(int32_t mode) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  c->mem_w8(0x1F800258u, 0);                                          // clear sink-mark
  c->mem_w8(G + 0x5Fu, (uint8_t)(c->mem_r8(G + 0x5Fu) & 0xFB));       // flag95 &= ~0x04

  if (c->mem_r8(G + 0x16Bu) != 0) { c->r[2] = 0; return 0; }          // flag363 gate

  // Probe offset selector: mode==0 default 0x1E / 0x3C (when G+0x17E has high bit clear),
  // mode!=0 always 0.
  uint32_t probeOffset = 0;
  if (mode == 0) {
    probeOffset = ((int16_t)c->mem_r16(G + 0x17Eu) >= 0) ? 0x3Cu : 0x1Eu;
  }

  // Probe base: G+0x62 (u16) unless G+0x78 (state) != 0, in which case pull the "hooked item" at
  // G+0x10 and compute `(item[+0x86] - item[+0x84]) - (G+0x32 - item[+0x32])`.
  int32_t base;
  if (c->mem_r8(G + 0x78u) == 0) {
    base = (int32_t)c->mem_r16s(G + 0x62u);
  } else {
    const uint32_t item = c->mem_r32(G + 0x10u);
    base = ((int32_t)c->mem_r16s(item + 0x86u) - (int32_t)c->mem_r16s(item + 0x84u))
         - ((int32_t)c->mem_r16s(G + 0x32u) - (int32_t)c->mem_r16s(item + 0x32u));
  }
  const int16_t half = (int16_t)(base / 2);

  auto probe = [&](int16_t offset) -> int32_t {
    c->r[4] = G; c->r[5] = probeOffset; c->r[6] = (uint32_t)(int32_t)offset;
    rec_dispatch(c, 0x8004954Cu);                                     // grid probe (substrate)
    return (int32_t)c->r[2];
  };

  if (probe(half) != 0 || probe((int16_t)(-half)) != 0) {
    uint8_t bV = (uint8_t)(c->mem_r8(G + 0x149u) & 1);
    if ((c->mem_r8(G + 0x149u) & 4) == 0) bV = c->mem_r8(G + 0x147u);
    c->mem_w8(G + 0x60u, 1);
    c->mem_w8(G + 0x5Fu, (uint8_t)(bV + 4));
    c->r[2] = 1;
    return 1;
  }

  // No probe hit — check the sink-mark and fall through with 0.
  if (c->mem_r8(0x1F800258u) != 0) {
    const int8_t v = (int8_t)(5 - (int8_t)c->mem_r8(G + 0x147u));
    c->mem_w8(G + 0x5Fu, (uint8_t)v);
  }
  c->r[2] = 0;
  return 0;
}

// =================================================================================
// Movement — velocity integrate (FUN_80056B48)
// =================================================================================
void ActorTomba::velocityIntegrate(bool suppressY) {
  Core* c = core;
  const uint32_t G = G_ADDR;

  const int32_t speed = c->mem_r16s(G + 0x44u);
  const int32_t dirX  = c->mem_r16s(G + 0x48u);
  const int32_t dirZ  = c->mem_r16s(G + 0x4Cu);
  c->mem_w32(G + 0x2Cu, c->mem_r32(G + 0x2Cu) + (uint32_t)(dirX * speed));   // posX
  c->mem_w32(G + 0x34u, c->mem_r32(G + 0x34u) + (uint32_t)(dirZ * speed));   // posZ

  if (!suppressY) {
    const int32_t dirY = c->mem_r16s(G + 0x4Au);
    c->mem_w32(G + 0x30u, c->mem_r32(G + 0x30u) + (uint32_t)(dirY * speed));  // posY
  }

  // Tail: settle-helper dispatch OR flag95 &= ~0x04.
  if (c->mem_r8(G + 0x16Bu) == 0 && c->mem_r8(G + 0x61u) == 0) {
    settleStep(0);                                                   // native FUN_80054650
  } else {
    const uint8_t f = (uint8_t)(c->mem_r8(G + 0x5Fu) & 0xFB);
    c->mem_w8(G + 0x5Fu, f);
    c->r[2] = f;
  }
}

// =================================================================================
// registerOverrides — wire the 4 postInteractWalk sub-handlers into EngineOverrides. Guest ABI:
// a0=G (implicit/unused — G is always ActorTomba::G_ADDR), a1=item, a2=mode where applicable;
// return via r[2] for the two that produce a v0. See actor_tomba.h for why EngineOverrides alone
// (no shard_set_override) is correct here — no substrate shard calls these 4 addresses directly.
// =================================================================================
void ActorTomba::ov_stepModeInteract(Core* c) {
  const uint32_t item = c->r[5];
  const uint32_t mode = c->r[6];
  c->r[2] = c->engine.actorTomba.stepModeInteract(item, mode);
}
void ActorTomba::ov_type8Interact(Core* c) {
  const uint32_t item = c->r[5];
  c->engine.actorTomba.type8Interact(item);
}
void ActorTomba::ov_type7Interact(Core* c) {
  const uint32_t item = c->r[5];
  c->r[2] = c->engine.actorTomba.type7Interact(item);
}
void ActorTomba::ov_growthYSnap(Core* c) {
  c->engine.actorTomba.growthYSnap();
}

void ActorTomba::ov_frameTick(Core* c) {
  c->engine.actorTomba.frameTick();
}

void ActorTomba::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x80020364u, "ActorTomba::stepModeInteract", ov_stepModeInteract);
  ov.register_(0x800205CCu, "ActorTomba::type8Interact",    ov_type8Interact);
  ov.register_(0x800235A0u, "ActorTomba::type7Interact",    ov_type7Interact);
  ov.register_(0x80022C78u, "ActorTomba::growthYSnap",      ov_growthYSnap);
  ov.register_(0x8005950Cu, "ActorTomba::frameTick",        ov_frameTick);
}

// =================================================================================
// Per-frame G-block driver cascade (0x8005950C region) — RE'd + drafted 2026-07-08 wide-RE pass.
// UNWIRED: Engine::frameStartTick's `default: target = 0x8005950Cu` dispatch site still reaches
// the substrate func_8005950C directly (game/core/engine.cpp); wiring these in is a future
// frontier-tier step (EngineOverrides + shard_set_override, then SBS-gate). All 6 functions below
// are faithful 1:1 ports from generated/shard_*.c ground truth (cited per-function), cross-checked
// against the Ghidra headless decompile in scratch/decomp/g_8005950C.c — which caught ONE real
// Ghidra decompiler error (see outerTransitionCommit's banner).
// =================================================================================

// turnBiasCompute — guest FUN_80055C9C. See actor_tomba.h for the full RE writeup.
void ActorTomba::turnBiasCompute(Core* c, int16_t facing) {
  bool closeIn;
  if (c->mem_r8(0x800E806Cu) == 5) {
    // Wide/menu variant: delta from a fixed 0xC00 reference instead of DAT_1F8000F2 directly.
    const uint32_t d = (3072u - c->mem_r16(0x1F8000F2u) - (uint32_t)(int32_t)facing) & 4095u;
    closeIn = (int32_t)d < 2048;
  } else {
    uint32_t r3 = ((uint32_t)(uint8_t)c->mem_r8(0x800E806Cu) - (uint32_t)(int32_t)facing) & 4095u;
    const uint32_t r2m = c->mem_r16(0x1F8000F2u) & 4095u;
    r3 = r3 - r2m;
    const uint32_t r4 = ((int16_t)r3 < 0) ? r3 : (r3 - 512u);
    const uint32_t d = r4 & 4095u;
    if (c->mem_r16(0x800E805Au) & 0x800u) closeIn = (int32_t)d < 1536;
    else                                  closeIn = (int32_t)d < 2560;
  }
  if (closeIn) { c->mem_w16(0x1F80016Cu, 128); c->mem_w16(0x1F80016Eu, 32); }
  else         { c->mem_w16(0x1F80016Cu, 32);  c->mem_w16(0x1F80016Eu, 128); }
}

// resetLoadGate — guest FUN_80042310. See actor_tomba.h for the full RE writeup.
void ActorTomba::resetLoadGate(Core* c) {
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24;
  c->mem_w32(c->r[29] + 16, c->r[31]);

  rec_dispatch(c, 0x8001CF78u);                    // niladic substrate cue
  c->r[4] = 0x7Fu; c->r[5] = 0; c->r[6] = 0;
  rec_dispatch(c, 0x80074590u);                     // FUN_80074590(0x7F, 0, 0)
  const uint8_t mode = c->mem_r8(0x800BF870u);      // area/mode byte, read before the unpause write
  c->mem_w8(0x1F800137u, 0);                        // unpause
  c->r[4] = mode;
  rec_dispatch(c, 0x80074F24u);                     // FUN_80074F24(DAT_800BF870)

  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

// assetReady — guest FUN_80045580. See actor_tomba.h for the full RE writeup.
bool ActorTomba::assetReady(Core* c, int32_t slot) {
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24;
  c->mem_w32(c->r[29] + 16, c->r[31]);

  constexpr uint32_t TABLE_8018A000 = 0x8018A000u;
  constexpr uint32_t DAT_800A3EC8   = 0x800A3EC8u;
  constexpr uint32_t SLOT_TABLE     = 0x800BE118u;   // &DAT_800be11c - 4 (slot*8 + 4 == +0xC)
  const uint32_t rec = c->mem_r32(SLOT_TABLE + (uint32_t)slot * 8u + 4u);
  c->r[4] = TABLE_8018A000;
  c->r[5] = c->mem_r32(DAT_800A3EC8);
  c->r[6] = rec;
  rec_dispatch(c, 0x80044CD4u);
  const bool ready = (int32_t)c->r[2] > 0;

  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
  return ready;
}

// outerTransitionGate — guest FUN_80053E50(G). See actor_tomba.h for the full RE writeup.
bool ActorTomba::outerTransitionGate() {
  Core* c = core;
  const uint32_t G = G_ADDR;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 28, c->r[31]);

  bool result = true;
  if (c->mem_r16s(G + 0x16Eu) > 0) {
    result = false;
    goto done;
  }

  c->mem_w8(0x800BF81Eu, 0);
  c->engine.gStateMutate(G, 0xB);

  if (c->mem_r8(G + 0x164u) == 1) {
    if ((c->mem_r8(G + 0u) & 4u) == 0) {
      if ((c->mem_r8(G + 0xDu) & 0x80u) == 0) {
        c->r[4] = 0; c->r[5] = 0x81; c->r[6] = 0x81; c->r[7] = 0x0F;
        rec_dispatch(c, 0x800521F4u);
      }
      c->mem_w8(G + 0xDu, 0);
      c->mem_w8(G + 0x61u, 0);
      c->r[4] = G; rec_dispatch(c, 0x80053D90u);
      c->mem_w8(G + 0u, 3);
      c->mem_w16(G + 0x16Eu, 0);
      c->mem_w16(G + 0x170u, 0);
      c->mem_w8(G + 0x146u, 0);
      c->mem_w8(G + 0x4u, 2);
      c->mem_w8(G + 0x5u, 1);
      c->mem_w8(G + 0x6u, 0);
      c->mem_w8(0x800BF80Du, 1);
      c->r[4] = 6; c->r[5] = G + 0x2Cu; c->r[6] = (uint32_t)-80;
      rec_dispatch(c, 0x800312D4u);
      goto done;
    }
    if ((c->mem_r8(G + 0xDu) & 0x80u) != 0) goto done;
    c->r[4] = 0; c->r[5] = 0x81; c->r[6] = 0x81; c->r[7] = 0x0F;
    rec_dispatch(c, 0x800521F4u);
    c->mem_w8(G + 0xDu, (uint8_t)(c->mem_r8(G + 0xDu) | 0x82u));
    goto done;
  }

  if (c->mem_r8s(0x800BF80Du) < 1) {
    c->r[4] = 0; c->r[5] = 0x81; c->r[6] = 0x81; c->r[7] = 0x0F;
    rec_dispatch(c, 0x800521F4u);
    c->mem_w8(G + 0xDu, 0);
    c->mem_w8(G + 0x61u, 0);
    c->r[4] = G; rec_dispatch(c, 0x80053D90u);
    c->mem_w8(G + 0u, 3);
    c->mem_w16(G + 0x16Eu, 0);
    c->mem_w16(G + 0x170u, 0);
    c->mem_w8(G + 0x146u, 0);
    c->mem_w8(G + 0x16Au, 0);            // extra clear only on this path (verified vs shard)
    c->mem_w8(G + 0x4u, 2);
    c->mem_w8(G + 0x5u, 1);
    c->mem_w8(G + 0x6u, 0);
    c->mem_w8(0x800BF80Du, 1);
    c->r[4] = 6; c->r[5] = G + 0x2Cu; c->r[6] = (uint32_t)-80;
    rec_dispatch(c, 0x800312D4u);
  }
  // else (busy latch already set): result stays true, nothing to do.

done:
  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
  return result;
}

// outerTransitionCommit — guest FUN_80053FDC(G, mode). See actor_tomba.h for the full RE writeup
// (incl. the Ghidra-vs-ground-truth correction in the decrement/settle tail below).
void ActorTomba::outerTransitionCommit(int32_t mode) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[31]);

  if (outerTransitionGate()) goto done;

  if (c->mem_r16s(G + 0x16Eu) != c->mem_r16s(G + 0x170u)) {
    // "reset to new target" — cue + gStateMutate(0xB) + conditional stop-motion clear.
    c->r[4] = 0; c->r[5] = 0x81; c->r[6] = 0x81; c->r[7] = 0x0F;
    rec_dispatch(c, 0x800521F4u);
    c->engine.gStateMutate(G, 0xB);
    c->mem_w8(0x800BF81Eu, 0);
    if ((c->mem_r8(G + 0u) & 4u) == 0) {
      c->r[4] = G; rec_dispatch(c, 0x80053D90u);
      c->mem_w8(G + 0x61u, 0);
    }
    if (mode != 1 && c->mem_r8(G + 0x4u) == 2) goto done;   // already committing — nothing to do
    // Commit a new target (shared by mode==1 and the mode!=1-but-not-yet-committing path).
    c->mem_w16(G + 0x172u, 0x5A);
    c->mem_w16(G + 0x170u, c->mem_r16(G + 0x16Eu));
    c->mem_w8(G + 0xDu, (uint8_t)(c->mem_r8(G + 0xDu) | 0x82u));
    if ((c->mem_r8(G + 0u) & 0xCu) != 0) {
      c->r[4] = 0x23; c->r[5] = 0; c->r[6] = 0;
      rec_dispatch(c, 0x80074590u);
      c->r[4] = 6; c->r[5] = G + 0x2Cu; c->r[6] = (uint32_t)-80;
      rec_dispatch(c, 0x800312D4u);
      goto done;
    }
    c->mem_w8(G + 0u, 3);
    c->mem_w8(G + 0x146u, 0);
    c->mem_w8(G + 0x145u, 0);
    c->mem_w8(G + 0x4u, 2);
    c->mem_w8(G + 0x5u, 0);
    c->mem_w8(G + 0x6u, 0);
    goto done;
  }

  // Pending counter already at target — decrement-and-settle path.
  {
    const int16_t remaining = (int16_t)c->mem_r16(G + 0x172u);
    if (remaining == 0) goto done;
    const int16_t newRemaining = (int16_t)(remaining - 1);
    c->mem_w16(G + 0x172u, (uint16_t)newRemaining);
    if (newRemaining != 0) goto done;

    const uint8_t g0 = c->mem_r8(G + 0u);
    if (g0 != 0 && (g0 & 4u) == 0) {
      // "unobstructed" — commit to walk-state 1, clearing/masking the stop-motion latch bits.
      c->mem_w8(G + 0u, 1);
      if ((c->mem_r8(G + 0xDu) & 0x50u) != 0) {
        c->mem_w8(G + 0xDu, (uint8_t)(c->mem_r8(G + 0xDu) & 0x7Fu));
      } else {
        c->mem_w8(G + 0xDu, 0);
      }
    } else {
      // g0==0 OR (g0&4)!=0 — re-arm the settle counter instead of committing.
      c->mem_w16(G + 0x172u, 1);
    }
  }

done:
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

// frameTick — guest FUN_8005950C. See actor_tomba.h for the full RE writeup.
//
// Wired 2026-07-09: EngineOverrides-only (no shard_set_override — the only direct caller of
// func_8005950C is gen_func_80059D28 = the SUBSTRATE frameStartTickFaithful, which runs on core B
// only; core A reaches 0x8005950C via the native frameStartTickFaithful's rec_dispatch, which
// consults EngineOverrides). The 5 drafted native sub-callees (turnBiasCompute / outerTransitionGate
// / outerTransitionCommit / assetReady / resetLoadGate) are NOT called from here yet — line-by-line
// verification vs generated/shard_*.c found real transcription bugs in the first two checked
// (turnBiasCompute: 0x800-threshold branches swapped; frameTick case-1 skipClear: 0x800BF80F
// condition inverted) and the fleet-workflow rule is "drafts are untrusted"; they are dispatched
// to the SUBSTRATE here so SBS gates only frameTick's own logic. Each sub-callee gets its own
// verify+fix+wire pass later. Every substrate callee dispatch sets the gen jal-site r31 constant
// first (matching gen_func_8005950C) so any callee that spills ra byte-matches core B.
//
// SBS-VERIFIED 2026-07-09: PSXPORT_SBS_MODE=full autonav, 0 sbs-div / 0 VIOLATION through f15600+.
// The first attempt diverged at f158 (A=0x1F80, B=0x0000 at an Animation::step r17 spill) — root
// caused to register-faithfulness: gen holds r17/r18 live across the case-1/4/7 callees (see the
// c->r[17]/c->r[18] writes in each case below), but the first draft used C++ locals alone, leaving
// stale caller values in those registers for the substrate callees (func_800597AC spills r18,
// func_80053FDC spills r17, func_80076D68 spills both) to spill. Mirroring gen's register
// assignments fixed it.
void ActorTomba::frameTick() {
  Core* c = core;
  const uint32_t G = c->r[4];                // a0 (== G_ADDR from both callers; matches gen's r16=r4+r0)
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G;
  c->mem_w32(c->r[29] + 28, c->r[31]);
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 20, c->r[17]);

  const uint8_t outerState = c->mem_r8(G + 0x4u);
  if (outerState < 8) {
    switch (outerState) {
      case 0: {
        c->r[4] = G; c->r[5] = 0;
        c->r[31] = 0x80059560u;
        rec_dispatch(c, 0x80058648u);                 // enterOuterState0 (substrate)
        break;
      }
      case 1: {
        const uint16_t savedE7E68 = c->mem_r16(0x800E7E68u);
        const uint16_t savedCF54  = c->mem_r16(0x800ECF54u);
        // Register-faithfulness (gen 7648/7650): r18/r17 hold the saved pair live across the
        // case-1 callees. func_800597AC spills r18, func_80053FDC spills r17 — without these
        // assignments the substrate callees spill stale caller values on core A and diverge.
        c->r[18] = savedE7E68;
        c->r[17] = savedCF54;
        if (c->mem_r8(0x1F800230u) != 0) {
          const uint16_t mask = c->mem_r16(0x1F800174u);
          c->mem_w16(0x800ECF54u, (uint16_t)(savedCF54  & ~mask));
          c->mem_w16(0x800E7E68u, (uint16_t)(savedE7E68 & ~mask));
        }
        // GT clears the turn-suppress pair unless BOTH 0x800BF80F==0 AND 0x1F800137==0
        // (the wide-RE draft inverted the 0x800BF80F condition — fixed).
        const bool skipClear = (c->mem_r8(0x800BF80Fu) == 0) && (c->mem_r8(0x1F800137u) == 0);
        if (!skipClear) { c->mem_w16(0x800E7E68u, 0); c->mem_w16(0x800ECF54u, 0); }

        c->r[5] = (uint32_t)(int16_t)c->mem_r16(G + 0x140u);
        c->r[31] = 0x800595DCu; c->r[4] = G;
        rec_dispatch(c, 0x80055C9Cu);                  // turnBiasCompute (substrate)
        c->r[31] = 0x800595E4u; c->r[4] = G;
        rec_dispatch(c, 0x80058918u);                   // mode-N dispatch table A (substrate)
        if (c->mem_r8(G + 0x146u) == 0) c->mem_w8(0x1F800232u, 1);
        c->r[31] = 0x80059604u; c->r[4] = G;
        rec_dispatch(c, 0x800597ACu);                   // matrix-compose (substrate)
        c->r[4] = G; c->r[5] = 0;
        c->r[31] = 0x80059610u;
        rec_dispatch(c, 0x80053FDCu);                   // outerTransitionCommit (substrate, mode=0)

        c->mem_w16(0x800E7E68u, savedE7E68);
        c->mem_w16(0x800ECF54u, savedCF54);
        break;
      }
      case 2: {
        c->mem_w8(G + 0x17Bu, 1);
        c->r[31] = 0x80059628u; c->r[4] = G;
        rec_dispatch(c, 0x80067CA4u);
        c->r[31] = 0x800596D8u; c->r[4] = G;
        rec_dispatch(c, 0x800597ACu);
        break;
      }
      case 3:
        break;   // unused jump-table slot (jump target = epilogue) — no-op
      case 4: {
        const uint16_t savedE7E68 = c->mem_r16(0x800E7E68u);
        const uint16_t savedCF54  = c->mem_r16(0x800ECF54u);
        // Register-faithfulness (gen 7693/7695): same live-across-callees pair as case 1.
        c->r[18] = savedE7E68;
        c->r[17] = savedCF54;
        c->mem_w8(G + 0xDu, (uint8_t)(c->mem_r8(G + 0xDu) & 0x7Fu));
        if (c->mem_r8(0x1F800137u) != 0) {
          c->mem_w16(0x800E7E68u, c->mem_r16(0x1F800166u));
          c->mem_w16(0x800ECF54u, c->mem_r16(0x1F800190u));
        } else {
          c->mem_w16(0x800E7E68u, 0);
          c->mem_w16(0x800ECF54u, 0);
        }
        c->r[5] = (uint32_t)(int16_t)c->mem_r16(G + 0x140u);
        c->r[31] = 0x8005968Cu; c->r[4] = G;
        rec_dispatch(c, 0x80055C9Cu);                  // turnBiasCompute (substrate)
        c->r[31] = 0x80059694u; c->r[4] = G;
        rec_dispatch(c, 0x80058F5Cu);                   // mode-N dispatch table B (substrate)
        c->r[31] = 0x8005969Cu; c->r[4] = G;
        rec_dispatch(c, 0x800597ACu);                   // matrix-compose (substrate)
        c->r[31] = 0x800596A4u; c->r[4] = G;
        rec_dispatch(c, 0x80053E50u);                   // outerTransitionGate (substrate, bare tick)

        c->mem_w16(0x800E7E68u, savedE7E68);
        c->mem_w16(0x800ECF54u, savedCF54);
        break;
      }
      case 5: {
        c->r[31] = 0x800596C0u; c->r[4] = G;
        rec_dispatch(c, 0x8018BD30u);                   // scripted leaf (overlay)
        c->r[31] = 0x800596D8u; c->r[4] = G;
        rec_dispatch(c, 0x800597ACu);
        break;
      }
      case 6: {
        c->r[31] = 0x800596D0u; c->r[4] = G;
        rec_dispatch(c, 0x8018BE40u);                   // scripted leaf (overlay)
        c->r[31] = 0x800596D8u; c->r[4] = G;
        rec_dispatch(c, 0x800597ACu);
        break;
      }
      case 7: {
        const uint8_t sub = c->mem_r8(G + 0x5u);
        // Register-faithfulness (gen 7736/7737): r17=sub, r18=1 held live across the sub-dispatch
        // and into the tail Animation::step call (which spills r17@+20, r18@+24). The sub==2 path
        // also stores r17/r18 verbatim into anim+0x4C/0x4E — but those are covered by the local
        // `sub` / literal 1 below; these register writes are purely for the callee spills.
        c->r[17] = sub;
        c->r[18] = 1;
        bool advance = false;
        if (sub == 0) {
          c->r[4] = G;
          c->r[31] = 0x80059720u;
          rec_dispatch(c, 0x8001CF2Cu);                 // engine tick cue (substrate)
          advance = true;
        } else if (sub == 1) {
          c->r[4] = 1;
          c->r[31] = 0x80059730u;
          rec_dispatch(c, 0x80045580u);                 // assetReady (substrate)
          if (c->r[2] != 0) advance = true;
        } else if (sub == 2) {
          if (c->mem_r8(0x1F80019Bu) != 0) {
            c->mem_w8(0x800BF89Cu, 4);
            c->r[4] = G;
            c->r[31] = 0x80059768u;
            rec_dispatch(c, 0x80042310u);               // resetLoadGate (substrate)
            c->mem_w8(G + 0x4u, 1);
            c->mem_w8(G + 0x5u, 0);
            c->mem_w8(G + 0x6u, 0);
            c->mem_w8(G + 0x7u, 0);
            const uint32_t anim = c->mem_r32(0x1F800138u);
            c->mem_w16(anim + 0x4Cu, (uint16_t)sub);   // sub==2 — matches gen's r17 trace
            c->mem_w16(anim + 0x4Eu, 1);
          }
        }
        // sub > 2: no advance, straight to tail — matches gen (no case, no-op).
        if (advance) c->mem_w8(G + 0x5u, (uint8_t)(c->mem_r8(G + 0x5u) + 1));
        c->r[4] = G;
        c->r[31] = 0x80059794u;
        rec_dispatch(c, 0x80076D68u);                   // Animation::step (substrate)
        break;
      }
    }
  }

  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

// =================================================================================
// matrixComposeAttached() — guest FUN_800597AC. 2026-07-10 wide-RE dedicated pass, UNWIRED
// (frameTick's own rec_dispatch(c, 0x800597ACu) call sites — cases 1/2/4/5/6 — still reach the
// substrate; wiring this in is a future frontier-tier step).
//
// Composes Tomba's own SRT (translate+rotate at G+0x2C.. via G+0xB8/BA/BC-derived scale, angles
// at G+0x54/56/58 read indirectly through G+0x144-adjacent bytes) plus, when G+0x8 (attach count)
// is nonzero, a per-attached-item pass over G+0xC0[i*4] (the SAME attach-record-pointer array
// GraphicsBind::recordArrayInit populates at obj+0xC0+i*4 — this is the loop that CONSUMES those
// records every frame) applying each item's own local rotation (item+0x38/3A/3C fields) composed
// onto G's matrix and adding G's + a per-item translate offset into item+0x2C/30/34 (the fresh
// record's own onto-world position, later read by the object's own render/collision code).
//
// LITERAL register-level transcription this pass (goto/label-preserving, NOT restructured into
// named locals) — deliberate per fleet-workflow.md §9 ("restructuring is where wide-RE drafts pick
// up bugs"; this function has 9 branch targets over a single densely-conditioned attach loop, so a
// first-pass mechanical port is the safer draft; a later verify/wire pass can fold it into named
// fields once every branch is independently confirmed against gen_func_800597AC). Faithful from
// generated/shard_5.c:8654 (ground truth) — every store/load/branch below is a 1:1 transcription;
// r18 = a0 = G for the whole body (gen keeps G resident in s2, not a separate local). Guest frame:
// addiu sp,-64; spill r16,r17,r18,r19,r20,r21,r22,r23 (all of s0-s7), r30 used here as a plain
// callee-saved scratch (not a frame pointer — gen loads it with the base scratchpad address
// 0x1F800000), ra(r31). One local byte at sp+16 (padded to the 64-byte frame) holds a saved copy
// of G+0x8 (attach count) across the two possible mutations of G+0x8 mid-function (case-block
// clears then restores it — see gen 8670/8919-20).
//
// Substrate callees still un-owned at draft time: FUN_800851F0 (an angle-mangling rotmat variant —
// takes the SAME anglesPtr the plain rotmat() call at 0x80059828-area builds, but with the "pan"
// field patched to a 3rd source and x/z zeroed; NOT yet triaged as its own method) and FUN_80084360
// (called right after matMul in every branch — likely a second matrix combine/copy step, mirrors
// the shape of Math::matMul's own "load result into CR0-4" side effect noted in gte_math.h;
// NOT yet triaged). Both are dispatched via rec_dispatch so this draft compiles without inventing
// their semantics. Already-native callees (matMul/rotmat/applyMatrixLV/applyMatlv/seedBlock) are
// ALSO reached via rec_dispatch here (not direct c->math.*/c->engine.nodeXform.* calls) to keep
// this pass a pure mechanical transcription — a follow-up pass can fold those into direct native
// calls once the whole function is line-verified.
void ActorTomba::matrixComposeAttached() {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 64;
  c->mem_w32(c->r[29] + 32, c->r[18]);
  c->r[18] = c->r[4] + c->r[0];
  c->mem_w32(c->r[29] + 60, c->r[31]);
  c->mem_w32(c->r[29] + 56, c->r[30]);
  c->mem_w32(c->r[29] + 52, c->r[23]);
  c->mem_w32(c->r[29] + 48, c->r[22]);
  c->mem_w32(c->r[29] + 44, c->r[21]);
  c->mem_w32(c->r[29] + 40, c->r[20]);
  c->mem_w32(c->r[29] + 36, c->r[19]);
  c->mem_w32(c->r[29] + 28, c->r[17]);
  c->mem_w32(c->r[29] + 24, c->r[16]);

  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 382u);
  c->r[8] = (uint32_t)c->mem_r8(c->r[18] + 8u);
  c->r[2] = c->r[2] & 32u;
  c->mem_w8(c->r[29] + 16u, (uint8_t)c->r[8]);
  if (c->r[2] != 0) {
    c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 377u);
    if (c->r[2] != 0) {
      c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 9u);
      c->mem_w8(c->r[18] + 8u, (uint8_t)c->r[2]);
    }
  }
  c->r[2] = 0x1F800000u;
  c->r[30] = c->r[2];
  c->r[5] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 184u);
  c->r[6] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 186u);
  c->r[7] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 188u);
  c->r[31] = 0x80059828u;
  c->r[4] = c->r[30];
  rec_dispatch(c, 0x800517BCu);                          // seedBlock(0x1F800000, x,y,z)

  c->r[19] = 0x1F800000u;
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 84u);
  c->r[17] = c->r[19] + 192u;
  c->mem_w16(c->r[19] + 192u, (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 86u);
  c->r[4] = c->r[17];
  c->mem_w16(c->r[17] + 2u, (uint16_t)c->r[2]);
  c->r[2] = 0x1F800000u;
  c->r[21] = c->r[2] + 64u;
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 88u);
  c->r[5] = c->r[21];
  c->r[31] = 0x8005985Cu;
  c->mem_w16(c->r[17] + 4u, (uint16_t)c->r[2]);
  rec_dispatch(c, 0x80085480u);                           // rotmat(anglesPtr=0x1F8000C0, outPtr=0x1F800040)

  c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 375u);
  c->r[2] = c->r[2] & 1u;
  if (c->r[2] == 0) {
    c->r[3] = (uint32_t)c->mem_r16(c->r[18] + 334u);
  } else {
    c->r[3] = 0;
  }
  c->r[4] = c->r[17];
  c->r[2] = 0x1F800000u;
  c->r[20] = c->r[2] + 32u;
  c->r[5] = c->r[20];
  c->mem_w16(c->r[19] + 192u, 0);
  c->mem_w16(c->r[17] + 2u, (uint16_t)c->r[3]);
  c->r[31] = 0x80059894u;
  c->mem_w16(c->r[17] + 4u, 0);
  rec_dispatch(c, 0x800851F0u);                           // FUN_800851F0(0x1F8000C0, outPtr=0x1F800020) — un-triaged rotmat variant

  c->r[4] = c->r[20];
  c->r[5] = c->r[30];
  c->r[16] = c->r[18] + 152u;
  c->r[31] = 0x800598A8u;
  c->r[6] = c->r[16];
  rec_dispatch(c, 0x80084110u);                           // matMul(0x1F800020, 0x1F800000, G+152)

  c->r[4] = c->r[21];
  c->r[31] = 0x800598B4u;
  c->r[5] = c->r[16];
  rec_dispatch(c, 0x80084360u);                           // FUN_80084360(0x1F800040, G+152) — un-triaged combine

  c->r[4] = c->r[16];
  c->r[22] = c->r[18] + 136u;
  c->r[5] = c->r[22];
  c->r[31] = 0x800598C8u;
  c->r[6] = c->r[18] + 172u;
  rec_dispatch(c, 0x80084470u);                           // applyMatrixLV(G+152, G+136, outPtr=G+172)

  c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 46u);
  c->r[2] = c->mem_r32(c->r[18] + 172u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 50u);
  c->r[5] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 54u);
  c->r[2] = c->r[2] + c->r[3];
  c->r[3] = c->mem_r32(c->r[18] + 180u);
  c->mem_w32(c->r[18] + 172u, c->r[2]);
  c->r[2] = c->mem_r32(c->r[18] + 176u);
  c->r[3] = c->r[3] + c->r[5];
  c->mem_w32(c->r[18] + 180u, c->r[3]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[18] + 356u);
  c->r[2] = c->r[2] + c->r[4];
  c->mem_w32(c->r[18] + 176u, c->r[2]);
  if (c->r[3] == 5u) {
    c->r[4] = c->r[16];
    c->r[5] = c->mem_r32(c->r[18] + 16u);
    c->r[31] = 0x80059914u;
    c->r[5] = c->r[5] + 24u;
    rec_dispatch(c, 0x80084250u);                         // FUN_80084250 (see game/math/wide_re_gte_transform3.cpp — drafted, ORPHAN)
  }

  bool haveAttachB = false;
  c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 325u);
  if (c->r[2] != 0) {
    c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 326u);
    c->r[2] = c->r[2] & 3u;
    if (c->r[2] != 0) {
      c->r[4] = c->r[17];
      c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 84u);
      c->mem_w16(c->r[19] + 192u, (uint16_t)c->r[2]);
      c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 86u);
      c->r[5] = c->r[21];
      c->mem_w16(c->r[4] + 4u, 0);
      c->r[31] = 0x80059958u;
      c->mem_w16(c->r[4] + 2u, (uint16_t)c->r[2]);
      rec_dispatch(c, 0x80085480u);                       // rotmat(0x1F8000C0, 0x1F800040)

      c->r[4] = c->r[20];
      c->r[5] = c->r[30];
      c->r[16] = 0x1F800000u + 96u;
      c->r[31] = 0x80059970u;
      c->r[6] = c->r[16];
      rec_dispatch(c, 0x80084110u);                       // matMul(0x1F800020, 0x1F800000, 0x1F800060)

      c->r[4] = c->r[21];
      c->r[31] = 0x8005997Cu;
      c->r[5] = c->r[16];
      rec_dispatch(c, 0x80084360u);                       // FUN_80084360(0x1F800040, 0x1F800060)

      c->r[4] = c->r[16];
      c->r[5] = c->r[22];
      c->r[31] = 0x8005998Cu;
      c->r[6] = c->r[16] + 20u;
      rec_dispatch(c, 0x80084470u);                       // applyMatrixLV(0x1F800060, G+136, outPtr=0x1F800074)

      c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 46u);
      c->r[2] = c->mem_r32(c->r[16] + 20u);
      c->r[2] = c->r[2] + c->r[3];
      c->mem_w32(c->r[16] + 20u, c->r[2]);
      c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 50u);
      c->r[2] = c->mem_r32(c->r[16] + 24u);
      c->r[2] = c->r[2] + c->r[3];
      c->mem_w32(c->r[16] + 24u, c->r[2]);
      c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[18] + 54u);
      c->r[2] = c->mem_r32(c->r[16] + 28u);
      c->r[22] = 1u;
      c->r[2] = c->r[2] + c->r[3];
      c->mem_w32(c->r[16] + 28u, c->r[2]);
      haveAttachB = true;
    }
  }
  if (!haveAttachB) c->r[22] = 0u;

  // Per-attached-item loop: for i in [0, G+8), item = *(G+0xC0 + i*4); compose G's SRT (or, when
  // (G+0x146&3)!=0, the alt "attach B" matrix at 0x1F800060 computed above) onto item's own local
  // rotation and accumulate the translate into item+0x2C/30/34.
  c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 9u);
  if (c->r[2] != 0) {
    c->r[2] = 0x1F800000u;
    c->r[30] = c->r[2] + 32u;
    c->r[2] = 0x1F800000u;
    c->r[20] = c->r[2] + 64u;
    c->r[2] = 0x1F800000u;
    c->r[21] = c->r[2] + 96u;
    c->r[17] = c->r[18];
    c->r[19] = 0;
    while (true) {
      c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 8u);
      if (!((int32_t)c->r[19] < (int32_t)c->r[2])) break;
      c->r[2] = c->mem_r32(c->r[17] + 192u);
      const uint32_t item = c->r[2];
      c->r[4] = 0x1F800000u;
      c->r[5] = (uint32_t)(int16_t)c->mem_r16(item + 56u);
      c->r[6] = (uint32_t)(int16_t)c->mem_r16(item + 58u);
      c->r[7] = (uint32_t)(int16_t)c->mem_r16(item + 60u);
      c->r[16] = (uint32_t)(int16_t)c->mem_r16(item + 6u);
      c->r[31] = 0x80059A34u;
      rec_dispatch(c, 0x800517BCu);                       // seedBlock(0x1F800000, item local rot x/y/z)

      c->r[4] = c->mem_r32(c->r[17] + 192u);
      c->r[5] = c->r[30];
      c->r[31] = 0x80059A44u;
      c->r[4] = c->r[4] + 8u;
      rec_dispatch(c, 0x80085480u);                       // rotmat(item+8, 0x1F800020)

      c->r[4] = c->r[30];
      c->r[5] = 0x1F800000u;
      c->r[31] = 0x80059A58u;
      c->r[6] = c->r[20];
      rec_dispatch(c, 0x80084110u);                       // matMul(0x1F800020, 0x1F800000, 0x1F800040)

      uint32_t dstPtr;
      if ((int32_t)c->r[16] >= 0) {
        c->r[16] = c->r[16] << 2;
        c->r[16] = c->r[18] + c->r[16];
        c->r[4] = c->mem_r32(c->r[16] + 192u);
        c->r[6] = c->mem_r32(c->r[17] + 192u);
        c->r[4] = c->r[4] + 24u;
        c->r[31] = 0x80059BB0u;
        c->r[6] = c->r[6] + 24u;
        rec_dispatch(c, 0x80084110u);                     // matMul(prevItem+24, thisItem+24, ...)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[31] = 0x80059BBCu;
        c->r[5] = c->r[4] + 44u;
        rec_dispatch(c, 0x80084220u);                     // applyMatlv(item+44)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[16] + 192u);
        c->r[2] = c->mem_r32(c->r[4] + 44u);
        c->r[3] = c->mem_r32(c->r[3] + 44u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 44u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[16] + 192u);
        c->r[2] = c->mem_r32(c->r[4] + 48u);
        c->r[3] = c->mem_r32(c->r[3] + 48u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 48u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[16] + 192u);
        c->r[2] = c->mem_r32(c->r[4] + 52u);
        c->r[3] = c->mem_r32(c->r[3] + 52u);
        c->r[2] = c->r[2] + c->r[3];
        dstPtr = c->r[4];
      } else if (c->r[22] == 0) {
        // "attach B" branch never armed this frame — compose against G's own matrix (G+152).
        c->r[6] = c->mem_r32(c->r[17] + 192u);
        c->r[5] = c->r[20];
        c->r[31] = 0x80059B44u;
        c->r[6] = c->r[6] + 24u;
        rec_dispatch(c, 0x80084110u);                     // matMul(0x1F800040, item+24, ...)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[31] = 0x80059B50u;
        c->r[5] = c->r[4] + 44u;
        rec_dispatch(c, 0x80084220u);                     // applyMatlv(item+44)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[18] + 172u);
        c->r[2] = c->mem_r32(c->r[4] + 44u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 44u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[18] + 176u);
        c->r[2] = c->mem_r32(c->r[4] + 48u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 48u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[18] + 180u);
        c->r[2] = c->mem_r32(c->r[4] + 52u);
        c->r[2] = c->r[2] + c->r[3];
        dstPtr = c->r[4];
      } else {
        // "attach B" branch armed — compose against the alt matrix at 0x1F800060.
        c->r[4] = c->r[21];
        c->r[6] = c->mem_r32(c->r[17] + 192u);
        c->r[5] = c->r[20];
        c->r[31] = 0x80059AE4u;
        c->r[6] = c->r[6] + 24u;
        rec_dispatch(c, 0x80084110u);                     // matMul(0x1F800040, item+24, 0x1F800060)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[31] = 0x80059AF0u;
        c->r[5] = c->r[4] + 44u;
        rec_dispatch(c, 0x80084220u);                     // applyMatlv(item+44)

        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[21] + 20u);
        c->r[2] = c->mem_r32(c->r[4] + 44u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 44u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[21] + 24u);
        c->r[2] = c->mem_r32(c->r[4] + 48u);
        c->r[2] = c->r[2] + c->r[3];
        c->mem_w32(c->r[4] + 48u, c->r[2]);
        c->r[4] = c->mem_r32(c->r[17] + 192u);
        c->r[3] = c->mem_r32(c->r[21] + 28u);
        c->r[2] = c->mem_r32(c->r[4] + 52u);
        c->r[2] = c->r[2] + c->r[3];
        c->r[23] = 1u;
        dstPtr = c->r[4];
      }
      c->mem_w32(dstPtr + 52u, c->r[2]);

      c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 9u);
      c->r[19] = c->r[19] + 1u;
      if (!((int32_t)c->r[19] < (int32_t)c->r[2])) break;
      c->r[17] = c->r[17] + 4u;
    }
  }

  c->r[8] = (uint32_t)c->mem_r8(c->r[29] + 16u);
  c->mem_w8(c->r[18] + 8u, (uint8_t)c->r[8]);
  c->r[31] = c->mem_r32(c->r[29] + 60);
  c->r[30] = c->mem_r32(c->r[29] + 56);
  c->r[23] = c->mem_r32(c->r[29] + 52);
  c->r[22] = c->mem_r32(c->r[29] + 48);
  c->r[21] = c->mem_r32(c->r[29] + 44);
  c->r[20] = c->mem_r32(c->r[29] + 40);
  c->r[19] = c->mem_r32(c->r[29] + 36);
  c->r[18] = c->mem_r32(c->r[29] + 32);
  c->r[17] = c->mem_r32(c->r[29] + 28);
  c->r[16] = c->mem_r32(c->r[29] + 24);
  c->r[29] = sp0;
}

// =================================================================================
// enterOuterState0(mode) — guest FUN_80058648(G, mode). 2026-07-10 wide-RE dedicated pass,
// UNWIRED (frameTick's case-0 branch still rec_dispatch(c, 0x80058648u)s to the substrate).
//
// Tomba's OUTER-STATE-0 (INIT) driver. Calls GraphicsBind::recordArrayInit(G, 17, *(0x800ED054),
// 0x800A3FA8) (already-native) to (re)allocate Tomba's 17-record attach array (the SAME array
// matrixComposeAttached's per-item loop walks at G+0xC0); on a bail (insufficient growth budget —
// recordArrayInit's own `mem_r16s(0x800ED098) < count` early-return path, see graphics_bind.cpp)
// skips straight to the matrix-compose tail. On success: zeroes a large block of G's per-frame
// scratch fields (G+0x40/42/178/50/14E), bumps G+4 (the outer-state byte itself — plausible
// "re-armed" bookkeeping the caller's own switch doesn't re-read until next frame), stamps fixed
// defaults (G+0xB..0x163 region: hitbox radius 15, alpha 255x2, timer trio 1/7/4), snapshots
// G+0x2C/30/34 (world position) into a scratch struct at 0x1F8000D0+12/16/20, clears a second
// cluster of G fields (0x180/175/148/29/145/16B), then dispatches FUN_800682C4(G, mode) (still
// substrate — un-triaged) and ActorTomba::growthStep(G, sign-bit of G+0x17E) (ALREADY NATIVE —
// dispatched here via rec_dispatch, not a direct call, to keep this pass a pure mechanical
// transcription; a follow-up verify pass should fold it to `growthStep(...)` directly) and
// FUN_80057FD4(G) (still substrate — the lift/ride interaction handler, docs/engine_re.md's
// "0x80057FD4" mapped-only entry).
//
// When `mode` (a1) == 0: reads DAT_800BF870 (the area/game-mode selector byte — the SAME global
// postFrameWaterCheck/interactWalk gate on) and, via a small comparison cascade (== 3 / < 4 / == 2
// / == 7 / == 20), picks one of TWO 32-entry jump tables — 0x800A45B8 (docs' PTR_DAT_800a45b8) or
// its companion 0x800C45B8 — indexed by DAT_800BF870 again, and rec_dispatches to
// `*(table + DAT_800BF870*4)` with a0=G. This is the "indirect table dispatch" docs/engine_re.md
// flagged as un-drafted; the table CONTENTS (32 function pointers per table) are a follow-up
// dedicated-pass target — this draft only reproduces the SELECTION + dispatch mechanics, not the
// table entries themselves (they stay opaque `rec_dispatch(c, c->r[2])` — the target address is
// read from guest memory at runtime, matching gen exactly).
// Tail (all mode values): reads DAT_1F800000+566 (a mode/animation-slot byte); if it's in {5,6}
// (checked via `(byte-5) < 2`), and G+2 is 0, bumps a scratch G+86 field by 1024 (Q12 +1.0) before
// stamping G's own state machine to a fixed "settle" preset (G+0=6, 0x1F8000000x137=2, G+4=4,
// G+5=40) and firing FUN_80068214(G) (still substrate — un-triaged); if the byte is NOT in {5,6},
// falls into the "G+348 gate" branch instead: no-op if G+348==0, else stamps G+4=4/G+5=36 and
// either G+6=4 (G+2==0) or G+6=0 (G+2!=0) [gen's own branch condition is on G+2, re-read as r3,
// NOT G+348's own value], then clears G+7 and G+348. Finally always dispatches matrixComposeAttached
// (0x800597AC — drafted above, still reached via rec_dispatch here since THIS draft is unwired).
//
// LITERAL register-level transcription (goto/label-preserving) per fleet-workflow.md §9 — a first
// restructuring attempt at the 566-byte "{5,6} vs G+348" tail (lines gen 7843-7877) mis-inverted
// BOTH inner branch polarities (G+320-increment gate at gen 7849, and G+6=0-vs-4 gate at gen 7871)
// before this mechanical pass caught it by re-diffing line-by-line against generated/shard_7.c —
// concrete evidence for why this function stays a literal transcription rather than clean control
// flow until a dedicated verify pass. Faithful from generated/shard_7.c:7739 (ground truth). Guest
// frame: addiu sp,-32; spill s0(r16)<-a0=G, s1(r17), s2(r18)<-a1=mode, ra(r31).
void ActorTomba::enterOuterState0(int32_t mode) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = c->r[4] + c->r[0];
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->r[18] = c->r[5] + c->r[0];
  c->r[5] = c->r[0] + 17u;
  c->r[2] = (uint32_t)32783u << 16;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = c->r[2] + (uint32_t)-12456;
  c->r[7] = (uint32_t)32778u << 16;
  c->mem_w32(c->r[29] + 28, c->r[31]);
  c->r[6] = c->mem_r32(c->r[17] + 188u);
  c->r[31] = 0x80058680u;
  c->r[7] = c->r[7] + 16296u;
  rec_dispatch(c, 0x800519E0u);          // GraphicsBind::recordArrayInit(G,17,*(0x800ED054),0x800A3FA8)
  {
    const bool bail = (c->r[2] != c->r[0]);
    c->r[3] = (uint32_t)8064u << 16;
    if (bail) goto L_800588A4;
  }
  c->r[2] = c->r[0] + (uint32_t)-1;
  c->mem_w8(c->r[16] + 12u, (uint8_t)c->r[0]);
  c->mem_w16(c->r[3] + 530u, (uint16_t)c->r[2]);
  c->r[3] = c->mem_r32(c->r[17] + 16u);
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 4u);
  c->mem_w16(c->r[16] + 64u, 0);
  c->mem_w16(c->r[16] + 66u, 0);
  c->mem_w8(c->r[16] + 376u, 0);
  c->mem_w16(c->r[16] + 80u, 0);
  c->mem_w16(c->r[16] + 334u, 0);
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w16(c->r[2] + 358u, 0);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w16(c->r[2] + 400u, 0);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w8(c->r[2] + 594u, 0);
  c->r[2] = c->r[0] + 15u;
  c->mem_w8(c->r[16] + 11u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 255u;
  c->mem_w8(c->r[16] + 70u, (uint8_t)c->r[2]);
  c->mem_w8(c->r[16] + 71u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 1u;
  c->mem_w8(c->r[16] + 353u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 7u;
  c->mem_w8(c->r[16] + 354u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 4u;
  c->mem_w8(c->r[16] + 355u, (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w32(c->r[16] + 60u, c->r[3]);
  c->r[3] = c->mem_r32(c->r[16] + 44u);
  c->r[2] = c->r[2] + 208u;
  c->mem_w8(c->r[16] + 384u, 0);
  c->mem_w8(c->r[16] + 373u, 0);
  c->mem_w8(c->r[16] + 328u, 0);
  c->mem_w8(c->r[16] + 41u, 0);
  c->mem_w8(c->r[16] + 325u, 0);
  c->mem_w8(c->r[16] + 363u, 0);
  c->mem_w32(c->r[2] + 12u, c->r[3]);
  c->r[3] = c->mem_r32(c->r[16] + 48u);
  c->r[4] = c->r[16] + c->r[0];
  c->mem_w32(c->r[2] + 16u, c->r[3]);
  c->r[3] = c->mem_r32(c->r[16] + 52u);
  c->r[5] = c->r[18] + c->r[0];
  c->r[31] = 0x80058744u;
  c->mem_w32(c->r[2] + 20u, c->r[3]);
  rec_dispatch(c, 0x800682C4u);          // still-substrate leaf FUN_800682C4(G, mode) — un-triaged
  c->r[5] = (uint32_t)(int16_t)c->mem_r16(c->r[16] + 382u);
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[5] >> 15;
  c->r[31] = 0x80058758u;
  c->r[5] = c->r[5] & 1u;
  rec_dispatch(c, 0x80057DC0u);          // ActorTomba::growthStep(G, sign-bit(G+0x17E)) — ALREADY NATIVE, dispatched here for now
  c->r[4] = c->r[16] + c->r[0];
  c->r[2] = (uint32_t)8064u << 16;
  c->r[31] = 0x80058768u;
  c->mem_w16(c->r[2] + 398u, 0);
  rec_dispatch(c, 0x80057FD4u);          // still-substrate leaf FUN_80057FD4(G) — lift/ride handler, un-triaged
  {
    const bool skip = (c->r[18] != c->r[0]);
    c->r[2] = (uint32_t)32780u << 16;
    if (skip) goto L_8005889C;
  }
  c->r[3] = (uint32_t)c->mem_r8(c->r[2] + (uint32_t)-1936);   // DAT_800BF870 (area/game-mode selector)
  c->r[2] = c->r[0] + 3u;
  {
    const bool eq3 = (c->r[3] == c->r[2]);
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 4);
    if (eq3) goto L_800587B8;
  }
  {
    const bool notLt4 = (c->r[2] == c->r[0]);
    c->r[2] = c->r[0] + 2u;
    if (notLt4) goto L_80058798;
  }
  {
    const bool eq2 = (c->r[3] == c->r[2]);
    c->r[3] = (uint32_t)32778u << 16;
    if (eq2) goto L_800587C0;
  }
  c->r[2] = (uint32_t)32780u << 16;
  goto L_800587D4;
L_80058798:
  c->r[2] = c->r[0] + 7u;
  {
    const bool eq7 = (c->r[3] == c->r[2]);
    c->r[2] = c->r[0] + 1u;
    if (eq7) goto L_800587C4;
  }
  c->r[2] = c->r[0] + 20u;
  {
    const bool eq20 = (c->r[3] == c->r[2]);
    c->r[3] = (uint32_t)32778u << 16;
    if (eq20) goto L_800587C0;
  }
  c->r[2] = (uint32_t)32780u << 16;
  goto L_800587D4;
L_800587B8:
  c->mem_w8(c->r[16] + 2u, 0);
  goto L_800587F8;
L_800587C0:
  c->r[2] = c->r[0] + 1u;
L_800587C4:
  c->r[3] = (uint32_t)32778u << 16;
  c->mem_w8(c->r[16] + 2u, (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)32780u << 16;
  goto L_800587D8;
L_800587D4:
  c->mem_w8(c->r[16] + 2u, 0);
L_800587D8:
  c->r[2] = (uint32_t)c->mem_r8(c->r[2] + (uint32_t)-1936);
  c->r[3] = c->r[3] + 17848u;
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->mem_r32(c->r[2] + 0u);
  c->r[31] = 0x800587F8u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, c->r[2]);              // table-dispatched substrate leaf (table @ 0x800A45B8 or 0x800C45B8, indexed by DAT_800BF870)
L_800587F8:
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r8(c->r[2] + 566u);
  c->r[2] = c->r[2] + (uint32_t)-5;
  c->r[2] = (uint32_t)(c->r[2] < 2u);
  {
    const bool in5or6 = (c->r[2] == c->r[0]);
    if (in5or6) goto L_80058864;
  }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 2u);
  {
    const bool skipBump = (c->r[2] != c->r[0]);
    c->r[4] = c->r[16] + c->r[0];
    if (skipBump) goto L_80058834;
  }
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 320u);
  c->r[2] = c->r[2] + 1024u;
  c->mem_w16(c->r[16] + 86u, (uint16_t)c->r[2]);
L_80058834:
  c->r[2] = c->r[0] + 6u;
  c->r[3] = (uint32_t)8064u << 16;
  c->mem_w8(c->r[16] + 0u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 2u;
  c->mem_w8(c->r[3] + 311u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 4u;
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + 40u;
  c->r[31] = 0x8005885Cu;
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);
  rec_dispatch(c, 0x80068214u);          // still-substrate leaf FUN_80068214(G) — un-triaged
  goto L_8005889C;
L_80058864:
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 348u);
  {
    const bool clear = (c->r[2] == c->r[0]);
    c->r[4] = c->r[0] + 4u;
    if (clear) goto L_8005889C;
  }
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 2u);
  c->r[2] = c->r[0] + 36u;
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[4]);
  {
    const bool g2set = (c->r[3] != c->r[0]);
    c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);
    if (g2set) goto L_80058890;
  }
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[4]);
  goto L_80058894;
L_80058890:
  c->mem_w8(c->r[16] + 6u, 0);
L_80058894:
  c->mem_w8(c->r[16] + 7u, 0);
  c->mem_w8(c->r[16] + 348u, 0);
L_8005889C:
  c->r[31] = 0x800588A4u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800597ACu);          // matrixComposeAttached (drafted above; still dispatched — this draft is unwired)
L_800588A4:
  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}
