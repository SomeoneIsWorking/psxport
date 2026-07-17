// class Spawn — PC-native ENTITY SPAWN / DESPAWN subsystem, owned by Engine.
//
// PROPER OOP: one instance per Core, reached as `eng(c).spawn.method(args)`. Back-pointer
// `core` wired once at Core construction time (same pattern as Placement / Animation / Collision).
//
// SCOPE: the engine path that instantiates and retires objects in the active object pool — pool-pop +
// field-init + list-link on spawn, and the mirror list-unlink + free-list return on despawn. Covers:
//   FUN_80079C3C  spawn primitive         (pool-pop + list-link)
//   FUN_8007A980  per-type dispatcher
//   FUN_8007A624  despawn primitive
//   FUN_8003116C  spawn-and-init helper
// The per-type spawn dispatch tables (0x80016e4c/0x80016e64) and their thin handlers stay PSX
// (content routing). Was the free functions `spawn_dispatch` / `world_despawn` / `world_spawn_and_init`
// taking Core*; the class removes that surface — Core is reached via the back-pointer.
#ifndef GAME_WORLD_SPAWN_H
#define GAME_WORLD_SPAWN_H
#include <cstdint>
struct Core;

// A pool free-list descriptor: the guest address of the free-list HEAD pointer (u32) and of the free
// COUNT byte (u8). Used by the spawn variants + the despawn free-push.
struct PoolDesc { uint32_t free_head, cnt; };

class Spawn {
public:
  Core* core = nullptr;

  // dispatch(cls, type, list): FUN_8007A980 per-type spawn dispatcher. cls picks the per-type spawn
  //   VARIANT; type and list are the object's type byte + destination linked-list. Returns node ptr
  //   (0 on out-of-range cls, or if the pool is empty).
  uint32_t dispatch(uint32_t cls, uint32_t type, uint32_t list);

  // despawn(node): FUN_8007A624 — unlink node from its live list and return it to the free pool. The
  //   live-wiring entry the per-object AI behavior handlers use in place of `rec_dispatch(c,
  //   0x8007A624u)` where their control flow is already native (PC calls PC for what it owns).
  void despawn(uint32_t node);

  // spawnAndInit(a0, posSrc, a2): FUN_8003116C — pool-0 spawn (cls=0/type=6/list=1) + optional
  //   position seed from `posSrc` + heading `a2`, then a PSX per-object init (FUN_80028E10) run via
  //   rec_dispatch. Returns node ptr (0 on pool-empty).
  uint32_t spawnAndInit(uint32_t a0, uint32_t posSrc, uint32_t a2);

  // sceneEntity(sceneId, subtype): FUN_8007E110 — allocate a SCENE-ENTITY node (class-3 tail-insert
  //   into active list 1 via the FUN_8007A5A8 allocator), install the per-frame scene-entity handler
  //   (0x8007DDE0), and initialise the node's data-table pointers from *(u32)0x800ECF60 (the field
  //   scene-entity data table). Returns node ptr on success or 0 on pool exhaustion (the caller
  //   stashes the return in Actor::sceneHandle (obj+0x14) and treats nonzero as "spawned"). The
  //   per-frame handler FUN_8007DDE0 stays reachable via its stored address (recomp substrate).
  uint32_t sceneEntity(uint16_t sceneId, uint8_t subtype);

  // dropScoreGem(sourceNode, value): FUN_8004B3F4 — thin wrapper around the score-gem spawner
  //   FUN_80071B44. Advances the running score-total counter at 0x800BF874 by `value`, then
  //   invokes FUN_80071B44(sourceNode, value, 0) which allocates a gem entity from the spawner
  //   pool, seeds its icon/hash from `value` (small-gem sprite 0x7C7E vs large-gem sprite 0x7C3E
  //   at the 5000-point boundary), and plays SFX 0x11 (the gem-pickup jingle via Sfx::trigger).
  //   The callee stays as substrate (gem spawner + SFX cluster). Used only by the item-drop
  //   dispatcher `beh_visibility_gate_dispatch` node[3] cases 4..11 — the 8 AP-gem denominations
  //   100/200/500/1000/5000/10000/20000/100000. Return is always 1 (recomp `addiu v0, zero, 1`).
  //   NOTE: FUN_80071B44 has a param_3 branch that would double-bump 0x800BF874 when param_3==1,
  //   but every callsite here (and the recomp wrapper itself) passes param_3=0, so the second
  //   bump is dormant — replicate the wrapper's single unconditional bump.
  void dropScoreGem(uint32_t sourceNode, int32_t value);

  // spawnOverlayVariant(recordIndex, variant): FUN_8007E038 — allocate a class-3 tail-insert node
  //   (the SAME FUN_8007A5A8 specialised allocator sceneEntity uses) and install the per-frame
  //   "variant overlay" handler (0x8007DC38, game/ai/beh_variant_overlay_lifecycle.cpp), seeding
  //   node[3]=variant, node[0x5E]=recordIndex, and the record-table pointers node[0x48]/[0x4C]/
  //   [0x50] from the SAME global list *(u32)0x800ECF60 that sceneEntity seeds from. Guarded: the
  //   allocation only happens if (variant != 0) OR (DAT_800BF81E==2) OR (DAT_800BF822==0); every
  //   other combination is a silent miss (returns 0). Returns the node ptr, or 0 on guard-miss /
  //   freelist exhaustion. Used by tickLinkedOverlay (below) and directly by
  //   game/ai/beh_pad_child_linker.cpp for its two linked-overlay children.
  uint32_t spawnOverlayVariant(uint16_t recordIndex, int16_t variant);

  // tickLinkedOverlay(obj, recordId): FUN_800735F4 — per-object controller that owns exactly ONE
  //   linked "variant overlay" child (spawned via spawnOverlayVariant) at obj[0x14], driven by a
  //   state byte obj[7] and a countdown obj[0x40]:
  //     state 0: if 0x800BF816==0 && obj[0x29]!=0, spawn the child (spawnOverlayVariant(recordId,
  //       2)); on success seed the countdown to 0x46 and advance obj[7].
  //     state 1: if (0x800BF816!=0 && 0x800BF80F==0) — a pause/freeze gate — kill the child
  //       (obj[0x14]->state=2 if still <2, clear the ptr) and reset obj[7]=0. Otherwise decrement
  //       the countdown; once it rolls from 0 to -1, kill the child the same way and advance obj[7].
  //     state 2: if obj[0x29]==0, reset obj[7]=0; otherwise no-op (holds at state 2 until the
  //       gate byte clears).
  //     state >2: no-op.
  //   No other substrate calls in this body — every op is a direct memory read/write plus the one
  //   call into spawnOverlayVariant. Body from disas 0x800735F4..0x8007374C.
  void tickLinkedOverlay(uint32_t obj, int16_t recordId);
  // spawnTypedChild(owner, cls, handlerAddr, typeByte, hasSub, sub): shared body for the 4 overlay
  //   TYPED-CHILD SPAWN leaves below — allocate via the owned dispatch(cls, type=4, list=0), then on
  //   success stamp the fresh node's per-object handler [+0x1C], owner back-pointer [+0x10], content-
  //   type byte [+2], and (when hasSub) a caller sub-index byte [+3]. Returns 0 on pool-empty.
  uint32_t spawnTypedChild(uint32_t owner, uint32_t cls, uint32_t handlerAddr, uint8_t typeByte,
                           bool hasSub, uint32_t sub);
  uint32_t spawnQuadRecordChild(uint32_t owner, uint32_t sub);    // FUN_801360F4
  uint32_t spawnSiblingAngleChild(uint32_t owner, uint32_t sub);  // FUN_80139838
  uint32_t spawnChildTrigChild(uint32_t owner, uint32_t sub);     // FUN_8013AC34
  uint32_t spawnLiftPlatformChild(uint32_t owner);                // FUN_8013A730

  // spawnEffectChild(owner, sub): FUN_80031558 — MAIN.EXE "spawn a child effect object" leaf. Allocate
  //   an effect node via the per-type dispatcher FUN_8007A980 (cls=0, type=6, list=1), then on success
  //   stamp the per-frame handler 0x80029B40 at [+0x1C], list/state byte 32 at [+0x0B], owner back-
  //   pointer at [+0x10], effect data-table ptr 0x80029F6C at [+0x18], caller sub-index (low byte) at
  //   [+3], and OR 0x80 into the flag byte at [+0x28]. Returns the node ptr, or 0 on pool exhaustion.
  //   READY-FRAME leaf (frame=32, spills ra/s1/s0). Wired via registerTypedChildOverrides().
  uint32_t spawnEffectChild(uint32_t owner, uint32_t sub);        // FUN_80031558

  // Wire the 4 typed-child spawners above into the override registry (overrides::install) at their
  // guest addresses so substrate/native rec_dispatch callers (beh_box_seed_phase_gate,
  // beh_single_child_cull) reach the native bodies instead of the recompiled ones. Called once at
  // boot (boot.cpp).
  void registerTypedChildOverrides();

private:
  // Guest-ABI bodies + shared pool helpers (static: plain fn-pointer shape for the verify gate).
  static void spawnLinkStamp(Core* c, uint32_t node, uint32_t ref, uint32_t type, uint32_t mode,
                             uint32_t list);
  static uint32_t entitySpawnBody(Core* c);                 // FUN_80079C3C
  static uint32_t spawnPool2Body(Core* c);                  // FUN_80079DDC
  static uint32_t poolSpawn(Core* c, const PoolDesc& p);    // FUN_80079F90 / 8007A12C / 8007A2C8 shared
  static uint32_t spawnVariantNative(Core* c, uint32_t cls);
  static uint32_t spawnAndInitBody(Core* c);                // FUN_8003116C
  static uint32_t sceneEntityBody(Core* c);                 // FUN_8007E110
  static uint32_t spawnOverlayVariantBody(Core* c);         // FUN_8007E038
};

#endif
