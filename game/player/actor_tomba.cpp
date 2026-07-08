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

void ActorTomba::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x80020364u, "ActorTomba::stepModeInteract", ov_stepModeInteract);
  ov.register_(0x800205CCu, "ActorTomba::type8Interact",    ov_type8Interact);
  ov.register_(0x800235A0u, "ActorTomba::type7Interact",    ov_type7Interact);
  ov.register_(0x80022C78u, "ActorTomba::growthYSnap",      ov_growthYSnap);
}
