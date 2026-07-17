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
#include "game_ctx.h"
#include "core.h"
#include "cfg.h"
#include "core/engine.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "game.h"
#include "guest_abi.h"    // GuestFrame/GuestReg/guest_fn — frameTick + the outer-transition cluster
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

// TombaState — named-field lens over Tomba's G block (ActorTomba::G_ADDR / a `G` local passed in
// by the per-frame driver). Guest addresses are UNCHANGED from the raw c->mem_r/w8/16 pokes this
// replaces (see actor_tomba.h's per-function doc comments for the RE of each field); this is a
// readability lens, not a state migration — every accessor is still a direct guest-RAM access.
// Scoped to the fields the frameTick / outer-transition-gate / outer-transition-commit cluster
// touches; a wider pass can grow this lens as more of the file gets ported.
struct TombaState {
  Core* c;
  uint32_t base;

  // outerState (+0x4): frameTick's own top-level FSM selector (0=INIT..7=LOAD-WAIT). Also used,
  // within LOAD-WAIT, as the settle target frameTick's case-7 sub-machine writes back to (=1).
  uint8_t outerState() const           { return c->mem_r8(base + 0x4u); }
  void setOuterState(uint8_t v)        { c->mem_w8(base + 0x4u, v); }

  // loadStep/loadSub/loadSub2 (+0x5/+0x6/+0x7): the 3-state LOAD-WAIT sub-machine counter
  // (frameTick case 7) — also the "G+5=1,G+6=0" pair outerTransitionGate/Commit write when they
  // commit a fresh walk-state.
  uint8_t loadStep() const             { return c->mem_r8(base + 0x5u); }
  void setLoadStep(uint8_t v)          { c->mem_w8(base + 0x5u, v); }
  void setLoadSub(uint8_t v)           { c->mem_w8(base + 0x6u, v); }
  void setLoadSub2(uint8_t v)          { c->mem_w8(base + 0x7u, v); }

  // statusFlags (+0x0): walk-state / cutscene-lock byte. Bit 4 and the 0xC mask gate the
  // outer-transition commit path; literal 3 is the "reset to walk" stamp.
  uint8_t statusFlags() const          { return c->mem_r8(base + 0x0u); }
  void setStatusFlags(uint8_t v)       { c->mem_w8(base + 0x0u, v); }

  // latchFlags (+0xD): stop-motion / facing-lock bitfield (0x80 busy, 0x50 lock mask, 0x82 armed).
  uint8_t latchFlags() const           { return c->mem_r8(base + 0xDu); }
  void setLatchFlags(uint8_t v)        { c->mem_w8(base + 0xDu, v); }

  // stopMotionAux (+0x61): companion byte cleared alongside latchFlags on a walk-state reset.
  void setStopMotionAux(uint8_t v)     { c->mem_w8(base + 0x61u, v); }

  // facing (+0x140, s16): Tomba's current heading — turnBiasCompute's `facing` arg source.
  int16_t facing() const               { return (int16_t)c->mem_r16(base + 0x140u); }

  // turnSuppressGate (+0x146): "already turn-suppressed" flag frameTick checks post-turnBias.
  uint8_t turnSuppressGate() const     { return c->mem_r8(base + 0x146u); }
  void setTurnSuppressGate(uint8_t v)  { c->mem_w8(base + 0x146u, v); }

  // transitionSlot (+0x164): the interaction-slot state outerTransitionGate branches on (1 = a
  // specific slot -> a stop-motion spawn without the busy-latch check; else gated by 0x800BF80D).
  uint8_t transitionSlot() const       { return c->mem_r8(base + 0x164u); }

  // extraClear (+0x16A): cleared only on outerTransitionGate's busy-latch branch (verified vs
  // ground truth shard — not present on the transitionSlot==1 branch).
  void setExtraClear(uint8_t v)        { c->mem_w8(base + 0x16Au, v); }

  // turnCurrent/turnTarget (+0x16E/+0x170, s16): the pending-frame turn counter and its commit
  // target. outerTransitionGate bails while turnCurrent is still positive; outerTransitionCommit
  // arms turnTarget = turnCurrent when they differ.
  int16_t turnCurrent() const          { return (int16_t)c->mem_r16(base + 0x16Eu); }
  void setTurnCurrent(uint16_t v)      { c->mem_w16(base + 0x16Eu, v); }
  int16_t turnTarget() const           { return (int16_t)c->mem_r16(base + 0x170u); }
  void setTurnTarget(uint16_t v)       { c->mem_w16(base + 0x170u, v); }

  // settleCounter (+0x172, s16): outerTransitionCommit's decrement-and-settle counter; reaching 0
  // either commits walk-state 1 or re-arms to 1 depending on statusFlags.
  int16_t settleCounter() const        { return (int16_t)c->mem_r16(base + 0x172u); }
  void setSettleCounter(uint16_t v)    { c->mem_w16(base + 0x172u, v); }

  // committing (+0x17B): frameTick case-2 (COMMITTING) latch, set on entry to that state.
  void setCommitting(uint8_t v)        { c->mem_w8(base + 0x17Bu, v); }

  // posAddr(): +0x2C — Tomba's position triple, passed BY ADDRESS to the stop-motion spawn call
  // (guest FUN_800312D4 takes a dest pointer, not a value).
  uint32_t posAddr() const             { return base + 0x2Cu; }

  // -- Extended 2026-07-15 (2nd code-quality pass): interactWalk/proximityCheck/subHitboxCheck/
  // postInteractWalk/postFrameWaterCheck/type8Interact/type7Interact cluster. TombaState is
  // constructed over G_ADDR for Tomba himself, but proximityCheck/subHitboxCheck/stepModeInteract/
  // type8Interact all read an ITEM node at the SAME field offsets (0x2E/0x32/0x36/0x80/0x84/0x86)
  // — items share Tomba's node layout, so this lens doubles as a generic actor-node view: wrap it
  // over `item` too (`TombaState other{c, item}`) rather than reading item+0xNN raw.

  // posX/posY/posZ (+0x2E/+0x32/+0x36, s16): world position triple interactWalk's proximity math
  // and postFrameWaterCheck's off-map check read/write. NOT the same triple as posAddr() (+0x2C) —
  // RE unresolved why the two don't coincide; posAddr() is only ever used as a spawn dest pointer.
  int16_t posX() const                 { return (int16_t)c->mem_r16(base + 0x2Eu); }
  void setPosX(int16_t v)              { c->mem_w16(base + 0x2Eu, (uint16_t)v); }
  int16_t posY() const                 { return (int16_t)c->mem_r16(base + 0x32u); }
  void setPosY(int16_t v)              { c->mem_w16(base + 0x32u, (uint16_t)v); }
  int16_t posZ() const                 { return (int16_t)c->mem_r16(base + 0x36u); }
  void setPosZ(int16_t v)              { c->mem_w16(base + 0x36u, (uint16_t)v); }

  // boundXZ (+0x80, s16): horizontal (X/Z) cylinder-proximity radius. boundYUp (+0x84, READ
  // UNSIGNED — ground truth never sign-extends this field) / boundYDown (+0x86, s16): the two
  // halves of the vertical proximity band, summed between two nodes in proximityCheck/
  // subHitboxCheck's Y-band test.
  int16_t boundXZ() const              { return (int16_t)c->mem_r16s(base + 0x80u); }
  uint16_t boundYUp() const            { return c->mem_r16(base + 0x84u); }
  int16_t boundYDown() const           { return (int16_t)c->mem_r16s(base + 0x86u); }

  // growthFlags (+0x17E, u16): bit 0x200 = "paused/frozen" (interactWalk/stepModeInteract/
  // type8Interact all early-out or branch on it), bit 0x8000 = "grown" (growthStep toggles it;
  // stepModeInteract/type8Interact branch on it to route to the grown-state delegate leaves).
  uint16_t growthFlags() const         { return c->mem_r16(base + 0x17Eu); }

  // justTransitioned (+0x144, u8): "just entered this interaction state" latch — stepModeInteract/
  // type8Interact both special-case `==1 && v0<2` as the just-triggered transition frame.
  uint8_t justTransitioned() const     { return c->mem_r8(base + 0x144u); }

  // groundedGate (+0x145, u8) bit 0: postFrameWaterCheck/type8Interact/growthYSnap check bit 0
  // clear before snapping the position to a water/growth-offset target.
  uint8_t groundedGate() const         { return c->mem_r8(base + 0x145u); }
  void setGroundedGate(uint8_t v)      { c->mem_w8(base + 0x145u, v); }

  // frozenFlag (+0x78, u8): "not frozen" gate type8Interact/growthYSnap check before a niladic
  // cue / a growth-offset re-snap.
  uint8_t frozenFlag() const           { return c->mem_r8(base + 0x78u); }

  // flag95 (+0x5F, u8) / groundContactFlag (+0x60, u8): the header's own names (see actor_tomba.h
  // "settleStep"/"type8Interact" doc comments) for the ground-probe response pair type8Interact's
  // proximity-hit branch stamps.
  void setFlag95(uint8_t v)            { c->mem_w8(base + 0x5Fu, v); }
  void setGroundContactFlag(uint8_t v) { c->mem_w8(base + 0x60u, v); }

  // physOffsetY (+0x62, s16): the "physics constant" growthStep rescales (header: "G+0x62/64/66/
  // 68 (physics constants)") — postFrameWaterCheck reuses it as the water-surface-to-feet offset.
  int16_t physOffsetY() const          { return (int16_t)c->mem_r16s(base + 0x62u); }

  // committing (+0x17B, u8): frameTick case-2 (COMMITTING) latch — see setCommitting() above;
  // postFrameWaterCheck's off-map trigger gates on it being clear.
  uint8_t committing() const           { return c->mem_r8(base + 0x17Bu); }
};

}  // namespace

// =================================================================================
// Per-frame interaction walk (FUN_80022760)
// =================================================================================
void ActorTomba::interactWalk() {
  Core* c = core;
  const uint32_t G = G_ADDR;
  TombaState tomba{c, G};

  // Early-outs.
  if (tomba.turnCurrent() == 0)                          return;
  if (c->mem_r8 (GATE_BF80C_hi) != 0)                    return;
  if (tomba.growthFlags() & 0x200u)                      return;

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

  TombaState tomba{c, G};
  TombaState other{c, item};

  const int32_t dx = (int32_t)(int16_t)(tomba.posX() - other.posX());
  const int32_t dz = (int32_t)(int16_t)(tomba.posZ() - other.posZ());
  c->r[4] = (uint32_t)(dx * dx + dz * dz);
  rec_dispatch(c, LEAF_ISQRT);
  const int32_t dist = (int32_t)(int16_t)(uint16_t)c->r[2];

  const int32_t rxz = (int32_t)tomba.boundXZ() + (int32_t)other.boundXZ();
  if (dist > rxz) return;

  const int32_t vbandRaw = (int32_t)(int16_t)(uint16_t)(
      (tomba.posY() - other.posY()) + tomba.boundYUp() + other.boundYUp());
  const int32_t vbandLim = (int32_t)tomba.boundYDown() + (int32_t)other.boundYDown();
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
  TombaState tomba{c, G};
  const int16_t hitboxCount = (int16_t)c->mem_r16(item + 0x6Au);
  if (hitboxCount <= 0) return;
  uint32_t hitboxArr = c->mem_r32(item + 0x6Cu);

  for (int32_t i = 0; i < hitboxCount; i++, hitboxArr += 0x10u) {
    const uint32_t mask = 1u << (i & 0x1F);
    if ((c->mem_r32(item + 0x70u) & mask) == 0) continue;

    const uint32_t typeParam = (uint32_t)c->mem_r8(hitboxArr + 3u) * 8u;
    const int32_t dx = (int32_t)(int16_t)(tomba.posX() - c->mem_r16(hitboxArr + 4u));
    const int32_t dz = (int32_t)(int16_t)(tomba.posZ() - c->mem_r16(hitboxArr + 8u));
    c->r[4] = (uint32_t)(dx * dx + dz * dz);
    rec_dispatch(c, LEAF_ISQRT);
    const int32_t dist = (int32_t)(uint32_t)(c->r[2] & 0xFFFFu);

    const int32_t rxz = (int32_t)tomba.boundXZ()
                       + (int32_t)c->mem_r8(SUB_HITBOX_PARAMS + typeParam + 0u);
    if (dist > rxz) continue;

    const uint32_t vbandRaw = (uint32_t)(
        (tomba.posY() - c->mem_r16(hitboxArr + 6u))
        + tomba.boundYUp() + c->mem_r8(SUB_HITBOX_PARAMS + typeParam + 1u));
    const int32_t vbandLim = (int32_t)tomba.boundYDown()
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
  TombaState tomba{c, G};

  // This walker uses a DIFFERENT aux list than interactWalk: the render/interaction queue at
  // *0x1F80013C with count *0x1F800144 (vs 0x1F800154 / 0x1F80015C for interactWalk).
  constexpr uint32_t LIST_HEAD_SPAD  = 0x1F80013Cu;
  constexpr uint32_t LIST_COUNT_SPAD = 0x1F800144u;

  // Sub-handler leaves (all substrate — the type-dispatch tree is its own future frontier). No
  // jal-site r31 constants: gen never assigns r31 before these calls (postInteractWalk is itself a
  // frameless leaf, so r31 here is whatever its own caller left) — guest_leaf, not guest_fn.
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
      if ((tomba.growthFlags() & 0x8200u) == 0) {
        guest_leaf(c, LEAF_TYPE_9_SPECIAL, G, item);
      }
      continue;                                                   // keep walking
    }
    const uint8_t typ = c->mem_r8(item + 2u);
    switch (typ) {
      case 3:
        guest_leaf(c, LEAF_TYPE_3, G, item);
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
        const int32_t v0 = (int32_t)guest_leaf(c, LEAF_TYPE_4_PROX_STEP, G, item, 1u);
        if (v0 < 0) break;                                        // no hit
        c->mem_w8(AUX_WALK_COUNTER, 0);                           // stop the walk
        const uint8_t bf9e5 = c->mem_r8(0x800BF9E5u);
        const uint8_t g144  = tomba.justTransitioned();
        if (bf9e5 == 6 && g144 == 1 && v0 < 2) {
          guest_leaf(c, LEAF_TYPE_4_TAG_SET, item, 0xFFFF8001u, 0x10u, 0x20u);
          break;                                                  // continue at loop top (via while)
        }
        if (c->mem_r8(0x1F800137u) != 0)                          break;
        if ((tomba.statusFlags() & 6) != 0)                       break;
        if (g144 > 1)                                             break;
        if (tomba.transitionSlot() != 0)                          break;
        if (bf9e5 != 6) {
          eng(c).announcerCue(0x2A, 0x41);                     // native FUN_8004ED94
        }
        // Type-4 hit state transition on G + item.
        c->mem_w8(G + 4, 2);
        c->mem_w8(G + 5, 2);
        tomba.setStatusFlags(3);
        c->mem_w8(G + 6, 0);
        c->mem_w8(G + 0x172u, 0x78);
        c->mem_w8(G + 0x173u, 0);
        c->mem_w8(G + 0x2Bu, (uint8_t)((int32_t)c->mem_r32(OUT_HEADING_SPAD) >> 4));
        break;
      }
      case 7:
        guest_leaf(c, LEAF_TYPE_7, G, item);
        break;
      case 8:
        guest_leaf(c, LEAF_TYPE_8, G, item);
        break;
      case 0x0F: case 0x14: case 0x56:
        guest_leaf(c, LEAF_TYPE_0F_14_56, G, item, 0u);
        break;
      case 0x13:
        guest_leaf(c, LEAF_TYPE_13, G, item);
        break;
      case 0x2F:
        guest_leaf(c, LEAF_TYPE_0F_14_56, G, item, 2u);
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
  TombaState tomba{c, G};

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
      guest_leaf(c, LEAF_DRY_TICK, G);
    }
  } else {
    // Water/sea mode. When water-state is 2 with a specific 800E7FEB (== 8) config, clamp
    // Tomba's Z to the water-region edge — matches the recomp's `< 0x1a05 → 0x1a04` snap.
    bool skipYSnap = false;
    if (waterState > 1) {
      if (waterState == 2 && c->mem_r8(0x800E7FEBu) == 8) {
        if (tomba.posZ() < 0x1A05) tomba.setPosZ(0x1A04);
      } else {
        skipYSnap = true;                                 // recomp: `goto LAB_8010E9D4;` skips the Y block
      }
    }
    if (!skipYSnap) {
      if ((tomba.groundedGate() & 1) == 0 &&
          (int32_t)waterLevel - (int32_t)tomba.physOffsetY() <= (int32_t)tomba.posY()) {
        tomba.setPosY((int16_t)(waterLevel - tomba.physOffsetY()));   // Y = waterLevel - G+0x62
        guest_leaf(c, LEAF_WATER_SPLASH, G);
      }
    }
  }

  // Area-exit trigger — fires only in water-mode 2 when Tomba is off-map.
  if (tomba.committing() != 0) return;
  if (c->mem_r8(0x800BF80Du) != 0) return;
  if (c->mem_r8(0x800BF839u) != 0) return;
  if (waterMode == 0) return;
  if (waterState != 2) return;
  if (tomba.posY() >= -0xE74) return;
  if (tomba.posZ() >= 0x1451) return;

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
// adding override-registry entries, deliberately left for the next frontier
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
        const int32_t cosv = trigOf(c).rcos(heading);
        const int32_t sinv = trigOf(c).rsin(heading);
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
  // Guest frame per abi_extract --contract 0x800205CC: single epilogue label -> RAII is safe.
  static constexpr GuestFrameSpill kSpills[] = {{17, 20}, {18, 24}, {31, 28}, {16, 16}};
  GuestFrame<32, 4> frameGuard(c, kSpills);
  TombaState tomba{c, G};
  TombaState other{c, item};

  if (c->mem_r8(item) == 5) {
    if ((tomba.growthFlags() & 0x200u) == 0 && tomba.frozenFlag() == 0) {
      guest_fn(c, LEAF_NILADIC_CUE, 0x80020620u);
    }
  } else if (tomba.growthFlags() & 0x8000u) {
    guest_fn(c, LEAF_GROWN_DELEGATE, 0x80020644u, G, item);
  } else {
    const int32_t v0 = (int32_t)guest_fn(c, LEAF_PROX_STEP, 0x80020658u, G, item, 0u);
    if (v0 >= 0) {
      if (c->mem_r8(item) == 1) {
        if (tomba.justTransitioned() == 1 && v0 < 2) {
          guest_fn(c, LEAF_ALT_TAG_SET, 0x8002069Cu, item, (uint32_t)-32766, 3u, 30u);
        } else if ((tomba.growthFlags() & 0x200u) == 0) {
          if ((v0 & 1) == 0) {
            if ((tomba.statusFlags() & 4u) == 0) {
              const int32_t heading = (int32_t)c->mem_r32(0x1F80009Cu);   // full 32-bit word
              const int32_t cosv = trigOf(c).rcos(heading);
              const int32_t sinv = trigOf(c).rsin(heading);
              const int32_t sum80 = (int32_t)tomba.boundXZ() + (int32_t)other.boundXZ();
              tomba.setPosX((int16_t)(other.posX() + (int16_t)((cosv * sum80) >> 12)));
              tomba.setPosZ((int16_t)(other.posZ() - (int16_t)((sinv * sum80) >> 12)));
            }
            tomba.setGroundContactFlag(1);
            // Heading arg is the full 32-bit OUT_HEADING_SPAD word (Ghidra: `iVar7 = (int)_DAT_1f80009c`
            // — a straight int cast, no 16-bit truncation, matching stepModeInteract's bVar6 fix).
            const int32_t cmp = Trig::angleCmp((int32_t)c->mem_r32(0x1F80009Cu), (int32_t)tomba.facing(), 1);
            tomba.setFlag95((uint8_t)(cmp + 2));
          } else if (v0 == 1 && (tomba.groundedGate() & 1u) == 0) {
            // G+0x32 = item[0x32] - (G[0x84] + item[0x84]) (all u16, unsigned per gen), THEN
            // growthYSnap()'s own reset+gated-Y-resnap tail — this branch's G+0x29/0x145/0x4A/
            // 0x50/0x148 reset (v0==1 here) plus the G+0x78/DAT_800BF816-gated const-140/70 snap
            // on G+0x32 are BYTE-IDENTICAL to guest FUN_80022C78 (growthYSnap), reused rather than
            // duplicated (generated/shard_0.c:1112 lines 1-19 == generated/shard_0.c:1466 lines 1-16).
            tomba.setPosY((int16_t)(other.posY() - (tomba.boundYUp() + other.boundYUp())));
            growthYSnap();
          }
        }
      } else if ((tomba.growthFlags() & 0x200u) == 0 && (tomba.groundedGate() & 1u) == 0) {
        c->mem_w8(item + 0x29u, 1);
      }
    }
  }
}

// FUN_800235A0 — postInteractWalk case 7.
uint8_t ActorTomba::type7Interact(uint32_t item) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  // Guest frame per abi_extract --contract 0x800235A0: single epilogue label -> RAII is safe.
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {17, 20}, {31, 24}};
  GuestFrame<32, 3> frameGuard(c, kSpills);
  TombaState tomba{c, G};

  const int32_t v0 = (int32_t)guest_fn(c, LEAF_PROX_STEP, 0x800235C0u, G, item, 1u);
  uint8_t result = 0;
  if (v0 >= 0) {
    // BUG FIX (RE cross-check against generated/shard_4.c:1267 gen_func_800235A0): the ground
    // truth does NOT reload r6 (a2) before this second call — a2 is a caller-saved scratch
    // register left as whatever LEAF_PROX_STEP's OWN body last wrote to it (a2 is never one of
    // that leaf's declared inputs/outputs, so its post-call value is an incidental side effect,
    // but it's a REAL, deterministic value since LEAF_PROX_STEP is the same substrate body on
    // both SBS cores). The original draft clobbered it with `item`, which is byte-exact ONLY if
    // it happens to equal what LEAF_PROX_STEP leaves behind — a latent SBS divergence. Leave r6
    // untouched here so it carries through exactly like the recompiled body (guest_fn would
    // clobber it — set r4/r5/r7 manually and dispatch without touching r6).
    const uint32_t flag = (tomba.transitionSlot() == 0x0Cu) ? 4u : 1u;
    c->r[4] = G; c->r[5] = item; c->r[7] = flag;
    guest_dispatch(c, 0x80023600u, LEAF_STEP_MODE_FLAG);
    result = 1;
  }
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
// mode0ActionGate() — guest FUN_8005A910(G). Tomba's mode-0 (default/walk) action-mode handler,
// reached from the mode-N dispatch table A (gen_func_80058918 case L_80058998). Picks between the
// normal sub-handler FUN_8005A970 (direct same-shard call) and the special sub-handler 0x80112B50
// (swim/water-interaction) based on the seaside water flag + Tomba's action-suppress bits:
//   A (FUN_8005A970): water mode ON (0x800BF816 != 0), OR G+0x17C == 0, OR (G+0x17E & 0x640) != 0
//   B (0x80112B50):   water OFF AND G+0x17C != 0 AND (G+0x17E & 0x640) == 0
// (G+0x17C = 380, an action-enable byte; G+0x17E = 382 = the growthFlags word — 0x640 = the
// 0x400|0x200|0x40 suppress bits.) Both sub-handlers stay substrate (guest_fn/rec_dispatch); this
// leaf owns only the branch. Faithful frame: sp-24, spill ra@+16 (mirrored via GuestFrame).
void ActorTomba::mode0ActionGate() {
  Core* c = core;
  const uint32_t G = G_ADDR;
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frame(c, kSpills);
  bool pathA = c->mem_r8(0x800BF816u) != 0           // water mode on
            || c->mem_r8(G + 0x17Cu) == 0            // action-enable byte clear
            || (c->mem_r16(G + 0x17Eu) & 0x640u) != 0;  // a suppress bit set
  if (pathA) guest_fn(c, 0x8005A970u, 0x8005A950u);  // normal handler (direct same-shard in gen)
  else       guest_fn(c, 0x80112B50u, 0x8005A960u);  // swim/water-interaction handler
}

// =================================================================================
// Four unowned per-frame Tomba leaves ported byte-faithfully 2026-07-17 (fleet band).
// Each body is the gen_func_<addr> guest-visible op stream VERBATIM (same stores, calls,
// order, and — for the two frame ports — the same sp descent + callee-save spills at the
// gen offsets, restored before return), wrapped as a Core*-taking method. Faithful by
// construction; port_check.py gates equivalence to the ORACLE. func_80085690/func_80074590
// are the substrate leaf thunks the gen bodies call (kept as calls, faithful).
// =================================================================================
void func_80085690(Core*);   // shard_disp.c — atan2-shaped angle leaf (called by proximityAngleWalk)
void func_80074590(Core*);   // shard_disp.c — spawn/record leaf (called by rampOffsetStep)

// ORACLE: gen_func_80053968
// proximityAngleWalk — FUN_80053968. Frame port (frame_size=56): spills s0..s7(r16..r23) at
// +16/+20/+24/+28/+32/+36/+40/+44 and ra at +48, mirrored verbatim below. Walks the aux list at
// mem[0x800A0140] (r20 base) with a live per-frame remaining-count byte at 0x800A0182, computing an
// angle (func_80085690) between each entry and Tomba (r17=a0) against a caller-supplied heading
// window (r23=a1); on a hit stamps the entry's +43 state byte (and, for the r19!=0 arm, +56 on the
// 0x800BF840 record). Returns 0/1 in r2.
void ActorTomba::proximityAngleWalk(Core* c) {
  c->r[29] = c->r[29] + (uint32_t)-56;
  c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
  c->r[17] = c->r[4] + c->r[0];
  c->mem_w32((c->r[29] + (uint32_t)48), c->r[31]);
  c->mem_w32((c->r[29] + (uint32_t)44), c->r[23]);
  c->mem_w32((c->r[29] + (uint32_t)40), c->r[22]);
  c->mem_w32((c->r[29] + (uint32_t)36), c->r[21]);
  c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
  c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
  c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)366));
  { int _t = (c->r[2] == c->r[0]); c->r[23] = c->r[5] + c->r[0]; if (_t) goto L_80053BC8; }
  c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)384));
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80053BCC; }
  c->r[3] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)330));
  c->r[2] = (uint32_t)(c->r[3] < (uint32_t)12);
  { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80053BC8; }
  c->r[2] = c->r[2] + (uint32_t)23492;
  c->r[3] = c->r[3] << 2;
  c->r[3] = c->r[3] + c->r[2];
  c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
  {  switch (c->r[2]) { case 0x800539E4u: goto L_800539E4; case 0x800539F0u: goto L_800539F0; case 0x80053BC8u: goto L_80053BC8; case 0x80053A00u: goto L_80053A00; case 0x80053A10u: goto L_80053A10; default: rec_dispatch(c, c->r[2]); return; } }
L_800539E4:;
  c->r[18] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)320));
  c->r[19] = c->r[0] + c->r[0]; goto L_80053A1C;
L_800539F0:;
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)320));
  c->r[19] = c->r[0] + c->r[0];
  c->r[18] = c->r[2] + (uint32_t)2048; goto L_80053A1C;
L_80053A00:;
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)320));
  c->r[19] = c->r[0] + (uint32_t)1;
  c->r[18] = c->r[2] + (uint32_t)-1024; goto L_80053A1C;
L_80053A10:;
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)320));
  c->r[19] = c->r[0] + (uint32_t)2;
  c->r[18] = c->r[2] + (uint32_t)1024;
L_80053A1C:;
  c->r[2] = (uint32_t)8064u << 16;
  c->r[20] = c->mem_r32((c->r[2] + (uint32_t)320));
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)326));
  c->r[3] = (uint32_t)8064u << 16;
  c->mem_w8((c->r[3] + (uint32_t)386), (uint8_t)c->r[2]);
  c->r[2] = c->r[2] & 255u;
  { int _t = (c->r[2] == c->r[0]); c->r[21] = c->r[0] + (uint32_t)2; if (_t) goto L_80053BC8; }
  c->r[2] = (uint32_t)32780u << 16;
  c->r[22] = c->r[2] + (uint32_t)-2040;
L_80053A48:;
  c->r[16] = c->mem_r32((c->r[20] + (uint32_t)0));
  c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)386));
  c->r[2] = c->r[2] + (uint32_t)-1;
  c->mem_w8((c->r[3] + (uint32_t)386), (uint8_t)c->r[2]);
  c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)12));
  c->r[2] = c->r[0] + (uint32_t)9;
  { int _t = (c->r[3] == c->r[2]); c->r[20] = c->r[20] + (uint32_t)4; if (_t) goto L_80053A84; }
  { int _t = (c->r[3] != c->r[21]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
  c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)2));
  c->r[2] = c->r[0] + (uint32_t)11;
  { int _t = (c->r[3] != c->r[2]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
L_80053A84:;
  c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)0));
  c->r[2] = c->r[2] & 1u;
  { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
  { int _t = (c->r[19] != c->r[0]);  if (_t) goto L_80053B18; }
  c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)43));
  { int _t = (c->r[2] != c->r[21]);  if (_t) goto L_80053BB8; }
  c->r[4] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)54));
  c->r[5] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
  c->r[4] = c->r[4] - c->r[2];
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)46));
  c->r[4] = c->r[0] - c->r[4];
  c->r[5] = c->r[5] - c->r[2];
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80053AE0u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80085690(c);
  c->r[2] = c->r[2] - c->r[18];
  c->r[2] = c->r[2] << 20;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80053AF8; }
  c->r[2] = c->r[0] - c->r[2];
L_80053AF8:;
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 4096);
  { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
  { int _t = (c->r[23] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80053BCC; }
  c->r[2] = c->r[0] + (uint32_t)3;
  c->mem_w8((c->r[16] + (uint32_t)43), (uint8_t)c->r[2]); goto L_80053BB0;
L_80053B18:;
  c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)43));
  c->r[2] = c->r[2] + (uint32_t)-1;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
  { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
  c->r[4] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)54));
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)54));
  c->r[5] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)46));
  c->r[4] = c->r[4] - c->r[2];
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)46));
  c->r[4] = c->r[0] - c->r[4];
  c->r[5] = c->r[5] - c->r[2];
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80053B60u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16); func_80085690(c);
  c->r[2] = c->r[2] - c->r[18];
  c->r[2] = c->r[2] << 20;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_80053B78; }
  c->r[2] = c->r[0] - c->r[2];
L_80053B78:;
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 4096);
  { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)8064u << 16; if (_t) goto L_80053BB8; }
  { int _t = (c->r[23] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80053BCC; }
  c->r[2] = c->r[0] + (uint32_t)3;
  c->mem_w8((c->r[16] + (uint32_t)43), (uint8_t)c->r[2]);
  c->r[2] = c->r[0] + (uint32_t)1;
  { int _t = (c->r[19] != c->r[2]);  if (_t) goto L_80053BA8; }
  c->r[2] = c->r[0] + (uint32_t)130; goto L_80053BAC;
L_80053BA8:;
  c->r[2] = c->r[0] + (uint32_t)131;
L_80053BAC:;
  c->mem_w8((c->r[22] + (uint32_t)56), (uint8_t)c->r[2]);
L_80053BB0:;
  c->r[2] = c->r[0] + (uint32_t)1; goto L_80053BCC;
L_80053BB8:;
  c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)386));
  { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80053A48; }
L_80053BC8:;
  c->r[2] = c->r[0] + c->r[0];
L_80053BCC:;
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)48));
  c->r[23] = c->mem_r32((c->r[29] + (uint32_t)44));
  c->r[22] = c->mem_r32((c->r[29] + (uint32_t)40));
  c->r[21] = c->mem_r32((c->r[29] + (uint32_t)36));
  c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
  c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
  c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
  c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
  c->r[29] = c->r[29] + (uint32_t)56; return;
}

// ORACLE: gen_func_80054790
// limbFrameLoad — FUN_80054790. Frameless leaf. On a state-byte (a0+71) change vs the value derived
// from state table 0x800A42F8, loads per-limb frame offsets from a base (mem[0x800A2FD4]) plus the
// per-state limb-offset triple (table 0x800A44AC) into the actor's five limb records
// (a0+196/208/220/204/216), each at record+64. r5(a1)=state index. No return value.
void ActorTomba::limbFrameLoad(Core* c) {
  c->r[2] = (uint32_t)32778u << 16;
  c->r[2] = c->r[2] + (uint32_t)17144;
  c->r[5] = c->r[5] + c->r[2];
  c->r[5] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
  c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)71));
  c->r[6] = c->r[5] & 255u;
  { int _t = (c->r[6] == c->r[2]); c->r[2] = (uint32_t)32783u << 16; if (_t) goto L_80054904; }
  c->r[3] = (uint32_t)32782u << 16;
  c->mem_w8((c->r[4] + (uint32_t)71), (uint8_t)c->r[5]);
  c->r[8] = c->mem_r32((c->r[2] + (uint32_t)-12268));
  c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)32766));
  c->r[2] = c->r[2] & 64u;
  { int _t = (c->r[2] != c->r[0]); c->r[7] = c->r[8] + (uint32_t)4; if (_t) goto L_80054844; }
  c->r[2] = (uint32_t)32778u << 16;
  c->r[5] = c->r[2] + (uint32_t)17580;
  c->r[2] = c->r[6] << 1;
  c->r[2] = c->r[2] + c->r[6];
  c->r[5] = c->r[2] + c->r[5];
  c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] + c->r[7];
  c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1));
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] + c->r[7];
  c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)208));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)2));
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] + c->r[7];
  c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)220));
  c->r[2] = c->r[8] + c->r[2]; goto L_80054900;
L_80054844:;
  c->r[2] = c->r[0] + (uint32_t)10;
  { int _t = (c->r[6] != c->r[2]); c->r[2] = (uint32_t)32778u << 16; if (_t) goto L_80054894; }
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)4));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[4] + (uint32_t)220));
  c->mem_w32((c->r[2] + (uint32_t)64), c->r[0]);
  c->r[2] = c->mem_r32((c->r[4] + (uint32_t)208));
  c->mem_w32((c->r[2] + (uint32_t)64), c->r[0]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)76));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)204));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)80));
   goto L_800548F8;
L_80054894:;
  c->r[5] = c->r[2] + (uint32_t)17580;
  c->r[2] = c->r[6] << 1;
  c->r[2] = c->r[2] + c->r[6];
  c->r[5] = c->r[2] + c->r[5];
  c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)0));
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] + c->r[7];
  c->r[2] = c->mem_r32((c->r[2] + (uint32_t)0));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)196));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)16));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)208));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)28));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)220));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)12));
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)204));
  c->r[2] = c->r[8] + c->r[2];
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
  c->r[2] = c->mem_r32((c->r[7] + (uint32_t)24));
L_800548F8:;
  c->r[3] = c->mem_r32((c->r[4] + (uint32_t)216));
  c->r[2] = c->r[8] + c->r[2];
L_80054900:;
  c->mem_w32((c->r[3] + (uint32_t)64), c->r[2]);
L_80054904:;
   return;
}

// ORACLE: gen_func_80060268
// invincibilityFlashStep — FUN_80060268. Frameless leaf. Reads the global damage/state word at
// mem[0x800A5354]; on the 0x10 bit sets the a0+361 "hit" bit 2; on the 0x80|0x20 bits runs the
// invincibility-flash cadence — compares a0+95/327/330, bumps the frame counter at 0x800A2238, and
// (re)arms a0+361 with the flicker mask (bit0 or bit1). Returns a small code in r2.
void ActorTomba::invincibilityFlashStep(Core* c) {
  c->r[2] = (uint32_t)32783u << 16;
  c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)-12460));
  c->r[2] = c->r[3] & 16u;
  { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 160u; if (_t) goto L_80060294; }
  c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)361));
  c->r[2] = c->r[2] | 2u;
  c->mem_w8((c->r[4] + (uint32_t)361), (uint8_t)c->r[2]); goto L_8006031C;
L_80060294:;
  { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8006031C; }
  c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)95));
  { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[2] & 1u; if (_t) goto L_800602BC; }
  c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
  { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + c->r[0]; if (_t) goto L_80060324; }
L_800602BC:;
  c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)330));
  c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)327));
  c->r[2] = c->r[2] & 1u;
  { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8006030C; }
  c->r[3] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)568));
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w16((c->r[3] + (uint32_t)568), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)6);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_800602F8; }
  c->mem_w8((c->r[4] + (uint32_t)361), (uint8_t)c->r[2]);
L_800602F8:;
  c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)361));
  c->r[2] = c->r[0] + (uint32_t)1;
L_80060300:;
  c->r[3] = c->r[3] | c->r[2];
  c->mem_w8((c->r[4] + (uint32_t)361), (uint8_t)c->r[3]); return;
L_8006030C:;
  c->mem_w16((c->r[2] + (uint32_t)568), (uint16_t)c->r[0]);
  c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)361));
  c->r[2] = c->r[0] + (uint32_t)2; goto L_80060300;
L_8006031C:;
  c->r[2] = c->r[0] + c->r[0]; return;
L_80060324:;
   return;
}

// ORACLE: gen_func_80063098
// rampOffsetStep — FUN_80063098. Frame port (frame_size=24): spills s0(r16)@+16 and ra@+20. Ramps
// the per-frame offset a0+340 up by 32 (clamped to 512), folds it into a0+66 (added, or subtracted
// when a0+348==a0+327) and a0+86; when the signed a0+66 falls below 1025 it fires the record leaf
// func_80074590(a0, 27, 0, 0), writes 136 to mem[0x800BF840], and bumps the a0+7 counter.
void ActorTomba::rampOffsetStep(Core* c) {
  c->r[29] = c->r[29] + (uint32_t)-24;
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
  c->r[16] = c->r[4] + c->r[0];
  c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
  c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)340));
  c->r[2] = c->r[2] + (uint32_t)32;
  c->mem_w16((c->r[16] + (uint32_t)340), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 513);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)512; if (_t) goto L_800630D0; }
  c->mem_w16((c->r[16] + (uint32_t)340), (uint16_t)c->r[2]);
L_800630D0:;
  c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)66));
  c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)340));
  c->r[5] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
  c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)348));
  c->r[2] = c->r[2] + c->r[3];
  { int _t = (c->r[4] != c->r[5]); c->mem_w16((c->r[16] + (uint32_t)66), (uint16_t)c->r[2]); if (_t) goto L_800630F8; }
  c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)86));
  c->r[2] = c->r[2] - c->r[3]; goto L_80063108;
L_800630F8:;
  c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)86));
  c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)340));
  c->r[2] = c->r[2] + c->r[3];
L_80063108:;
  c->mem_w16((c->r[16] + (uint32_t)86), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)66));
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 1025);
  { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[0] + (uint32_t)27; if (_t) goto L_80063148; }
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x8006312Cu;
  c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
  c->r[3] = (uint32_t)32780u << 16;
  c->r[2] = c->r[0] + (uint32_t)136;
  c->mem_w8((c->r[3] + (uint32_t)-1984), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[2]);
L_80063148:;
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
  c->r[29] = c->r[29] + (uint32_t)24; return;
}

// gov_ trampolines for the four leaves above (guest ABI: a0..a2 in r4..r6, return in r2 — the
// method bodies read/write c->r[] directly, so the wrapper just forwards c).
void ActorTomba::gov_proximityAngleWalk(Core* c)     { eng(c).actorTomba.proximityAngleWalk(c); }
void ActorTomba::gov_limbFrameLoad(Core* c)          { eng(c).actorTomba.limbFrameLoad(c); }
void ActorTomba::gov_invincibilityFlashStep(Core* c) { eng(c).actorTomba.invincibilityFlashStep(c); }
void ActorTomba::gov_rampOffsetStep(Core* c)         { eng(c).actorTomba.rampOffsetStep(c); }

// registerOverrides — wire the 4 postInteractWalk sub-handlers into the global override registry.
// Guest ABI: a0=G (implicit/unused — G is always ActorTomba::G_ADDR), a1=item, a2=mode where
// applicable; return via r[2] for the two that produce a v0. See actor_tomba.h for why a
// rec_dispatch-only registration (no shard_set_override setter) is correct here — no substrate
// shard calls these 4 addresses directly.
// =================================================================================
void ActorTomba::ov_stepModeInteract(Core* c) {
  const uint32_t item = c->r[5];
  const uint32_t mode = c->r[6];
  c->r[2] = eng(c).actorTomba.stepModeInteract(item, mode);
}
void ActorTomba::ov_type8Interact(Core* c) {
  const uint32_t item = c->r[5];
  eng(c).actorTomba.type8Interact(item);
}
void ActorTomba::ov_type7Interact(Core* c) {
  const uint32_t item = c->r[5];
  c->r[2] = eng(c).actorTomba.type7Interact(item);
}
void ActorTomba::ov_growthYSnap(Core* c) {
  eng(c).actorTomba.growthYSnap();
}

void ActorTomba::ov_frameTick(Core* c) {
  eng(c).actorTomba.frameTick();
}

// ov_turnBiasCompute/ov_outerTransitionGate/ov_outerTransitionCommit/ov_assetReady — guest ABI
// trampolines for the frameTick sub-callee cluster (§9 re-verified + wired 2026-07-10). Guest ABI
// per gen_func_<addr> (see the definitions above for the cited call sites): turnBiasCompute takes
// facing in a1 (a0=G is unused — the guest body never reads r4 in this leaf); outerTransitionGate/
// outerTransitionCommit always operate on Tomba's single fixed G block (a0=G is always G_ADDR, so
// the instance methods read G_ADDR directly rather than c->r[4]); outerTransitionCommit takes mode
// in a1; assetReady takes slot in a0 (NOT a1 — gen_func_80045580 uses r4<<3 directly).
void ActorTomba::ov_turnBiasCompute(Core* c) {
  turnBiasCompute(c, (int16_t)c->r[5]);
}
void ActorTomba::ov_outerTransitionGate(Core* c) {
  c->r[2] = eng(c).actorTomba.outerTransitionGate() ? 1u : 0u;
}
void ActorTomba::ov_outerTransitionCommit(Core* c) {
  eng(c).actorTomba.outerTransitionCommit((int32_t)c->r[5]);
}
void ActorTomba::ov_assetReady(Core* c) {
  c->r[2] = assetReady(c, (int32_t)c->r[4]) ? 1u : 0u;
}

// Dual-wire (§9 + fleet-workflow.md "most leaves are substrate-called"): all 4 addresses have
// direct substrate func_<addr>(c) callers (generated/shard_2.c, shard_4.c, shard_5.c — the
// still-substrate mode-N dispatch tables 0x80058918/0x80058F5C and enterOuterState0's own body)
// that do NOT go through rec_dispatch, so a rec_dispatch-only registration alone misses them. Per
// CLAUDE.md ("engine/game natives on the process-global g_override[]/g_ov_* tables MUST install via
// engine_set_override_* ... never the raw shard_set_override"), route through
// engine_set_override_main (runtime/recomp/override_registry.cpp) — the ONE oracle-gated choke
// point for this table (core B/psx_fallback always runs the passed gen_func_<addr>, core A always
// runs the native trampoline; no per-cluster hand-rolled psx_fallback gate to forget).
void ActorTomba::gov_turnBiasCompute(Core* c)      { ov_turnBiasCompute(c); }
void ActorTomba::gov_outerTransitionGate(Core* c)  { ov_outerTransitionGate(c); }
void ActorTomba::gov_outerTransitionCommit(Core* c){ ov_outerTransitionCommit(c); }
void ActorTomba::gov_assetReady(Core* c)           { ov_assetReady(c); }

// Wiring of the two 2026-07-10 wide-RE drafts (2026-07-16): both are dense literal transcriptions,
// so the verifier is MECHANICAL, not eyeball — wired through the same thunk and byte-gated per
// invocation with PSXPORT_MIRROR_VERIFY=0x800597AC,0x80058648 (native-vs-gen strict compare) plus
// the standard 2-leg SBS gate. matrixComposeAttached reads G from a0 per its banner; enterOuterState0
// takes (G=a0 implicit via eng(c).actorTomba's G, mode=a1).
void ActorTomba::gov_matrixComposeAttached(Core* c) { eng(c).actorTomba.matrixComposeAttached(); }
void ActorTomba::gov_enterOuterState0(Core* c)      { eng(c).actorTomba.enterOuterState0((int32_t)c->r[5]); }
void ActorTomba::gov_mode0ActionGate(Core* c)       { eng(c).actorTomba.mode0ActionGate(); }
void ActorTomba::gov_mode0WalkHandler(Core* c)      { eng(c).actorTomba.mode0WalkHandler(); }
void ActorTomba::gov_actionHandler8005ACC8(Core* c) { eng(c).actorTomba.actionHandler8005ACC8(); }
void ActorTomba::gov_actionHandler8005AEE4(Core* c) { eng(c).actorTomba.actionHandler8005AEE4(); }
void ActorTomba::gov_actionHandler8005F1B0(Core* c) { eng(c).actorTomba.actionHandler8005F1B0(); }
void ActorTomba::gov_actionHandler800588BC(Core* c) { eng(c).actorTomba.actionHandler800588BC(); }
void ActorTomba::gov_actionHandler800531DC(Core* c) { eng(c).actorTomba.actionHandler800531DC(); }
void ActorTomba::gov_actionHandler800660AC(Core* c) { eng(c).actorTomba.actionHandler800660AC(); }
void ActorTomba::gov_actionHandler8005EF48(Core* c) { eng(c).actorTomba.actionHandler8005EF48(); }

void ActorTomba::registerOverrides(Game* /*game*/) {
  using overrides::install;
  extern void gen_func_80020364(Core*);
  extern void gen_func_800205CC(Core*);
  extern void gen_func_800235A0(Core*);
  extern void gen_func_80022C78(Core*);
  extern void gen_func_8005950C(Core*);
  // rec_dispatch-only postInteractWalk sub-handlers + frameTick (no direct same-module caller ->
  // setter omitted). turnBiasCompute/outerTransitionGate/outerTransitionCommit/assetReady are
  // dual-wired via engine_set_override_main below (direct callers exist).
  install(0x80020364u, "ActorTomba::stepModeInteract", ov_stepModeInteract, gen_func_80020364);
  install(0x800205CCu, "ActorTomba::type8Interact",    ov_type8Interact,    gen_func_800205CC);
  install(0x800235A0u, "ActorTomba::type7Interact",    ov_type7Interact,    gen_func_800235A0);
  install(0x80022C78u, "ActorTomba::growthYSnap",      ov_growthYSnap,      gen_func_80022C78);
  install(0x8005950Cu, "ActorTomba::frameTick",        ov_frameTick,        gen_func_8005950C);

  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  extern void gen_func_80055C9C(Core*);
  extern void gen_func_80053E50(Core*);
  extern void gen_func_80053FDC(Core*);
  extern void gen_func_80045580(Core*);
  engine_set_override_main(0x80055C9Cu, gov_turnBiasCompute,       gen_func_80055C9C);
  engine_set_override_main(0x80053E50u, gov_outerTransitionGate,   gen_func_80053E50);
  engine_set_override_main(0x80053FDCu, gov_outerTransitionCommit, gen_func_80053FDC);
  engine_set_override_main(0x80045580u, gov_assetReady,            gen_func_80045580);
  extern void gen_func_80058648(Core*);
  engine_set_override_main(0x80058648u, gov_enterOuterState0, gen_func_80058648);
  // matrixComposeAttached (0x800597AC) — wired 2026-07-16 after the line-diff found and fixed FIVE
  // restructure defects in the 2026-07-10 draft (all delay-slot/branch-structure classes; the
  // original joint wiring was MV-rejected at 0x800F2758 because of them):
  //   1. missing `r23 = 0` delay-slot init at the attach-B gate — the loop's alt-matrix latch
  //      branched on the caller's stale s7 (the control-flow slip behind the record-init diff),
  //   2. missing `r4 = G+152` delay slot at the in-loop r22 branch + `r4 = r16` moved inside the
  //      G+356==5 test,
  //   3. the loop's THREE-way branch collapsed to two with the G-compose body carrying the
  //      alt-reuse path's ra constants (0x80059B44/50 vs the real 0x80059A78/84),
  //   4. missing `r5 = r20` delay slot at the chained-item (r16>=0) branch — matMul a1 got
  //      0x1F800000 instead of 0x1F800040 on that path,
  //   5. the G+325 arming condition INVERTED (gen arms attach-B iff G+325 == 0 && (G+326&3) != 0).
  // matrixComposeAttached (0x800597AC) — WIRED + verified 2026-07-16 after a SIX-defect repair of
  // the 2026-07-10 wide-RE draft (all delay-slot/branch-structure/inverted-gate classes; the
  // MIRROR_VERIFY-driven bisection found them one path at a time): #1 missing r23=0 delay-slot
  // init, #2 missing r4=G+152 / r4=r16 delay slots at the in-loop r22 and G+356==5 branches,
  // #3 the three-way per-item branch collapsed to two with conflated ra constants, #4 missing
  // r5=r20 delay slot on the chained-item path, #5 the G+325 arming gate inverted, #6 the
  // G+375&1 pan-patch gate inverted (r3=0 vs r3=G+334 — surfaced only at MV invocation #197, the
  // first call with the bit set, corrupting the whole G+152 SRT matrix). Plus the loop-exit v0
  // (gen leaves r2=(r19<count)=0). Verifier: 11713 MIRROR_VERIFY passes + 0 sbs-div over 6000
  // frames, both this and enterOuterState0 wired.
  extern void gen_func_800597AC(Core*);
  engine_set_override_main(0x800597ACu, gov_matrixComposeAttached, gen_func_800597AC);
  // mode0ActionGate (0x8005A910) — reached via the still-substrate mode-N table (gen_func_80058918
  // case 0); it calls func_8005A970 DIRECTLY, so it needs the shard_set_override setter too, not
  // just a rec_dispatch-only registration.
  extern void gen_func_8005A910(Core*);
  engine_set_override_main(0x8005A910u, gov_mode0ActionGate, gen_func_8005A910);
  // mode0WalkHandler (0x8005A970) — the mode-0 normal walk handler mode0ActionGate's path A calls
  // (verbatim PORT_GEN, port_check-equivalent, game/player/actor_tomba_actions.cpp).
  extern void gen_func_8005A970(Core*);
  engine_set_override_main(0x8005A970u, gov_mode0WalkHandler, gen_func_8005A970);
  extern void gen_func_8005ACC8(Core*);
  engine_set_override_main(0x8005ACC8u, gov_actionHandler8005ACC8, gen_func_8005ACC8);
  extern void gen_func_8005AEE4(Core*); engine_set_override_main(0x8005AEE4u, gov_actionHandler8005AEE4, gen_func_8005AEE4);
  extern void gen_func_8005F1B0(Core*); engine_set_override_main(0x8005F1B0u, gov_actionHandler8005F1B0, gen_func_8005F1B0);
  extern void gen_func_800588BC(Core*); engine_set_override_main(0x800588BCu, gov_actionHandler800588BC, gen_func_800588BC);
  extern void gen_func_800531DC(Core*); engine_set_override_main(0x800531DCu, gov_actionHandler800531DC, gen_func_800531DC);
  extern void gen_func_800660AC(Core*); engine_set_override_main(0x800660ACu, gov_actionHandler800660AC, gen_func_800660AC);
  extern void gen_func_8005EF48(Core*); engine_set_override_main(0x8005EF48u, gov_actionHandler8005EF48, gen_func_8005EF48);

  // Four unowned per-frame leaves (2026-07-17). All have DIRECT substrate func_<addr>(c) callers in
  // generated/shard_*.c (5/9/8/2 respectively — no rec_dispatch case), so they route through
  // engine_set_override_main (the oracle-gated main-module setter), not a rec_dispatch-only install.
  extern void gen_func_80053968(Core*);
  extern void gen_func_80054790(Core*);
  extern void gen_func_80060268(Core*);
  extern void gen_func_80063098(Core*);
  engine_set_override_main(0x80053968u, gov_proximityAngleWalk,     gen_func_80053968);
  engine_set_override_main(0x80054790u, gov_limbFrameLoad,          gen_func_80054790);
  engine_set_override_main(0x80060268u, gov_invincibilityFlashStep, gen_func_80060268);
  engine_set_override_main(0x80063098u, gov_rampOffsetStep,         gen_func_80063098);
}

// =================================================================================
// Per-frame G-block driver cascade (0x8005950C region) — RE'd + drafted 2026-07-08 wide-RE pass.
// UNWIRED: Engine::frameStartTick's `default: target = 0x8005950Cu` dispatch site still reaches
// the substrate func_8005950C directly (game/core/engine.cpp); wiring these in is a future
// frontier-tier step (an override-registry entry, then SBS-gate). All 6 functions below
// are faithful 1:1 ports from generated/shard_*.c ground truth (cited per-function), cross-checked
// against the Ghidra headless decompile in scratch/decomp/g_8005950C.c — which caught ONE real
// Ghidra decompiler error (see outerTransitionCommit's banner).
// =================================================================================

// turnBiasCompute — guest FUN_80055C9C. See actor_tomba.h for the full RE writeup. Frameless leaf
// (frame_size=0 per abi_extract) — no stack, purely fixed-address reads + a bias-pair write.
namespace {
constexpr uint32_t UI_MODE_BYTE       = 0x800E806Cu;   // ==5 selects the wide/menu delta formula
constexpr uint32_t VIEW_HEADING_SPAD  = 0x1F8000F2u;   // cached view heading, subtracted from facing
constexpr uint32_t CLOSE_MASK_WORD    = 0x800E805Au;   // bit 0x800 widens the "close" threshold
constexpr uint32_t TURN_BIAS_IN_SPAD  = 0x1F80016Cu;
constexpr uint32_t TURN_BIAS_OUT_SPAD = 0x1F80016Eu;
}  // namespace
void ActorTomba::turnBiasCompute(Core* c, int16_t facing) {
  bool closeIn;
  if (c->mem_r8(UI_MODE_BYTE) == 5) {
    // Wide/menu variant: delta from a fixed 0xC00(3072) reference minus the cached view heading
    // and facing.
    const uint32_t d = (3072u - c->mem_r16(VIEW_HEADING_SPAD) - (uint32_t)(int32_t)facing) & 4095u;
    closeIn = (int32_t)d < 2048;
  } else {
    // §9 re-verify 2026-07-10 (real bug found): gen_func_80055C9C's non-wide path does NOT use
    // the mode byte as a subtraction operand — the branch-delay slot of the `r3==5` compare
    // ALWAYS executes `r3 = 3072` (MIPS branch-delay-slot semantics: the delay-slot instruction
    // runs regardless of whether the branch is taken), so by the time the non-taken path reaches
    // `r3 = r3 - r5`, r3 already holds the literal 3072, not the mode byte. The mode byte is used
    // ONLY to select which formula runs (this branch vs the wide-variant one above), never as an
    // operand. The wide-RE draft mis-read this MIPS idiom and subtracted the mode byte instead —
    // confirmed via a live debug trace (`r3(mode-facing raw)=0x00000C00` i.e. 3072, not the mode
    // byte, immediately before the subtraction) which also fully explained the earlier apparent
    // "threshold swap" (a downstream symptom of this same root cause, not a separate bug — the
    // swapped-looking thresholds still needed the mask==0->1536/mask!=0->2560 fix independently
    // confirmed against gen above — that part of the earlier fix was correct and stays).
    uint32_t r3 = (3072u - (uint32_t)(int32_t)facing) & 4095u;
    const uint32_t r2m = c->mem_r16(VIEW_HEADING_SPAD) & 4095u;
    r3 = r3 - r2m;
    const uint32_t r4 = ((int16_t)r3 < 0) ? r3 : (r3 - 512u);
    const uint32_t d = r4 & 4095u;
    if (c->mem_r16(CLOSE_MASK_WORD) & 0x800u) closeIn = (int32_t)d < 2560;
    else                                      closeIn = (int32_t)d < 1536;
  }
  if (closeIn) { c->mem_w16(TURN_BIAS_IN_SPAD, 128); c->mem_w16(TURN_BIAS_OUT_SPAD, 32); }
  else         { c->mem_w16(TURN_BIAS_IN_SPAD, 32);  c->mem_w16(TURN_BIAS_OUT_SPAD, 128); }
}

// resetLoadGate — guest FUN_80042310. See actor_tomba.h for the full RE writeup. Guest frame
// (abi_extract --contract): 24 B, ra@sp+16 only.
void ActorTomba::resetLoadGate(Core* c) {
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frameGuard(c, kSpills);

  guest_leaf(c, 0x8001CF78u);                              // niladic substrate cue
  guest_fn(c, 0x80074590u, 0x80042320u, 0x7Fu, 0u, 0u);     // FUN_80074590(0x7F, 0, 0)
  const uint8_t areaMode = c->mem_r8(0x800BF870u);          // read before the unpause write below
  c->mem_w8(0x1F800137u, 0);                                // unpause
  guest_fn(c, 0x80074F24u, 0x80042320u, areaMode);          // FUN_80074F24(DAT_800BF870)
}

// assetReady — guest FUN_80045580. See actor_tomba.h for the full RE writeup. Guest frame: 24 B,
// ra@sp+16 only.
bool ActorTomba::assetReady(Core* c, int32_t slot) {
  static constexpr GuestFrameSpill kSpills[] = {{31, 16}};
  GuestFrame<24, 1> frameGuard(c, kSpills);

  constexpr uint32_t TABLE_8018A000 = 0x8018A000u;
  constexpr uint32_t DAT_800A3EC8   = 0x800A3EC8u;
  constexpr uint32_t SLOT_TABLE     = 0x800BE118u;   // &DAT_800be11c - 4 (slot*8 + 4 == +0xC)
  const uint32_t rec = c->mem_r32(SLOT_TABLE + (uint32_t)slot * 8u + 4u);
  const bool ready = guest_fn(c, 0x80044CD4u, 0x800455B0u, TABLE_8018A000, c->mem_r32(DAT_800A3EC8), rec) > 0;
  return ready;
}

// outerTransitionGate — guest FUN_80053E50(G). See actor_tomba.h for the full RE writeup. Guest
// frame (abi_extract --contract): 32 B; s0/s1/s2/ra spilled but never written by this body — pure
// passthrough preservation of the caller's values, which GuestFrame reproduces for free.
namespace {
constexpr uint32_t BUSY_LATCH_HI       = 0x800BF81Eu;
constexpr uint32_t BUSY_LATCH          = 0x800BF80Du;   // global "outer transition busy" latch
constexpr uint32_t LEAF_CUE_800521F4   = 0x800521F4u;    // transition-cue dispatch (4 call sites)
constexpr uint32_t LEAF_WALK_RESET     = 0x80053D90u;    // walk-state reset leaf
constexpr uint32_t LEAF_STOPMOTION     = 0x800312D4u;    // stop-motion task spawn (dest ptr, magnitude)
}  // namespace
bool ActorTomba::outerTransitionGate() {
  Core* c = core;
  const uint32_t G = G_ADDR;
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {17, 20}, {18, 24}, {31, 28}};
  GuestFrame<32, 4> frameGuard(c, kSpills);
  TombaState tomba{c, G};

  if (tomba.turnCurrent() > 0) return false;   // still mid-turn — nothing to do yet

  c->mem_w8(BUSY_LATCH_HI, 0);
  eng(c).gStateMutate(G, 0xB);

  if (tomba.transitionSlot() == 1) {
    if ((tomba.statusFlags() & 4u) == 0) {
      if ((tomba.latchFlags() & 0x80u) == 0) {
        guest_fn(c, LEAF_CUE_800521F4, 0x80053F14u, 0u, 0x81u, 0x81u, 0x0Fu);
      }
      tomba.setLatchFlags(0);
      tomba.setStopMotionAux(0);
      guest_fn(c, LEAF_WALK_RESET, 0x80053F24u, G);
      tomba.setStatusFlags(3);
      tomba.setTurnCurrent(0);
      tomba.setTurnTarget(0);
      tomba.setTurnSuppressGate(0);
      tomba.setOuterState(2);
      tomba.setLoadStep(1);
      tomba.setLoadSub(0);
      c->mem_w8(BUSY_LATCH, 1);
      guest_fn(c, LEAF_STOPMOTION, 0x80053FC0u, 6u, tomba.posAddr(), (uint32_t)-80);
      return true;
    }
    if ((tomba.latchFlags() & 0x80u) != 0) return true;
    guest_fn(c, LEAF_CUE_800521F4, 0x80053ED8u, 0u, 0x81u, 0x81u, 0x0Fu);
    tomba.setLatchFlags((uint8_t)(tomba.latchFlags() | 0x82u));
    return true;
  }

  if (c->mem_r8s(BUSY_LATCH) < 1) {
    guest_fn(c, LEAF_CUE_800521F4, 0x80053F74u, 0u, 0x81u, 0x81u, 0x0Fu);
    tomba.setLatchFlags(0);
    tomba.setStopMotionAux(0);
    guest_fn(c, LEAF_WALK_RESET, 0x80053F84u, G);
    tomba.setStatusFlags(3);
    tomba.setTurnCurrent(0);
    tomba.setTurnTarget(0);
    tomba.setTurnSuppressGate(0);
    tomba.setExtraClear(0);            // extra clear only on this path (verified vs shard)
    tomba.setOuterState(2);
    tomba.setLoadStep(1);
    tomba.setLoadSub(0);
    c->mem_w8(BUSY_LATCH, 1);
    guest_fn(c, LEAF_STOPMOTION, 0x80053FC0u, 6u, tomba.posAddr(), (uint32_t)-80);
  }
  // else (busy latch already set): nothing to do.
  return true;
}

// outerTransitionCommit — guest FUN_80053FDC(G, mode). See actor_tomba.h for the full RE writeup
// (incl. the Ghidra-vs-ground-truth correction in the decrement/settle tail below). Guest frame:
// 32 B; s0(r16)/s1(r17)/ra spilled but, like outerTransitionGate, never written by this body —
// pure passthrough (no s2 slot here, a smaller frame than outerTransitionGate's).
void ActorTomba::outerTransitionCommit(int32_t mode) {
  Core* c = core;
  const uint32_t G = G_ADDR;
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {17, 20}, {31, 24}};
  GuestFrame<32, 3> frameGuard(c, kSpills);
  TombaState tomba{c, G};

  // outerTransitionGate spills/restores ra to ITS OWN guest frame, so the caller's r31 at call
  // time is a live guest-RAM byte (spilled into that frame) and must match gen's jal-site constant.
  c->r[31] = 0x80053FF8u;
  if (outerTransitionGate()) return;

  if (tomba.turnCurrent() != tomba.turnTarget()) {
    // "reset to new target" — cue + gStateMutate(0xB) + conditional stop-motion clear.
    guest_fn(c, LEAF_CUE_800521F4, 0x80054024u, 0u, 0x81u, 0x81u, 0x0Fu);
    eng(c).gStateMutate(G, 0xB);
    c->mem_w8(BUSY_LATCH_HI, 0);
    if ((tomba.statusFlags() & 4u) == 0) {
      guest_fn(c, LEAF_WALK_RESET, 0x80054054u, G);
      tomba.setStopMotionAux(0);
    }
    if (mode != 1 && tomba.outerState() == 2) return;   // already committing — nothing to do
    // Commit a new target (shared by mode==1 and the mode!=1-but-not-yet-committing path).
    tomba.setSettleCounter(0x5A);
    tomba.setTurnTarget((uint16_t)tomba.turnCurrent());
    tomba.setLatchFlags((uint8_t)(tomba.latchFlags() | 0x82u));
    if ((tomba.statusFlags() & 0xCu) != 0) {
      guest_fn(c, 0x80074590u, 0x80054108u, 0x23u, 0u, 0u);
      guest_fn(c, LEAF_STOPMOTION, 0x80054118u, 6u, tomba.posAddr(), (uint32_t)-80);
      return;
    }
    tomba.setStatusFlags(3);
    tomba.setTurnSuppressGate(0);
    c->mem_w8(G + 0x145u, 0);
    tomba.setOuterState(2);
    tomba.setLoadStep(0);
    tomba.setLoadSub(0);
    return;
  }

  // Pending counter already at target — decrement-and-settle path.
  const int16_t remaining = tomba.settleCounter();
  if (remaining == 0) return;
  const int16_t newRemaining = (int16_t)(remaining - 1);
  tomba.setSettleCounter((uint16_t)newRemaining);
  if (newRemaining != 0) return;

  const uint8_t g0 = tomba.statusFlags();
  // §9 re-verify 2026-07-10: gen_func_80053FDC's gate compares g0 against the LITERAL 2, not 0
  // (`r3=g0; if(r3==2) goto rearm;` — a genuinely distinct constant, not a stand-in for "g0!=0").
  // The wide-RE draft had `g0 != 0`, which flips behavior at g0==0 (gen commits) and g0==2 (gen
  // re-arms) — fixed to match gen exactly.
  if (g0 != 2 && (g0 & 4u) == 0) {
    // "unobstructed" — commit to walk-state 1, clearing/masking the stop-motion latch bits.
    tomba.setStatusFlags(1);
    if ((tomba.latchFlags() & 0x50u) != 0) {
      tomba.setLatchFlags((uint8_t)(tomba.latchFlags() & 0x7Fu));
    } else {
      tomba.setLatchFlags(0);
    }
  } else {
    // g0==2 OR (g0&4)!=0 — re-arm the settle counter instead of committing.
    tomba.setSettleCounter(1);
  }
}

// frameTick — guest FUN_8005950C. See actor_tomba.h for the full RE writeup.
//
// Wired 2026-07-09: rec_dispatch-only registration (no shard_set_override setter — the only direct
// caller of func_8005950C is gen_func_80059D28 = the SUBSTRATE frameStartTickFaithful, which runs
// on core B only; core A reaches 0x8005950C via the native frameStartTickFaithful's rec_dispatch,
// which consults the override registry). Of the 5 drafted native sub-callees, 4 (turnBiasCompute /
// outerTransitionGate / outerTransitionCommit / assetReady) are now ALSO wired+verified as of
// 2026-07-10 (own overrides::install + engine_set_override_main registrations in registerOverrides()
// below — see docs/findings/animation.md "turnBiasCompute/outerTransitionGate/
// outerTransitionCommit/assetReady — §9 promotion" for the bugs that pass found and fixed:
// a MIPS branch-delay-slot misread in turnBiasCompute, a wrong gate constant in
// outerTransitionCommit, and 7 missing r31 mirrors). resetLoadGate is still UNWIRED (out of this
// pass's scope) and still dispatched to the SUBSTRATE below. The call sites below are UNCHANGED
// (still `rec_dispatch(c, addr)`, same r31-mirror-then-dispatch shape) — per CLAUDE.md, a wired
// leaf reached via `rec_dispatch` automatically routes through the new override-registry
// registration, so frameTick's own body needed no edits to pick up the 4 newly-wired callees.
//
// SBS-VERIFIED 2026-07-09: PSXPORT_SBS_MODE=full autonav, 0 sbs-div / 0 VIOLATION through f15600+.
// The first attempt diverged at f158 (A=0x1F80, B=0x0000 at an Animation::step r17 spill) — root
// caused to register-faithfulness: gen holds r17/r18 live across the case-1/4/7 callees (see the
// r17Reg/r18Reg writes in each case below), but the first draft used C++ locals alone, leaving
// stale caller values in those registers for the substrate callees (func_800597AC spills r18,
// func_80053FDC spills r17, func_80076D68 spills both) to spill. Mirroring gen's register
// assignments fixed it. GuestReg<17>/<18> below make that bug class impossible to reintroduce —
// they ARE c->r[17]/c->r[18], not shadow locals.
namespace {
constexpr uint32_t TURN_SUPPRESS_A         = 0x800E7E68u;   // "turn-suppress mask" pair, also
constexpr uint32_t TURN_SUPPRESS_B         = 0x800ECF54u;   // written by the enemy-engage tables
constexpr uint32_t TURN_SUPPRESS_CLEAR_GATE= 0x1F800230u;
constexpr uint32_t TURN_SUPPRESS_MASK_SPAD = 0x1F800174u;
constexpr uint32_t CUTSCENE_FLAG           = 0x800BF80Fu;
constexpr uint32_t FRAME_PAUSE_FLAG        = 0x1F800137u;
constexpr uint32_t CASE4_MASK_SRC_A        = 0x1F800166u;    // case-4's alt source for TURN_SUPPRESS_A
constexpr uint32_t CASE4_MASK_SRC_B        = 0x1F800190u;    // case-4's alt source for TURN_SUPPRESS_B
constexpr uint32_t TURN_SUPPRESS_ACTIVE_OUT= 0x1F800232u;
constexpr uint32_t LOAD_KICK_GATE_SPAD     = 0x1F80019Bu;
constexpr uint32_t LOAD_KICK_MODE_BYTE     = 0x800BF89Cu;
constexpr uint32_t ANIM_PTR_SPAD           = 0x1F800138u;
}  // namespace
void ActorTomba::frameTick() {
  Core* c = core;
  const uint32_t G = c->r[4];                // a0 (== G_ADDR from both callers; matches gen's r16=r4+r0)
  static constexpr GuestFrameSpill kSpills[] = {{16, 16}, {31, 28}, {18, 24}, {17, 20}};
  GuestFrame<32, 4> frameGuard(c, kSpills);
  GuestReg<16> gReg(c);
  gReg = G;
  TombaState tomba{c, G};

  const uint8_t outerState = tomba.outerState();
  if (outerState < 8) {
    switch (outerState) {
      case 0: {
        guest_fn(c, 0x80058648u, 0x80059560u, G, 0u);        // enterOuterState0 (substrate)
        break;
      }
      case 1: {
        const uint16_t savedA = c->mem_r16(TURN_SUPPRESS_A);
        const uint16_t savedB = c->mem_r16(TURN_SUPPRESS_B);
        // Register-faithfulness (gen 7648/7650): r18/r17 hold the saved pair live across the
        // case-1 callees. func_800597AC spills r18, func_80053FDC spills r17 — without these
        // assignments the substrate callees spill stale caller values on core A and diverge.
        GuestReg<18> r18Reg(c); r18Reg = savedA;
        GuestReg<17> r17Reg(c); r17Reg = savedB;
        if (c->mem_r8(TURN_SUPPRESS_CLEAR_GATE) != 0) {
          const uint16_t mask = c->mem_r16(TURN_SUPPRESS_MASK_SPAD);
          c->mem_w16(TURN_SUPPRESS_B, (uint16_t)(savedB & ~mask));
          c->mem_w16(TURN_SUPPRESS_A, (uint16_t)(savedA & ~mask));
        }
        // GT clears the turn-suppress pair unless BOTH CUTSCENE_FLAG==0 AND FRAME_PAUSE_FLAG==0
        // (the wide-RE draft inverted the CUTSCENE_FLAG condition — fixed).
        const bool skipClear = (c->mem_r8(CUTSCENE_FLAG) == 0) && (c->mem_r8(FRAME_PAUSE_FLAG) == 0);
        if (!skipClear) { c->mem_w16(TURN_SUPPRESS_A, 0); c->mem_w16(TURN_SUPPRESS_B, 0); }

        guest_fn(c, 0x80055C9Cu, 0x800595DCu, G, (uint32_t)(int32_t)tomba.facing());  // turnBiasCompute
        guest_fn(c, 0x80058918u, 0x800595E4u, G);            // mode-N dispatch table A (substrate)
        if (tomba.turnSuppressGate() == 0) c->mem_w8(TURN_SUPPRESS_ACTIVE_OUT, 1);
        guest_fn(c, 0x800597ACu, 0x80059604u, G);            // matrix-compose (substrate)
        guest_fn(c, 0x80053FDCu, 0x80059610u, G, 0u);        // outerTransitionCommit (mode=0)

        c->mem_w16(TURN_SUPPRESS_A, savedA);
        c->mem_w16(TURN_SUPPRESS_B, savedB);
        break;
      }
      case 2: {
        tomba.setCommitting(1);
        guest_fn(c, 0x80067CA4u, 0x80059628u, G);
        guest_fn(c, 0x800597ACu, 0x800596D8u, G);
        break;
      }
      case 3:
        break;   // unused jump-table slot (jump target = epilogue) — no-op
      case 4: {
        const uint16_t savedA = c->mem_r16(TURN_SUPPRESS_A);
        const uint16_t savedB = c->mem_r16(TURN_SUPPRESS_B);
        // Register-faithfulness (gen 7693/7695): same live-across-callees pair as case 1.
        GuestReg<18> r18Reg(c); r18Reg = savedA;
        GuestReg<17> r17Reg(c); r17Reg = savedB;
        tomba.setLatchFlags((uint8_t)(tomba.latchFlags() & 0x7Fu));
        if (c->mem_r8(FRAME_PAUSE_FLAG) != 0) {
          c->mem_w16(TURN_SUPPRESS_A, c->mem_r16(CASE4_MASK_SRC_A));
          c->mem_w16(TURN_SUPPRESS_B, c->mem_r16(CASE4_MASK_SRC_B));
        } else {
          c->mem_w16(TURN_SUPPRESS_A, 0);
          c->mem_w16(TURN_SUPPRESS_B, 0);
        }
        guest_fn(c, 0x80055C9Cu, 0x8005968Cu, G, (uint32_t)(int32_t)tomba.facing());  // turnBiasCompute
        guest_fn(c, 0x80058F5Cu, 0x80059694u, G);            // mode-N dispatch table B (substrate)
        guest_fn(c, 0x800597ACu, 0x8005969Cu, G);            // matrix-compose (substrate)
        guest_fn(c, 0x80053E50u, 0x800596A4u, G);            // outerTransitionGate (bare tick)

        c->mem_w16(TURN_SUPPRESS_A, savedA);
        c->mem_w16(TURN_SUPPRESS_B, savedB);
        break;
      }
      case 5: {
        guest_fn(c, 0x8018BD30u, 0x800596C0u, G);            // scripted leaf (overlay)
        guest_fn(c, 0x800597ACu, 0x800596D8u, G);
        break;
      }
      case 6: {
        guest_fn(c, 0x8018BE40u, 0x800596D0u, G);            // scripted leaf (overlay)
        guest_fn(c, 0x800597ACu, 0x800596D8u, G);
        break;
      }
      case 7: {
        const uint8_t sub = tomba.loadStep();
        // Register-faithfulness (gen 7736/7737): r17=sub, r18=1 held live across the sub-dispatch
        // and into the tail Animation::step call (which spills r17@+20, r18@+24). The sub==2 path
        // also stores r17/r18 verbatim into anim+0x4C/0x4E — but those are covered by the local
        // `sub` / literal 1 below; these register writes are purely for the callee spills.
        GuestReg<17> r17Reg(c); r17Reg = sub;
        GuestReg<18> r18Reg(c); r18Reg = 1u;
        bool advance = false;
        if (sub == 0) {
          guest_fn(c, 0x8001CF2Cu, 0x80059720u, G);          // engine tick cue (substrate)
          advance = true;
        } else if (sub == 1) {
          advance = guest_fn(c, 0x80045580u, 0x80059730u, 1u) != 0;  // assetReady
        } else if (sub == 2) {
          if (c->mem_r8(LOAD_KICK_GATE_SPAD) != 0) {
            c->mem_w8(LOAD_KICK_MODE_BYTE, 4);
            guest_fn(c, 0x80042310u, 0x80059768u, G);        // resetLoadGate (substrate)
            tomba.setOuterState(1);
            tomba.setLoadStep(0);
            tomba.setLoadSub(0);
            tomba.setLoadSub2(0);
            const uint32_t anim = c->mem_r32(ANIM_PTR_SPAD);
            c->mem_w16(anim + 0x4Cu, (uint16_t)sub);   // sub==2 — matches gen's r17 trace
            c->mem_w16(anim + 0x4Eu, 1);
          }
        }
        // sub > 2: no advance, straight to tail — matches gen (no case, no-op).
        if (advance) tomba.setLoadStep((uint8_t)(tomba.loadStep() + 1));
        guest_fn(c, 0x80076D68u, 0x80059794u, G);            // Animation::step (substrate)
        break;
      }
    }
  }
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
// ALSO reached via rec_dispatch here (not direct mathOf(c).*/eng(c).nodeXform.* calls) to keep
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
  // gen L_80059874: (G+375 & 1) == 0 → r3 = 0 ; else → r3 = G+334. The 2026-07-10 draft had this
  // INVERTED (defect #6 — same class as the G+325 gate) — invisible until the first call with the
  // bit SET (MV invocation #197), where it corrupted the whole G+152 SRT matrix (pan-patch feeds
  // func_800851F0 → matMul → G+152).
  if (c->r[2] == 0) {
    c->r[3] = 0;
  } else {
    c->r[3] = (uint32_t)c->mem_r16(c->r[18] + 334u);
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
  c->r[4] = c->r[16];                                     // gen delay slot at the ==5 test — executes on BOTH paths
  if (c->r[3] == 5u) {
    c->r[5] = c->mem_r32(c->r[18] + 16u);
    c->r[31] = 0x80059914u;
    c->r[5] = c->r[5] + 24u;
    rec_dispatch(c, 0x80084250u);                         // FUN_80084250 (see game/math/wide_re_gte_transform3.cpp — drafted, ORPHAN)
  }

  // Attach-B arming gate — 1:1 with gen L_80059914: armed iff G+325 == 0 AND (G+326 & 3) != 0
  // (the original draft had the G+325 condition INVERTED — defect #5 of the 2026-07-16 line-diff),
  // with gen's two delay slots preserved: r23 = 0 (the loop's alt-matrix latch init — defect #1)
  // and r4 = r17 at the second test.
  bool haveAttachB = false;
  c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 325u);
  c->r[23] = 0;                                           // gen delay slot — executes on BOTH paths
  if (c->r[2] == 0) {
    c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 326u);
    c->r[2] = c->r[2] & 3u;
    c->r[4] = c->r[17];                                   // gen delay slot — executes on BOTH paths
    if (c->r[2] != 0) {
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
  c->r[19] = 0;                                           // gen delay slot at the loop pre-gate — both paths
  if (c->r[2] != 0) {
    c->r[2] = 0x1F800000u;
    c->r[30] = c->r[2] + 32u;
    c->r[2] = 0x1F800000u;
    c->r[20] = c->r[2] + 64u;
    c->r[2] = 0x1F800000u;
    c->r[21] = c->r[2] + 96u;
    c->r[17] = c->r[18];
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

      // Three-way per-item compose branch — 1:1 with gen (2026-07-16 re-transcription; the prior
      // restructure conflated these bodies and their ra constants, see the wiring banner):
      //   r16 >= 0            -> L_80059B94: chain onto the PREVIOUS item (index r16) matrices
      //   r22 != 0, r23 == 0  -> L_80059AC8: build the alt attach-B matrix at 0x1F800060 once (r23 latch)
      //   r22 != 0, r23 != 0  -> L_80059B34: reuse the alt matrix (adds G+172/176/180)
      //   r22 == 0            -> fallthrough: compose against G's own matrix (ra 0x80059A78/84)
      {
        const bool chainPrev = ((int32_t)c->r[16] >= 0);
        c->r[5] = c->r[20];                               // gen delay slot — matMul a1 for EVERY path
        if (chainPrev) goto L_80059B94;
      }
      {
        const bool armed = (c->r[22] != 0);
        c->r[4] = c->r[18] + 152u;                        // gen delay slot — matMul a0 for the G-compose and alt-reuse paths
        if (armed) goto L_80059AC8;
      }
      c->r[6] = c->mem_r32(c->r[17] + 192u);
      c->r[5] = c->r[20];
      c->r[31] = 0x80059A78u;
      c->r[6] = c->r[6] + 24u;
      rec_dispatch(c, 0x80084110u);                       // matMul(G+152, 0x1F800040, item+24)
      c->r[4] = c->mem_r32(c->r[17] + 192u);
      c->r[31] = 0x80059A84u;
      c->r[5] = c->r[4] + 44u;
      rec_dispatch(c, 0x80084220u);                       // applyMatlv(item+44)
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
      goto L_80059C0C;
L_80059AC8:
      if (c->r[23] != 0) goto L_80059B34;
      c->r[4] = c->r[21];
      c->r[6] = c->mem_r32(c->r[17] + 192u);
      c->r[5] = c->r[20];
      c->r[31] = 0x80059AE4u;
      c->r[6] = c->r[6] + 24u;
      rec_dispatch(c, 0x80084110u);                       // matMul(0x1F800060, 0x1F800040, item+24)
      c->r[4] = c->mem_r32(c->r[17] + 192u);
      c->r[31] = 0x80059AF0u;
      c->r[5] = c->r[4] + 44u;
      rec_dispatch(c, 0x80084220u);                       // applyMatlv(item+44)
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
      c->r[23] = 1u;
      goto L_80059C04;
L_80059B34:
      c->r[6] = c->mem_r32(c->r[17] + 192u);
      c->r[5] = c->r[20];
      c->r[31] = 0x80059B44u;
      c->r[6] = c->r[6] + 24u;
      rec_dispatch(c, 0x80084110u);                       // matMul(G+152, 0x1F800040, item+24) — alt-reuse
      c->r[4] = c->mem_r32(c->r[17] + 192u);
      c->r[31] = 0x80059B50u;
      c->r[5] = c->r[4] + 44u;
      rec_dispatch(c, 0x80084220u);                       // applyMatlv(item+44)
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
      goto L_80059C0C;
L_80059B94:
      c->r[16] = c->r[16] << 2;
      c->r[16] = c->r[18] + c->r[16];
      c->r[4] = c->mem_r32(c->r[16] + 192u);
      c->r[6] = c->mem_r32(c->r[17] + 192u);
      c->r[4] = c->r[4] + 24u;
      c->r[31] = 0x80059BB0u;
      c->r[6] = c->r[6] + 24u;
      rec_dispatch(c, 0x80084110u);                       // matMul(prevItem+24, 0x1F800040, thisItem+24)
      c->r[4] = c->mem_r32(c->r[17] + 192u);
      c->r[31] = 0x80059BBCu;
      c->r[5] = c->r[4] + 44u;
      rec_dispatch(c, 0x80084220u);                       // applyMatlv(item+44)
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
L_80059C04:
      c->r[2] = c->r[2] + c->r[3];
L_80059C0C:
      c->mem_w32(c->r[4] + 52u, c->r[2]);

      c->r[2] = (uint32_t)c->mem_r8(c->r[18] + 9u);
      c->r[19] = c->r[19] + 1u;
      c->r[2] = (uint32_t)((int32_t)c->r[19] < (int32_t)c->r[2]);   // gen: r2 = (r19 < count) — LIVE at exit
      if (c->r[2] == 0) break;
      c->r[17] = c->r[17] + 4u;
    }
  }
  // gen exits 800597AC with r2 = the final (r19 < count) comparison = 0 (loop-run path) OR the
  // G+9==0 gate's 0 (loop-skipped path). enterOuterState0 returns this as its own v0, so a stale
  // r2 here (the old draft left r2 = count) surfaced as eos0 v0=0x11 vs 0 under MIRROR_VERIFY.
  c->r[2] = 0;

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
// WIRED 2026-07-16 (registerOverrides; MIRROR_VERIFY pass-OK + 2-leg 0-diff — see parity map).
//
// Tomba's OUTER-STATE-0 (INIT) driver. Calls GraphicsBind::recordArrayInit(G, 17, *(0x800ED014),
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
  rec_dispatch(c, 0x800519E0u);          // GraphicsBind::recordArrayInit(G,17,*(0x800ED014),0x800A3FA8) — a2 = [r17+188], r17=0x800ECF58
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

// ----------------------------------------------------------------------------
// 2026-07-10 wide-RE pass — mode-N dispatch table A/B case-target cluster
// 0x80060064-0x80065374. See actor_tomba.h banner for the shared caveats (G+0x6 sub-FSM,
// field semantics past the state byte NOT derived). Literal register-level transcriptions.
// ----------------------------------------------------------------------------

void ActorTomba::caseAreaEntryHook_80065374() {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24;
  c->r[2] = (uint32_t)32780u << 16;
  c->r[3] = (uint32_t)c->mem_r8(c->r[2] + (uint32_t)-1936);   // DAT_800BF870 (current area id)
  c->r[2] = c->r[0] + 5u;
  c->mem_w32(c->r[29] + 16, c->r[31]);
  if (c->r[3] == c->r[2]) goto L_800653CC;                    // area==5
  c->r[2] = (uint32_t)((int32_t)c->r[3] < 6);
  if (c->r[2] == c->r[0]) goto L_800653A8;                    // area>=6
  if (c->r[3] == c->r[0]) goto L_800653BC;                    // area==0
  goto L_800653E4;                                            // area in 1..4: no-op
L_800653A8:
  c->r[2] = c->r[0] + 6u;
  if (c->r[3] == c->r[2]) goto L_800653DC;                    // area==6
  goto L_800653E4;                                            // area>6: no-op
L_800653BC:
  c->r[31] = 0x800653C4u;
  rec_dispatch(c, 0x8010AECCu);                                // still-substrate area-0 hook — un-triaged
  goto L_800653E4;
L_800653CC:
  c->r[31] = 0x800653D4u;
  rec_dispatch(c, 0x80110CB8u);                                // still-substrate area-5 hook — un-triaged
  goto L_800653E4;
L_800653DC:
  c->r[31] = 0x800653E4u;
  rec_dispatch(c, 0x80113E3Cu);                                // still-substrate area-6 hook — un-triaged
L_800653E4:
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::caseArea0EntryHook_800653F4(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G + c->r[0];
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);                // G+0x6 (sub-state)
  c->r[2] = c->r[0] + 1u;
  if (c->r[3] == c->r[0]) goto L_80065424;                     // state 0: init
  {
    const uint32_t r2_state1 = (uint32_t)32780u << 16;
    if (c->r[3] == c->r[2]) { c->r[2] = r2_state1; goto L_80065450; }  // state 1: skip init
  }
  goto L_80065468;                                             // state >=2: no-op
L_80065424:
  c->r[31] = 0x8006542Cu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                // func_80054198(G) — SceneTransition-style resetSwap, still-substrate
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 64u;
  c->r[31] = 0x8006543Cu;
  c->r[6] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80054D14u);                                // func_80054D14(G,64,0) — still-substrate, un-triaged
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                  // G+0x7 = 0
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                  // G+0x6++ (0 -> 1)
  c->r[2] = (uint32_t)32780u << 16;
L_80065450:
  c->r[2] = (uint32_t)c->mem_r8(c->r[2] + (uint32_t)-1936);    // DAT_800BF870 (current area id)
  if (c->r[2] != c->r[0]) goto L_80065468;                     // area != 0: skip
  c->r[31] = 0x80065468u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x8010C780u);                                // still-substrate area-0 hook — un-triaged
L_80065468:
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::caseModeFsm_800620D0(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G + c->r[0];
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->r[31] = 0x800620E8u;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  rec_dispatch(c, 0x80076D68u);                                // still-substrate leaf FUN_80076D68(G) — un-triaged (ActorTomba::frameTick uses the same addr as a leaf; here reached as a plain substrate call)
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);                 // G+0x6 (sub-state)
  c->r[17] = c->r[0] + 1u;
  if (c->r[3] == c->r[17]) goto L_80062178;                     // state 1
  {
    const bool lt2 = ((int32_t)c->r[3] < 2);
    if (!lt2) goto L_80062110;                                  // state >=2
  }
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[3] == c->r[0]) goto L_8006212C;                      // state 0
  goto L_80062278;                                               // unreachable in practice (state<2, !=0, !=1 impossible for uint8) — kept for fidelity
L_80062110:
  {
    const uint32_t two = c->r[0] + 2u;
    if (c->r[3] == two) goto L_800621E4;                        // state 2
  }
  {
    const uint32_t three = c->r[0] + 3u;
    if (c->r[3] == three) goto L_80062238;                      // state 3
  }
  goto L_80062278;                                                // state >3: no-op
L_8006212C:
  c->r[31] = 0x80062134u;
  c->mem_w8(c->r[16] + 326u, (uint8_t)c->r[0]);
  rec_dispatch(c, 0x80053D90u);                                  // still-substrate leaf — un-triaged
  c->r[31] = 0x8006213Cu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                  // func_80054198(G)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 225u;
  c->r[31] = 0x8006214Cu;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                  // func_80054D14(G,225,4)
  c->r[4] = c->r[0] + 57u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x8006215Cu;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                  // SFX/cue trigger(57,0,0)
  c->r[4] = c->r[16] + c->r[0];
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[3] = c->r[0] + 30u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[3]);                 // G+0x40 (timer) = 30
  c->r[2] = c->r[2] + 1u;
  c->r[31] = 0x80062178u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                    // G+0x6++
  rec_dispatch(c, 0x800551C4u);                                  // still-substrate leaf — un-triaged
L_80062178:
  c->r[31] = 0x80062180u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                  // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 50u);
  c->r[4] = c->r[16] + c->r[0];
  c->r[2] = c->r[2] + 8u;
  c->r[31] = 0x80062194u;
  c->mem_w16(c->r[16] + 50u, (uint16_t)c->r[2]);                 // G+0x32 += 8
  rec_dispatch(c, 0x8005444Cu);                                  // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x800621A0u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80056C00u);                                  // still-substrate leaf(G,1) — un-triaged
  {
    const int32_t timer_s = (int32_t)(int16_t)c->mem_r16(c->r[16] + 64u);
    const uint32_t timer_u = (uint32_t)c->mem_r16(c->r[16] + 64u);
    c->r[2] = (uint32_t)timer_s;
    c->r[3] = timer_u;
    if (c->r[2] != c->r[0]) { c->r[2] = c->r[3] - 1u; goto L_800621DC; }  // timer != 0: decrement, store, done
  }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 41u);                 // G+0x29
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[2] == c->r[0]) goto L_800621CC;                       // G+0x29 == 0
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[0]);                    // G+0x5 = 0 (re-idle)
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[0]);                    // G+0x6 = 0
  goto L_80062278;
L_800621CC:
  c->r[31] = 0x800621D4u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80056D44u);                                  // still-substrate leaf(G,0) — un-triaged
  goto L_80062278;
L_800621DC:
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                 // G+0x40 -= 1
  goto L_80062278;
L_800621E4:
  c->r[4] = c->r[16] + c->r[0];
  {
    const uint32_t base = (uint32_t)32780u << 16;
    c->mem_w8(base + (uint32_t)-2034, (uint8_t)c->r[0]);         // clear byte @ 0x800BF80E
  }
  c->r[31] = 0x800621F8u;
  c->mem_w8(c->r[16] + 326u, (uint8_t)c->r[0]);
  rec_dispatch(c, 0x80053D90u);                                  // still-substrate leaf — un-triaged
  c->r[31] = 0x80062200u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                  // func_80054198(G)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 225u;
  c->r[31] = 0x80062210u;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                  // func_80054D14(G,225,4)
  c->r[4] = c->r[0] + 57u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x80062220u;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                  // SFX/cue trigger(57,0,0)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[3] = c->r[0] + 30u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[3]);                 // G+0x40 (timer) = 30
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                    // G+0x6++ (2 -> 3)
  goto L_80062278;
L_80062238:
  c->r[31] = 0x80062240u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800551C4u);                                  // still-substrate leaf — un-triaged
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 64u);
  c->r[2] = c->r[2] - 1u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                 // G+0x40 -= 1
  {
    const uint32_t shifted = c->r[2] << 16;
    if (shifted != c->r[0]) goto L_80062278;                     // timer != 0: done
  }
  {
    const uint32_t base = (uint32_t)32780u << 16;
    c->mem_w8(base + (uint32_t)-2034, (uint8_t)c->r[17]);        // byte @ 0x800BF80E = 1 (r17 still holds 1 from entry)
  }
  c->r[2] = c->r[0] + 4u;
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[2]);                    // G+0x4 = 4
  c->r[2] = c->r[0] + 32u;
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);                    // G+0x5 = 32
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[0]);                    // G+0x6 = 0
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                    // G+0x7 = 0
L_80062278:
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::caseModeFsm_80061A7C(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->r[31] = 0x80061A90u;
  c->r[16] = G + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                  // still-substrate leaf(G) — un-triaged
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);                  // G+0x6 (sub-state)
  c->r[5] = c->r[0] + 1u;
  if (c->r[3] == c->r[5]) goto L_80061B04;                       // state 1
  {
    const bool lt2 = ((int32_t)c->r[3] < 2);
    if (!lt2) goto L_80061AB8;                                    // state >=2
  }
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[3] == c->r[0]) goto L_80061AD4;                        // state 0
  goto L_80061C54;                                                 // unreachable (see caseModeFsm_800620D0)
L_80061AB8:
  {
    const uint32_t two = c->r[0] + 2u;
    if (c->r[3] == two) goto L_80061B6C;                          // state 2
  }
  {
    const uint32_t three = c->r[0] + 3u;
    if (c->r[3] == three) goto L_80061BAC;                        // state 3
  }
  goto L_80061C54;                                                  // state >3: no-op
L_80061AD4:
  {
    const uint32_t base = (uint32_t)32780u << 16;
    c->mem_w8(base + (uint32_t)-2039, (uint8_t)c->r[5]);           // byte @ 0x800BF7D9 = 1
  }
  c->r[2] = c->r[0] + 7u;
  c->r[31] = 0x80061AE8u;
  c->mem_w8(c->r[16] + 0u, (uint8_t)c->r[2]);                      // G+0x0 = 7
  rec_dispatch(c, 0x80054198u);                                    // func_80054198(G)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 114u;
  c->r[31] = 0x80061AF8u;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                    // func_80054D14(G,114,4)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[3] = c->r[0] + 25u;
  goto L_80061B9C;
L_80061B04:
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 64u);
  c->r[2] = c->r[2] - 1u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                   // G+0x40 (timer) -= 1
  {
    const uint32_t shifted = c->r[2] << 16;
    c->r[4] = c->r[0] + 6u;
    if (shifted != c->r[0]) goto L_80061C54;                       // timer != 0: done
  }
  c->r[31] = 0x80061B28u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x8002F514u);                                    // still-substrate leaf(6,0) — un-triaged
  c->r[4] = c->r[0] + 55u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x80061B38u;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                    // SFX/cue trigger(55,0,0)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[3] = c->r[0] + 10u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[3]);                   // G+0x40 (timer) = 10
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                      // G+0x6++ (1 -> 2)
  {
    const uint32_t idx = ((uint32_t)c->mem_r16(c->r[16] + 382u)) & 15u;   // G+0x17E & 0xF
    const uint32_t tbl = ((uint32_t)32778u << 16) + 17748u;
    c->r[2] = (uint32_t)c->mem_r8(tbl + idx - 4u);                  // .rodata lookup table
  }
  c->mem_w16(c->r[16] + 66u, (uint16_t)c->r[2]);                    // G+0x42 = table[idx]
  goto L_80061C54;
L_80061B6C:
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 64u);
  c->r[2] = c->r[2] - 1u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                    // G+0x40 (timer) -= 1
  {
    const uint32_t shifted = c->r[2] << 16;
    if (shifted != c->r[0]) goto L_80061C54;                        // timer != 0: done
  }
  c->r[31] = 0x80061B90u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800248D0u);                                     // still-substrate leaf(G) — un-triaged, result in r2
  if (c->r[2] == c->r[0]) { c->r[3] = c->r[0] + 70u; goto L_80061C18; }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
L_80061B9C:
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[3]);                    // G+0x40 (timer) = r3 (25 or the fallthrough value)
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                       // G+0x6++
  goto L_80061C54;
L_80061BAC:
  {
    const uint32_t bit0 = ((uint32_t)c->mem_r16(c->r[16] + 66u)) & 1u;  // G+0x42 bit 0
    c->r[2] = bit0;
  }
  if (c->r[2] == c->r[0]) goto L_80061BE4;
  {
    const uint32_t base = ((uint32_t)32780u << 16) + (uint32_t)-1936; // 0x800BF870
    c->r[2] = (uint32_t)c->mem_r8(base + 123u);                      // byte @ 0x800BF8EB
    if (c->r[2] != c->r[0]) goto L_80061BE4;
    c->r[2] = (uint32_t)c->mem_r8(base + 14u);                       // byte @ 0x800BF87E
    c->r[2] = c->r[2] - 1u;
    c->mem_w8(base + 14u, (uint8_t)c->r[2]);                         // byte @ 0x800BF87E -= 1
  }
L_80061BE4:
  c->r[2] = (uint32_t)(int32_t)(int16_t)c->mem_r16(c->r[16] + 66u);
  c->r[3] = (uint32_t)c->mem_r16(c->r[16] + 66u);
  c->r[2] = c->r[3] - 1u;                                           // delay-slot value, computed always
  if ((int32_t)(int16_t)c->mem_r16(c->r[16] + 66u) == 0) goto L_80061BF8;   // v66==0: skip write-back
  c->mem_w16(c->r[16] + 66u, (uint16_t)c->r[2]);                    // v66 -= 1
L_80061BF8:
  c->r[2] = (uint32_t)(int32_t)(int16_t)c->mem_r16(c->r[16] + 64u);
  c->r[3] = (uint32_t)c->mem_r16(c->r[16] + 64u);
  c->r[2] = c->r[3] - 1u;                                           // delay-slot value, computed always
  if ((int32_t)(int16_t)c->mem_r16(c->r[16] + 64u) != 0) goto L_80061C50;   // v64!=0: store decrement, done
  c->r[2] = (uint32_t)(int32_t)(int16_t)c->mem_r16(c->r[16] + 66u);
  if ((int32_t)(int16_t)c->mem_r16(c->r[16] + 66u) != 0) goto L_80061C54;   // v66(current) != 0: no-op
L_80061C18:
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 2u;
  c->r[6] = c->r[0] + 5u;
  c->r[2] = c->r[0] + 3u;
  c->mem_w8(c->r[4] + 0u, (uint8_t)c->r[2]);                        // G+0x0 = 3
  c->r[2] = c->r[0] + 20u;
  c->mem_w16(c->r[4] + 370u, (uint16_t)c->r[2]);                    // G+0x172 = 20
  c->mem_w8(c->r[4] + 5u, (uint8_t)c->r[0]);                        // G+0x5 = 0
  c->mem_w8(c->r[4] + 6u, (uint8_t)c->r[0]);                        // G+0x6 = 0
  c->r[31] = 0x80061C44u;
  c->mem_w8(c->r[4] + 7u, (uint8_t)c->r[0]);                        // G+0x7 = 0
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,2,5)
  {
    const uint32_t base = (uint32_t)32780u << 16;
    c->mem_w8(base + (uint32_t)-2039, (uint8_t)c->r[0]);            // byte @ 0x800BF7D9 = 0
  }
  goto L_80061C54;
L_80061C50:
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);
L_80061C54:
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::caseModeFsm_80060064(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G + c->r[0];
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);                  // G+0x6 (outer sub-state)
  c->r[2] = c->r[0] + 1u;
  if (c->r[3] == c->r[0]) goto L_80060098;                       // G+0x6 == 0
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[3] == c->r[2]) goto L_800601D8;                       // G+0x6 == 1
  goto L_8006024C;                                                // G+0x6 >=2: skip straight to tail
L_80060098:
  c->r[17] = (uint32_t)c->mem_r8(c->r[16] + 7u);                 // G+0x7 (inner sub-state)
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[17] == c->r[0]) goto L_800600B8;                      // G+0x7 == 0
  if (c->r[17] == c->r[2]) goto L_80060100;                      // G+0x7 == 1
  goto L_8006024C;                                                 // G+0x7 >=2: skip straight to tail
L_800600B8:
  c->mem_w8(c->r[16] + 325u, (uint8_t)c->r[0]);                   // G+0x145 = 0
  c->r[31] = 0x800600C4u;
  c->mem_w8(c->r[16] + 326u, (uint8_t)c->r[0]);                   // G+0x146 = 0
  rec_dispatch(c, 0x80053D90u);                                   // still-substrate leaf — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 1u;
  c->r[31] = 0x800600D4u;
  c->mem_w16(c->r[16] + 88u, 0);                                  // G+0x58 = 0
  rec_dispatch(c, 0x80055E28u);                                   // still-substrate leaf(G,1) — un-triaged
  c->r[31] = 0x800600DCu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                   // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 22u;
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 7u);
  c->r[6] = c->r[0] + 3u;
  c->r[2] = c->r[2] + 1u;
  c->r[31] = 0x800600F8u;
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[2]);                     // G+0x7++
  rec_dispatch(c, 0x80054D14u);                                   // func_80054D14(G,22,3)
  goto L_8006024C;
L_80060100:
  c->r[31] = 0x80060108u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80055E28u);                                   // still-substrate leaf(G,1) — un-triaged
  c->r[31] = 0x80060110u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                   // still-substrate leaf(G) — un-triaged
  c->r[31] = 0x80060118u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                   // still-substrate leaf(G) — un-triaged, result in r2
  c->r[3] = c->r[2] + c->r[0];
  c->r[4] = c->r[0] + 15u;
  if (c->r[3] != c->r[17]) goto L_8006024C;                       // result != G+0x7's pre-fetch value: bail to tail
  c->r[5] = c->r[0] + c->r[0];
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[6] = c->r[5] + c->r[0];
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                     // G+0x7 = 0
  c->mem_w8(c->r[16] + 325u, (uint8_t)c->r[3]);                   // G+0x145 = matched value
  c->mem_w16(c->r[16] + 80u, 0);                                  // G+0x50 = 0
  c->r[2] = c->r[2] + 1u;
  c->r[31] = 0x80060148u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                     // G+0x6++
  rec_dispatch(c, 0x80074590u);                                   // still-substrate leaf: r4 is LEFTOVER=G (not reloaded since the func_80076D68 call above), r5=0, r6=0 — matches gen exactly; not necessarily the same (cueId,0,0) signature as the other func_80074590 call sites in this file
  c->r[4] = c->r[0] + 37u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x80060158u;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                   // SFX/cue trigger(37,0,0)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 18u;
  c->r[31] = 0x80060168u;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                   // func_80054D14(G,18,4)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[16] + 44u;
  c->r[31] = 0x80060178u;
  c->r[6] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x800538E0u);                                   // still-substrate leaf(G,G+44,0) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80060184u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80055F48u);                                   // still-substrate leaf(G,0) — un-triaged
  c->r[31] = 0x8006018Cu;
  rec_dispatch(c, 0x80055844u);                                   // ActorMeleeEngage::doIt's own leaf — still substrate here, result in r2
  if (c->r[2] == c->r[0]) { c->r[2] = c->r[0] + (uint32_t)-10240; goto L_800601D0; }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 357u);                 // G+0x165
  if (c->r[2] == c->r[0]) goto L_800601B4;
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 74u);                 // G+0x4A
  c->r[2] = c->r[2] - 1408u;
  c->mem_w16(c->r[16] + 74u, (uint16_t)c->r[2]);                  // G+0x4A -= 1408
L_800601B4:
  c->r[3] = (uint32_t)c->mem_r16(c->r[16] + 74u);                 // G+0x4A
  c->r[2] = c->r[3] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 18);                   // sign-extended G+0x4A / 4
  c->r[3] = c->r[3] + c->r[2];                                    // G+0x4A + G+0x4A/4
  c->mem_w16(c->r[16] + 74u, (uint16_t)c->r[3]);
  goto L_8006024C;
L_800601D0:
  c->mem_w16(c->r[16] + 74u, (uint16_t)c->r[2]);                  // G+0x4A = -10240 (0xD800)
  goto L_8006024C;
L_800601D8:
  c->r[31] = 0x800601E0u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80055E28u);                                   // still-substrate leaf(G,1) — un-triaged
  c->r[5] = (uint32_t)c->mem_r8(c->r[16] + 330u);                 // G+0x14A
  c->r[31] = 0x800601ECu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055FBCu);                                   // still-substrate leaf(G, byte@G+0x14A) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x800601F8u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80056B48u);                                   // still-substrate leaf(G,1) — un-triaged (same addr as ActorTomba::velocityIntegrate's guest FUN)
  c->r[31] = 0x80060200u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                   // still-substrate leaf(G) — un-triaged
  c->r[31] = 0x80060208u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                   // still-substrate leaf(G) — un-triaged
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 325u);                 // G+0x145
  c->r[2] = c->r[0] + 2u;
  if (c->r[3] != c->r[2]) goto L_80060230;                        // G+0x145 != 2
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80060224u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x800574E0u);                                   // still-substrate leaf(G,0) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + c->r[0];
  goto L_80060244;
L_80060230:
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x8006023Cu;
  c->r[5] = c->r[0] + 17u;
  rec_dispatch(c, 0x800574E0u);                                   // still-substrate leaf(G,17) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 1u;
L_80060244:
  c->r[31] = 0x8006024Cu;
  rec_dispatch(c, 0x80057C08u);                                   // still-substrate leaf(G, 0-or-1) — un-triaged
L_8006024C:
  c->r[31] = 0x80060254u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800551C4u);                                   // still-substrate leaf(G) — un-triaged, shared tail every branch funnels through
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

// ----------------------------------------------------------------------------
// 2026-07-10 wide-RE pass #2 — see actor_tomba.h banner + docs/engine_re.md's "third dispatch
// layer map". FAITHFUL DRAFTS, UNWIRED, literal register-level transcription per fleet-workflow.md
// §9 (a future wiring session must still re-diff against generated/ before trusting these).
// ----------------------------------------------------------------------------

void ActorTomba::caseModeFsm_8006228C(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G + c->r[0];
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);                   // G+0x6 (sub-state)
  c->r[17] = c->r[0] + 1u;                                         // s1 = 1 (constant, dead scratch — never restored, matches gen)
  {
    const bool lt2 = ((int32_t)c->r[3] < 2);
    c->r[2] = (uint32_t)lt2;
    if (c->r[3] == c->r[17]) goto L_80062338;                     // state == 1
  }
  if (c->r[2] == c->r[0]) goto L_800622C8;                        // state >= 2
  c->r[4] = c->r[16] + c->r[0];
  if (c->r[3] == c->r[0]) goto L_800622E4;                        // state == 0
  goto L_800624A0;                                                 // unreachable (state<2, !=0, !=1 impossible for uint8)
L_800622C8:
  c->r[2] = c->r[0] + 2u;
  if (c->r[3] == c->r[2]) { c->r[2] = c->r[0] + 3u; goto L_80062404; }  // state == 2
  if (c->r[3] == c->r[2]) goto L_80062458;                        // state == 3
  goto L_800624A0;                                                 // state > 3: no-op
L_800622E4:                                                        // state-0 init
  c->r[31] = 0x800622ECu;
  c->mem_w8(c->r[16] + 326u, (uint8_t)c->r[0]);                    // G+0x146 = 0 (resetSwap-style clear)
  rec_dispatch(c, 0x80053D90u);                                    // still-substrate leaf — un-triaged
  c->r[31] = 0x800622F4u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                    // func_80054198(G)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 224u;
  c->r[31] = 0x80062304u;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                    // func_80054D14(G,224,4)
  c->r[4] = c->r[0] + 58u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x80062314u;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                    // SFX/cue trigger(58,0,0)
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[2] = c->r[0] + 30u;
  c->mem_w8(c->r[16] + 359u, (uint8_t)c->r[2]);                    // G+0x167 = 30
  c->r[2] = c->r[0] + 7u;
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                      // G+0x7 = 0
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                   // G+0x40 (timer) = 7
  c->mem_w16(c->r[16] + 66u, (uint16_t)c->r[0]);                   // G+0x42 = 0
  c->r[3] = c->r[3] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[3]);                      // G+0x6++
  // falls through into L_80062338 (state-1 body), no goto in gen
L_80062338:                                                        // state-1 body (also reached directly when state==1)
  c->r[5] = (uint32_t)c->mem_r8(c->r[16] + 327u);                  // G+0x147
  c->r[31] = 0x80062344u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055FBCu);                                    // still-substrate leaf(G, byte@G+0x147) — un-triaged
  c->r[31] = 0x8006234Cu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                    // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80062358u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80056B48u);                                    // still-substrate leaf(G,0) — un-triaged
  c->r[31] = 0x80062360u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                    // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 50u);                  // G+0x32
  c->r[4] = c->r[16] + c->r[0];
  c->r[2] = c->r[2] + 8u;
  c->r[31] = 0x80062374u;
  c->mem_w16(c->r[16] + 50u, (uint16_t)c->r[2]);                   // G+0x32 += 8
  rec_dispatch(c, 0x8005444Cu);                                    // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 41u);                   // G+0x29
  if (c->r[2] == c->r[0]) { c->r[4] = c->r[16] + c->r[0]; goto L_800623AC; }
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 64u);                  // G+0x40 (timer)
  c->r[2] = c->r[2] - 1u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                   // G+0x40--
  c->r[2] = c->r[2] << 16;                                          // sign-check region on the 16-bit timer
  if (c->r[2] != c->r[0]) goto L_800623AC;
  c->r[31] = 0x800623A8u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x8005A714u);                                    // still-substrate leaf(G) — un-triaged, fires once when timer wraps through 0
  c->r[4] = c->r[16] + c->r[0];
L_800623AC:
  c->r[31] = 0x800623B4u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80056C00u);                                    // still-substrate leaf(G,1) — un-triaged
  c->r[31] = 0x800623BCu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800551C4u);                                    // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 359u);                  // G+0x167 (separate 8-bit counter)
  if (c->r[2] != c->r[0]) { c->r[2] = c->r[2] - 1u; goto L_800623FC; }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 41u);                   // G+0x29 (reload)
  if (c->r[2] == c->r[0]) goto L_800623E8;
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[0]);                      // G+0x5 = 0 (re-idle)
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[0]);                      // G+0x6 = 0
  goto L_800624A0;
L_800623E8:
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x800623F4u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80056D44u);                                    // still-substrate leaf(G,0) — un-triaged
  goto L_800624A0;
L_800623FC:
  c->mem_w8(c->r[16] + 359u, (uint8_t)c->r[2]);                    // G+0x167--
  goto L_800624A0;
L_80062404:                                                        // state-2 body
  c->r[4] = c->r[16] + c->r[0];
  {
    const uint32_t base = (uint32_t)32780u << 16;
    c->mem_w8(base + (uint32_t)-2034, (uint8_t)c->r[0]);           // clear byte @ 0x800BF80E
  }
  c->r[31] = 0x80062418u;
  c->mem_w8(c->r[16] + 326u, (uint8_t)c->r[0]);                    // G+0x146 = 0
  rec_dispatch(c, 0x80053D90u);                                    // still-substrate leaf — un-triaged
  c->r[31] = 0x80062420u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                    // func_80054198(G)
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 223u;                                         // NOTE: 223, not 224 (state-0's value)
  c->r[31] = 0x80062430u;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                    // func_80054D14(G,223,4)
  c->r[4] = c->r[0] + 58u;
  c->r[5] = c->r[0] + c->r[0];
  c->r[31] = 0x80062440u;
  c->r[6] = c->r[5] + c->r[0];
  rec_dispatch(c, 0x80074590u);                                    // SFX/cue trigger(58,0,0)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->r[3] = c->r[0] + 30u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[3]);                   // G+0x40 (timer) = 30
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                      // G+0x6++ (exits directly)
  goto L_800624A0;
L_80062458:                                                        // state-3 body
  c->r[31] = 0x80062460u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                    // still-substrate leaf(G) — un-triaged
  c->r[31] = 0x80062468u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x800551C4u);                                    // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 64u);                  // G+0x40 (timer)
  c->r[2] = c->r[2] - 1u;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                   // G+0x40--
  c->r[2] = c->r[2] << 16;
  if (c->r[2] != c->r[0]) { c->r[2] = (uint32_t)32780u << 16; goto L_800624A0; }
  c->mem_w8(c->r[2] + (uint32_t)-2034, (uint8_t)c->r[17]);         // byte @ 0x800BF80E = 1 (r17 == 1)
  c->r[2] = c->r[0] + 4u;
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[2]);                      // G+0x4 = 4
  c->r[2] = c->r[0] + 32u;
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);                      // G+0x5 = 32
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[0]);                      // G+0x6 = 0
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                      // G+0x7 = 0
L_800624A0:
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::caseModeFsm_8006506C(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = G + c->r[0];
  c->mem_w32(c->r[29] + 28, c->r[31]);
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = (uint32_t)c->mem_r8(c->r[16] + 6u);                   // G+0x6 (sub-state)
  c->r[18] = c->r[0] + 1u;                                          // s2 = 1 (constant)
  {
    const bool lt2 = ((int32_t)c->r[17] < 2);
    c->r[2] = (uint32_t)lt2;
    if (c->r[17] == c->r[18]) goto L_80065188;                     // state == 1
  }
  if (c->r[2] == c->r[0]) goto L_800650AC;                         // state >= 2
  if (c->r[17] == c->r[0]) goto L_800650C8;                        // state == 0
  goto L_8006535C;                                                  // unreachable
L_800650AC:
  c->r[2] = c->r[0] + 2u;
  if (c->r[17] == c->r[2]) { c->r[2] = c->r[0] + 3u; goto L_800652B0; }  // state == 2
  if (c->r[17] == c->r[2]) goto L_80065324;                        // state == 3
  goto L_8006535C;                                                  // state > 3: no-op
L_800650C8:                                                         // state-0 init (gated)
  c->r[31] = 0x800650D0u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80054198u);                                    // func_80054198(G) (resetSwap)
  {
    uint32_t mask = (uint32_t)1088u << 16;                          // 0x1088<<16 (mid bits), OR 0x200 below
    c->r[3] = c->mem_r32(c->r[16] + 380u);                          // G+0x17C
    mask = mask | 512u;                                              // mask = 0x1088<<16 | 0x200
    c->r[3] = c->r[3] & mask;
    c->r[2] = c->r[0] + 512u;
    c->mem_w16(c->r[16] + 106u, (uint16_t)c->r[0]);                 // G+0x6A = 0 (unconditional side write, computed for both branches)
    if (c->r[3] != c->r[2]) goto L_80065138;                        // (G+0x17C & mask) != 0x200
  }
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 64u;
  c->r[6] = c->r[0] + 3u;
  c->r[2] = c->r[6] + c->r[0];
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                       // G+0x6 = 3
  c->r[31] = 0x80065108u;
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                       // G+0x7 = 0
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,64,3)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 348u);                   // G+0x15C
  c->r[2] = c->r[2] & 2u;
  if (c->r[2] == c->r[0]) { c->r[4] = c->r[16] + c->r[0]; goto L_8006535C; }
  c->r[2] = (uint32_t)c->mem_r16(c->r[4] + 50u);                    // G+0x32 (r4 still == G here)
  c->r[5] = c->r[0] + 129u;
  c->r[2] = c->r[2] + 32u;
  c->r[31] = 0x80065130u;
  c->mem_w16(c->r[4] + 50u, (uint16_t)c->r[2]);                     // G+0x32 += 32
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,129) — un-triaged
  goto L_8006535C;
L_80065138:                                                         // (G+0x17C & mask) != 0x200 branch
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 348u);                   // G+0x15C (reload)
  c->r[2] = c->r[2] & 2u;
  if (c->r[2] == c->r[0]) { c->r[5] = c->r[0] + 24u; goto L_80065168; }
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80065158u;
  c->r[6] = c->r[0] + 3u;
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,24,3) — arg5 set below fallthrough label, but this path recomputes G+6/G+7 directly
  c->r[2] = c->r[0] + 2u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                       // G+0x6 = 2
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                       // G+0x7 = 0
  goto L_8006535C;
L_80065168:
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 64u;
  c->r[31] = 0x80065178u;
  c->r[6] = c->r[0] + 3u;
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,64,3)
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 6u);
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                       // G+0x7 = 0
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                       // G+0x6++
  // falls through into L_80065188 (shared state-1-ish tail)
L_80065188:
  c->r[31] = 0x80065190u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                     // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)32783u << 16;
  c->r[3] = (uint32_t)c->mem_r16(c->r[2] + (uint32_t)-12460);        // DAT_8004CED4
  c->r[2] = c->r[3] & 16u;                                          // bit4
  if (c->r[2] == c->r[0]) { c->r[4] = c->r[16] + c->r[0]; goto L_800651E0; }
  c->r[5] = c->r[0] + 65u;
  c->r[31] = 0x800651B4u;
  c->r[6] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,65,0)
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x800651C0u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,0) — un-triaged
  c->r[2] = (uint32_t)32800u << 16;                                  // scratchpad base
  c->r[2] = (uint32_t)c->mem_r16(c->r[2] + 380u);
  c->r[2] = c->r[2] & 3u;
  if (c->r[2] != c->r[0]) { c->r[4] = c->r[0] + 5u; goto L_80065238; }
  c->r[5] = c->r[0] + 3u;
  goto L_80065210;
L_800651E0:
  c->r[2] = c->r[3] & 64u;                                          // bit6
  if (c->r[2] == c->r[0]) { c->r[5] = c->r[0] + 64u; goto L_80065220; }
  c->r[31] = 0x800651F4u;
  c->r[5] = c->r[0] + 1u;
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,1) — un-triaged
  c->r[2] = (uint32_t)32800u << 16;
  c->r[2] = (uint32_t)c->mem_r16(c->r[2] + 380u);
  c->r[2] = c->r[2] & 3u;
  if (c->r[2] != c->r[0]) { c->r[4] = c->r[0] + 5u; goto L_80065238; }
  c->r[5] = (uint32_t)-2;
L_80065210:
  c->r[31] = 0x80065218u;
  c->r[6] = (uint32_t)-60;
  rec_dispatch(c, 0x80074590u);                                     // SFX/cue trigger(<band>,0,-60)
  goto L_80065238;
L_80065220:
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x8006522Cu;
  c->r[6] = c->r[0] + 4u;
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G,64,4)
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80065238u;
  c->r[5] = c->r[0] + 2u;
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,2) — un-triaged
L_80065238:
  c->r[31] = 0x80065240u;
  rec_dispatch(c, 0x80055824u);                                     // ActorTomba::frameTick's own leaf per its header comment — result in r2
  if (c->r[2] == c->r[0]) { c->r[4] = c->r[0] + 29u; goto L_8006535C; }
  c->r[5] = c->r[0] + c->r[0];
  c->r[6] = c->r[5] + c->r[0];
  c->r[2] = c->r[0] + 4u;
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);                       // G+0x5 = 4
  c->r[2] = c->r[0] + 2u;
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[2]);                       // G+0x6 = 2
  c->r[2] = c->r[0] + 8u;
  c->mem_w8(c->r[16] + 356u, (uint8_t)c->r[0]);                     // G+0x164 = 0
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                       // G+0x7 = 0
  c->mem_w32(c->r[16] + 344u, c->r[0]);                             // G+0x158 (dword) = 0
  c->mem_w16(c->r[16] + 88u, (uint16_t)c->r[0]);                    // G+0x58 (word) = 0
  c->r[31] = 0x8006527Cu;
  c->mem_w16(c->r[16] + 64u, (uint16_t)c->r[2]);                    // G+0x40 (timer) = 8
  rec_dispatch(c, 0x80074590u);                                     // SFX/cue trigger(29,0,0)
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80065288u;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x80055E28u);                                     // still-substrate leaf(G,0) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[5] = c->r[0] + 20u;
  c->r[2] = (uint32_t)c->mem_r8(c->r[4] + 330u);                    // G+0x14A
  c->r[6] = c->r[0] + c->r[0];
  c->r[2] = c->r[2] & 1u;
  c->r[2] = c->r[2] + 2u;
  c->r[31] = 0x800652A8u;
  c->mem_w8(c->r[4] + 329u, (uint8_t)c->r[2]);                      // G+0x149 = 2 | (byte@G+0x14A & 1)
  rec_dispatch(c, 0x80054D14u);                                     // func_80054D14(G, 2|(byte@G+0x14A&1), 20)
  goto L_8006535C;
L_800652B0:                                                         // state-2 body
  c->r[31] = 0x800652B8u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                     // still-substrate leaf(G) — un-triaged
  c->r[2] = (uint32_t)32782u << 16;
  c->r[2] = (uint32_t)c->mem_r16(c->r[2] + 32360u);                 // DAT_80047E68
  c->r[2] = c->r[2] & 16u;                                          // bit4
  if (c->r[2] != c->r[0]) { c->r[2] = c->r[0] + 7u; goto L_800652E0; }
  c->r[31] = 0x800652D8u;
  rec_dispatch(c, 0x80055824u);                                     // ActorTomba::frameTick's own leaf — result in r2
  if (c->r[2] == c->r[0]) { c->r[2] = c->r[0] + 7u; goto L_800652F0; }
L_800652E0:
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[2]);                       // G+0x5 = 7
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[18]);                      // G+0x6 = 1 (r18 == 1)
  c->mem_w8(c->r[16] + 7u, (uint8_t)c->r[0]);                       // G+0x7 = 0
  goto L_8006535C;
L_800652F0:
  c->r[2] = (uint32_t)32783u << 16;
  c->r[2] = (uint32_t)c->mem_r16(c->r[2] + (uint32_t)-12460);        // DAT_8004CED4 (reload)
  c->r[2] = c->r[2] & 64u;                                          // bit6
  if (c->r[2] == c->r[0]) { c->r[4] = c->r[16] + c->r[0]; goto L_8006535C; }
  c->r[2] = (uint32_t)c->mem_r16(c->r[16] + 50u);                   // G+0x32
  c->r[5] = c->r[0] + 1u;
  c->r[2] = c->r[2] + 16u;
  c->r[31] = 0x8006531Cu;
  c->mem_w16(c->r[16] + 50u, (uint16_t)c->r[2]);                    // G+0x32 += 16
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,1) — un-triaged
  c->mem_w8(c->r[16] + 6u, (uint8_t)c->r[18]);                      // G+0x6 = 1
  goto L_8006535C;
L_80065324:                                                         // state-3 body
  c->r[31] = 0x8006532Cu;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80076D68u);                                     // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[16] + c->r[0];
  c->r[31] = 0x80065338u;
  c->r[5] = c->r[0] + 129u;
  rec_dispatch(c, 0x80062D8Cu);                                     // still-substrate leaf(G,129) — un-triaged
  c->r[2] = (uint32_t)32800u << 16;
  c->r[2] = (uint32_t)c->mem_r16(c->r[2] + 380u);
  c->r[2] = c->r[2] & 3u;
  if (c->r[2] != c->r[0]) { c->r[4] = c->r[0] + 5u; goto L_8006535C; }
  c->r[5] = (uint32_t)-2;
  c->r[31] = 0x8006535Cu;
  c->r[6] = (uint32_t)-60;
  rec_dispatch(c, 0x80074590u);                                     // SFX/cue trigger(-2,0,-60)
L_8006535C:
  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::nestedDispatch_800624B4(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = G + c->r[0];
  c->r[2] = c->r[0] + 1u;
  {
    uint32_t base634 = (uint32_t)8064u << 16;
    c->mem_w32(c->r[29] + 24, c->r[31]);
    c->mem_w32(c->r[29] + 16, c->r[16]);
    c->mem_w8(c->r[17] + 379u, (uint8_t)c->r[2]);                   // G+0x17B = 1
    c->r[2] = c->r[0] + 2u;
    c->mem_w8(base634 + 634u, (uint8_t)c->r[2]);                    // scratchpad byte @ +634 = 2
  }
  c->r[3] = (uint32_t)c->mem_r8(c->r[17] + 6u);                     // G+0x6
  {
    const bool ge5 = !(c->r[3] < 5u);
    c->r[2] = (uint32_t)32769u << 16;                                 // r2 = 0x80010000 unconditionally (matches gen's combined assign+branch)
    if (ge5) goto L_80062708;
  }
  c->r[2] = c->r[2] + 25628u;                                         // r2 = 0x800163DC (table base)
  c->r[3] = c->r[3] << 2;
  c->r[3] = c->r[3] + c->r[2];
  c->r[2] = c->mem_r32(c->r[3] + (uint32_t)0);
  switch (c->r[2]) {
    case 0x8006250Cu: goto L_8006250C;
    case 0x800625D4u: goto L_800625D4;
    case 0x800625F8u: goto L_800625F8;
    case 0x8006261Cu: goto L_8006261C;
    case 0x80062678u: goto L_80062678;
    default:
      // gen's own default arm: calls out WITHOUT the epilogue (dead per the <5 gate above,
      // mirrored literally — matches generated/shard_6.c:9640's `rec_dispatch(c, c->r[2]); return;`)
      rec_dispatch(c, c->r[2]);
      return;
  }
L_8006250C:
  {
    uint32_t base2040 = ((uint32_t)32780u << 16) + (uint32_t)-2040;   // 0x800BF7FA
    c->r[3] = base2040;
    c->r[2] = c->r[0] + 6u;
    c->mem_w8(c->r[3] + 6u, (uint8_t)c->r[0]);                       // byte @ 0x800BF800 = 0
    c->mem_w8(c->r[17] + 0u, (uint8_t)c->r[2]);                      // G+0x0 = 6
    c->r[2] = c->r[0] + 1u;
    c->mem_w8(c->r[3] + 1u, (uint8_t)c->r[2]);                       // byte @ 0x800BF7FB = 1
  }
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 2u);                      // G+0x2
  if (c->r[2] != c->r[0]) { c->r[4] = c->r[17] + c->r[0]; goto L_8006254C; }
  c->r[31] = 0x80062540u;
  c->r[4] = c->r[17] + c->r[0];
  rec_dispatch(c, 0x80053D90u);                                      // still-substrate leaf — un-triaged
  c->r[31] = 0x80062548u;
  c->r[4] = c->r[17] + c->r[0];
  rec_dispatch(c, 0x800551C4u);                                      // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[17] + c->r[0];
L_8006254C:
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 111u);                    // G+0x6F
  {
    uint32_t base1905 = ((uint32_t)32780u << 16) + (uint32_t)-1905;   // 0x800BF88F
    c->r[3] = base1905;
    c->r[31] = 0x8006255Cu;
    c->mem_w8(c->r[3] + (uint32_t)0, (uint8_t)c->r[2]);
  }
  rec_dispatch(c, 0x80067EF4u);                                      // still-substrate leaf(G, byte@G+0x6F) — un-triaged
  c->r[31] = 0x80062564u;
  rec_dispatch(c, 0x8001CF2Cu);                                      // still-substrate leaf() — un-triaged
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 2u);
  if (c->r[2] != c->r[0]) goto L_8006257C;
  c->r[31] = 0x8006257Cu;
  c->r[4] = c->r[17] + c->r[0];
  rec_dispatch(c, 0x80055D5Cu);                                      // still-substrate leaf(G) — un-triaged
L_8006257C:
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 6u);
  c->r[4] = c->r[17] + c->r[0];
  c->r[2] = c->r[2] + 1u;
  c->r[31] = 0x80062590u;
  c->mem_w8(c->r[17] + 6u, (uint8_t)c->r[2]);                        // G+0x6++
  rec_dispatch(c, 0x80076D68u);                                      // still-substrate leaf(G) — un-triaged
  c->r[4] = c->r[0] + 30u;
  c->r[31] = 0x8006259Cu;
  c->r[5] = c->r[0] + c->r[0];
  rec_dispatch(c, 0x800310F4u);                                      // still-substrate leaf(30,0) — un-triaged, spawning-style, result in r2/r16
  c->r[16] = c->r[2] + c->r[0];
  if (c->r[16] == c->r[0]) { c->r[4] = c->r[0] + 55u; goto L_800625B8; }
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 40u);
  c->r[2] = c->r[2] | 128u;
  c->mem_w8(c->r[16] + 40u, (uint8_t)c->r[2]);                       // spawned-item+0x28 |= 0x80
L_800625B8:
  c->r[5] = c->r[0] + 22u;
  c->r[31] = 0x800625C4u;
  c->r[6] = c->r[0] + 30u;
  rec_dispatch(c, 0x80074590u);                                      // SFX/cue trigger(22,0,30)
  c->r[2] = c->r[0] + 5u;
  c->mem_w32(c->r[17] + 16u, c->r[16]);                               // G+0x10 (dword) = spawned item ptr (or 0)
  c->mem_w16(c->r[17] + 64u, (uint16_t)c->r[2]);                      // G+0x40 (timer) = 5
  goto L_80062708;
L_800625D4:
  c->r[2] = (uint32_t)(int32_t)(int16_t)c->mem_r16(c->r[17] + 64u);   // G+0x40 (timer, sign-extended)
  c->r[3] = (uint32_t)c->mem_r16(c->r[17] + 64u);
  if (c->r[2] != c->r[0]) { c->r[2] = c->r[3] - 1u; goto L_800625F0; }
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 6u);
  c->mem_w8(c->r[17] + 1u, (uint8_t)c->r[0]);                        // G+0x1 = 0
  goto L_80062668;
L_800625F0:
  c->mem_w16(c->r[17] + 64u, (uint16_t)c->r[2]);                     // G+0x40--
  goto L_80062708;
L_800625F8:
  {
    uint32_t base = ((uint32_t)32800u << 16) + (uint32_t)-7968;      // 0x800FE0A0
    c->r[2] = (uint32_t)c->mem_r16(base);
  }
  if (c->r[2] == c->r[0]) goto L_80062664;
  c->r[31] = 0x80062614u;
  rec_dispatch(c, 0x8001CF2Cu);                                      // still-substrate leaf() — un-triaged
  goto L_80062708;
L_8006261C:
  {
    uint32_t p1 = ((uint32_t)32783u << 16) + (uint32_t)-12268;       // 0x8009D014 (dword pointer)
    c->r[4] = c->mem_r32(p1);
    uint32_t tbl = ((uint32_t)32784u << 16) + (uint32_t)-20112;      // per-item table base
    c->r[2] = (uint32_t)c->mem_r16(c->r[17] + 382u);                 // G+0x17E
    c->r[3] = tbl;
    c->r[2] = c->r[2] & 15u;
    c->r[5] = c->r[2] << 3;
    c->r[5] = c->r[5] + c->r[3];
    c->r[2] = c->r[2] << 3;
    c->r[2] = c->r[2] | 4u;
    c->r[2] = c->r[2] + c->r[3];
  }
  c->r[3] = c->mem_r32(c->r[5] + (uint32_t)0);
  c->r[6] = c->mem_r32(c->r[2] + (uint32_t)0);
  c->r[5] = c->r[3] >> 11;
  c->r[31] = 0x8006265Cu;
  c->r[6] = c->r[6] - c->r[3];
  rec_dispatch(c, 0x80044CD4u);                                      // still-substrate leaf(G, entry>>11, entry2-entry) — un-triaged, result in r2
  if (c->r[2] == c->r[0]) goto L_80062708;
L_80062664:
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 6u);
L_80062668:
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[17] + 6u, (uint8_t)c->r[2]);                        // G+0x6++
  goto L_80062708;
L_80062678:
  {
    uint32_t base = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8(base + 411u);                      // scratchpad byte @ +411
  }
  if (c->r[2] == c->r[0]) goto L_80062708;
  c->r[16] = c->mem_r32(c->r[17] + 16u);                              // G+0x10 (dword, set by case 0x8006250C)
  if (c->r[16] == c->r[0]) { c->r[2] = c->r[0] + 2u; goto L_800626A4; }
  c->mem_w8(c->r[16] + 4u, (uint8_t)c->r[2]);                         // item+0x4 = 2
  c->mem_w8(c->r[16] + 5u, (uint8_t)c->r[0]);                         // item+0x5 = 0
L_800626A4:
  c->r[31] = 0x800626ACu;
  c->r[4] = c->r[17] + c->r[0];
  rec_dispatch(c, 0x80057FD4u);                                      // still-substrate leaf(G) — un-triaged
  c->r[2] = c->r[0] + 1u;
  c->r[16] = (uint32_t)8064u << 16;
  c->mem_w8(c->r[17] + 1u, (uint8_t)c->r[2]);                         // G+0x1 = 1
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 311u);                     // scratchpad byte @ +311
  c->r[2] = c->r[0] + 1u;
  if (c->r[3] == c->r[2]) { c->r[2] = (uint32_t)32780u << 16; goto L_800626D4; }
  c->r[4] = (uint32_t)c->mem_r8(c->r[2] + (uint32_t)-1936);           // byte @ 0x800BF890-ish
  c->r[31] = 0x800626D4u;
  rec_dispatch(c, 0x80074F24u);                                       // still-substrate leaf(byte) — un-triaged
L_800626D4:
  c->r[2] = c->r[0] + 3u;
  c->mem_w8(c->r[17] + 0u, (uint8_t)c->r[2]);                         // G+0x0 = 3
  {
    uint32_t base = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32(base + 572u);                                // scratchpad dword @ +572
  }
  c->r[2] = c->r[0] + 30u;
  c->mem_w16(c->r[17] + 370u, (uint16_t)c->r[2]);                     // G+0x172 = 30
  c->mem_w32(c->r[17] + 4u, c->r[3]);                                 // G+0x4 (dword) = scratchpad dword @ +572
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 311u);
  c->r[2] = c->r[0] + 2u;
  if (c->r[3] != c->r[2]) { c->r[2] = (uint32_t)32780u << 16; goto L_80062704; }
  c->mem_w8(c->r[16] + 311u, (uint8_t)c->r[0]);                       // scratchpad byte @ +311 = 0
L_80062704:
  c->mem_w8(c->r[2] + (uint32_t)-2039, (uint8_t)c->r[0]);             // byte @ 0x800BF7D9 = 0
L_80062708:
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}

void ActorTomba::nestedDispatch_80060C60(uint32_t G) {
  Core* c = core;
  const uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = G + c->r[0];
  {
    uint32_t base = (uint32_t)8064u << 16;
    c->mem_w32(c->r[29] + 24, c->r[31]);
    c->mem_w32(c->r[29] + 16, c->r[16]);
    c->mem_w8(base + 635u, (uint8_t)c->r[0]);                        // scratchpad byte @ +635 = 0
  }
  c->r[3] = (uint32_t)c->mem_r8(c->r[17] + 6u);                       // G+0x6
  c->r[2] = (uint32_t)(c->r[3] < 8u);
  if (c->r[2] == c->r[0]) { c->r[2] = (uint32_t)32769u << 16; goto L_800617D0; }
  {
    uint32_t tblbase = ((uint32_t)32769u << 16) + 25596u;              // 0x800163BC table base
    c->r[3] = c->r[3] << 2;
    c->r[3] = c->r[3] + tblbase;
  }
  c->r[2] = c->mem_r32(c->r[3] + (uint32_t)0);
  switch (c->r[2]) {
    case 0x80060CACu: goto L_80060CAC;
    case 0x80061010u: goto L_80061010;
    case 0x800611B0u: goto L_800611B0;
    case 0x800611D8u: goto L_800611D8;
    case 0x800613F0u: goto L_800613F0;
    case 0x800614C0u: goto L_800614C0;
    case 0x800615C8u: goto L_800615C8;
    case 0x80061710u: goto L_80061710;
    default:
      // mirrors gen's own default arm — calls out WITHOUT the epilogue, dead per the <8 gate
      rec_dispatch(c, c->r[2]);
      return;
  }
  // TODO(wide-RE): cases 0x80060CAC/80061010/800611D8/800613F0/800614C0/800615C8/80061710 are
  // MAPPED ONLY (not transcribed) — see docs/engine_re.md's "third dispatch layer map" for
  // per-case line ranges + shape notes. This drafted skeleton is dead code (unwired), so the stub
  // labels below just reach the epilogue directly instead of the gen behavior — NOT faithful for
  // those 7 cases, faithful only for the frame/gate/switch shape and the one drafted case below.
L_80060CAC:
L_80061010:
L_800611D8:
L_800613F0:
L_800614C0:
L_800615C8:
L_80061710:
  goto L_800617D0;
L_800611B0:                                                           // smallest case (~10 gen-C lines) — drafted faithfully
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 327u);                      // G+0x147
  if (c->r[2] != c->r[0]) { c->r[2] = c->r[0] + 1792u; goto L_800611C4; }
  c->r[2] = c->r[0] + 256u;
L_800611C4:
  c->mem_w16(c->r[17] + 332u, (uint16_t)c->r[2]);                      // G+0x14C = 1792-or-256
  c->r[2] = (uint32_t)c->mem_r8(c->r[17] + 6u);
  c->mem_w16(c->r[17] + 334u, (uint16_t)c->r[0]);                      // G+0x14E = 0
  c->r[2] = c->r[2] + 1u;
  c->mem_w8(c->r[17] + 6u, (uint8_t)c->r[2]);                          // G+0x6++
  // gen falls through into case 0x800611D8's own body here (no goto) — that case is undrafted
  // (see TODO above), so this drafted body stops at the shared epilogue instead of the literal
  // fallthrough, which is NOT faithful past this point but is honestly marked as such.
  goto L_800617D0;
L_800617D0:
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = sp0;
}
