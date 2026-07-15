// game/scene/scene_events.cpp — PC-native SCENE-EVENT ARM subsystem (FUN_80040B48 + FUN_80040A58).
//
// RE'd from disas 0x80040B48..0x80040BFC (arm) + 0x80040A58..0x80040AA0 (classSize) on the resident
// MAIN.EXE. Six callsites — see docs/journal, arc-12. The class is stateless (methods over Core*);
// all state lives in guest RAM at the addresses named in scene_events.h.

#include "core.h"
#include "cfg.h"
#include "scene/scene_events.h"
#include "core/engine.h"
#include "game.h"              // c->game->verify — the shared A/B verify scaffold
#include "engine_overrides.h"  // EngineOverrides::register_ (FUN_80040B48 override wiring)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// Guest-address constants (see scene_events.h for the full state map).
static const uint32_t EVENTS_GATE_HW  = 0x800E7FEEu;  // int16 — 0 disables the event system
static const uint32_t ARM_SLOT_BASE   = 0x800BF870u;  // per-slot arm-flag table, +arg+68 = flag byte
static const uint32_t ARM_COUNTER_HW  = 0x800BF8A8u;  // u16 arm counter
static const uint32_t STREAM_CURSOR   = 0x800BF874u;  // u32 event-stream write cursor
static const uint32_t RING_BASE       = 0x800ED058u;  // event ring buffer base
static const uint32_t RING_HEAD_BYTE  = 0x800ED06Du;  // u8 ring write-head
static const uint32_t TBL_A_BASE      = 0x800A33C8u;  // classSize table A (stride 12)
static const uint32_t TBL_B_BASE      = 0x800A3B38u;  // classSize table B (stride 4, 16 entries)
static const uint32_t RING_REC_STRIDE = 0;            // stride not fixed — record slot is idx-indexed
                                                       // directly via head byte; stamp offsets 0x16 and
                                                       // 0x1C are hard-wired (matches recomp).

uint32_t SceneEvents::classSize(uint8_t argKey, bool nibbleLo) {
  Core* c = this->core;
  // Table A: 12-byte stride, we want byte @+1 of the entry. lbu zero-extends into 32-bit v0, so the
  // recomp's `sra v0, v0, 4` on that (always 0..255) value is identical to a logical >> 4 — no sign
  // extension in play. Pick low or high nibble per the a1 arg.
  uint32_t nibbleByte = c->mem_r8(TBL_A_BASE + (uint32_t)argKey * 12u + 1u);
  uint32_t nibble = nibbleLo ? (nibbleByte & 0x0Fu) : ((nibbleByte >> 4) & 0x0Fu);
  return c->mem_r32(TBL_B_BASE + nibble * 4u);
}

// The arm primitive body. Wrapped by SceneEvents::arm below so it can be A/B'd via the verify harness.
// Return value convention (mirrors the recomp exactly):
//    -1 if events gate off (recomp's `addiu v0, zero, -1` on the early-return path)
//     0 if the per-slot flag was already set (recomp's `addu v0, zero, zero` on the branch epilogue)
//     1 if fresh arm (recomp's `addiu v0, zero, 1` seq at the tail — the +1 constant that also bumps
//                     the ring write head).
uint32_t SceneEvents::armBody(Core* c) {
  // Full 32-bit slot index — gen_func_80040B48 indexes SLOT_STATE with r4 UNMASKED (`r3 = r17 + base;
  // lbu r3+68`). Masking to a byte here was a latent deviation, observable only if an event ID ever
  // reached >= 256 (it never does in-game, but the verify gate would have caught it). Matches the
  // ground truth and the former CubeTextLedger::activateSlot copy now deduped onto this body.
  const uint32_t eventId = c->r[4];

  // (a) Global events gate — 0 disables the whole system.
  if ((int16_t)c->mem_r16(EVENTS_GATE_HW) == 0) return (uint32_t)(int32_t)-1;

  // (b) Per-slot arm flag: bail with 0 if already armed, else arm it.
  const uint32_t slotByteAddr = ARM_SLOT_BASE + eventId + 68u;
  if (c->mem_r8(slotByteAddr) != 0) return 0;
  c->mem_w8(slotByteAddr, 1);

  // (c) Bump global arm counter.
  c->mem_w16(ARM_COUNTER_HW, (uint16_t)(c->mem_r16(ARM_COUNTER_HW) + 1u));

  // (d) Advance stream cursor by classSize(arg, high nibble) — reuses the public method so
  //     substrate callers of FUN_80040A58 still see identical semantics.
  c->r[4] = eventId; c->r[5] = 0;
  uint32_t size = c->engine.sceneEvents.classSize((uint8_t)eventId, /*nibbleLo=*/false);
  c->mem_w32(STREAM_CURSOR, c->mem_r32(STREAM_CURSOR) + size);

  // (e) Append to the ring. record[idx]+0x16 = arg, record[idx]+0x1C = 0. Head byte holds `idx`
  //     as a raw byte offset — the recomp reads it three times (once per stamp) then bumps it by 1;
  //     we do the same to preserve the exact write pattern (each read is fresh, so a concurrent
  //     write between them would be racy, but the recomp isn't concurrent).
  uint8_t idx = c->mem_r8(RING_HEAD_BYTE);
  c->mem_w8(RING_BASE + (uint32_t)idx + 0x16u, (uint8_t)eventId);
  idx = c->mem_r8(RING_HEAD_BYTE);
  c->mem_w8(RING_BASE + (uint32_t)idx + 0x1Cu, 0);
  idx = c->mem_r8(RING_HEAD_BYTE);
  c->mem_w8(RING_HEAD_BYTE, (uint8_t)(idx + 1u));

  (void)RING_REC_STRIDE;
  return 1;
}

int32_t SceneEvents::arm(uint8_t eventId) {
  Core* c = this->core;
  c->r[4] = eventId;
  c->game->verify.run(&SceneEvents::armBody, 0x80040B48u, "sceneeventsarmverify",
                      c->game->verify.on("sceneeventsarmverify"));
  return (int32_t)c->r[2];
}

// FUN_80040B48 override entry (guest ABI: slot in r4, ret in r2). Single canonical body for every
// caller that reaches the guest ADDRESS (substrate func_80040B48, and rec_dispatch(0x80040B48) from
// ActorReward) — as opposed to the native `arm(eventId)` API used by ordinary engine code.
void SceneEvents::armOverride(Core* c) { c->r[2] = armBody(c); }

extern void gen_func_80040B48(Core*);
extern void shard_set_override(uint32_t, void (*)(Core*));
namespace {
// psx_fallback-gated so SBS core B (the pure reference) keeps running the exact recompiled body —
// g_override[] is a single table shared by every Core/Game (same pattern the deduped cube_text_ledger
// entries follow for FUN_80040C00/80040AA4).
void ov_sceneEventArm(Core* c) {
  if (c->game->psx_fallback) { gen_func_80040B48(c); return; }
  SceneEvents::armOverride(c);
}
}  // namespace

void SceneEvents::registerOverrides(Game* game) {
  game->engine_overrides.register_(0x80040B48u, "SceneEvents::armBody", SceneEvents::armOverride);
  shard_set_override(0x80040B48u, ov_sceneEventArm);
}
