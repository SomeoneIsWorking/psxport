// game/scene/scene_events.cpp — PC-native SCENE-EVENT ARM subsystem (FUN_80040B48 + FUN_80040A58).
//
// RE'd from disas 0x80040B48..0x80040BFC (arm) + 0x80040A58..0x80040AA0 (classSize) on the resident
// MAIN.EXE. Six callsites — see docs/journal, arc-12. The class is stateless (methods over Core*);
// all state lives in guest RAM at the addresses named in scene_events.h.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "scene/scene_events.h"
#include "core/engine.h"
#include "game.h"              // c->game->verify — the shared A/B verify scaffold
#include "override_registry.h"   // overrides::install — the one native-override registry
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

// --- Scene-command record (FUN_80042258 / FUN_80042448 handlers; r4 = record pointer) -------------
// The command record whose selector byte the two handlers below decode. Field usage differs slightly
// between the two entry points (see per-field notes), but the offsets are shared.
static const uint32_t CMD_TIMER   = 100u;  // u16 — dwell timer (delayedTrigger phase 1)
static const uint32_t CMD_SELECT  = 114u;  // int16 — selector: latch-bit mask (trigger) / op mode (flagOp)
static const uint32_t CMD_ARG_A   = 116u;  // int16 — arg A: latch-half A value (trigger) / table index (flagOp)
static const uint32_t CMD_ARG_B   = 118u;  // arg B: latch-half B value (trigger) / flag byte (flagOp)
static const uint32_t CMD_PHASE   = 120u;  // u8  — phase (delayedTrigger)

static const uint32_t LATCH_HALF_A   = 0x800E7EAEu;  // int16 param-latch half A (selector bit 0)
static const uint32_t LATCH_HALF_B   = 0x800E7EB6u;  // int16 param-latch half B (selector bit 1)
static const uint32_t ARM_READY_BYTE = 0x800BF80Eu;  // u8 — set once this frame's arm has fired
static const uint32_t FLAG_TABLE_BASE= 0x800BF850u;  // flag/param table; entry byte @ +argA+324

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
  uint32_t size = eng(c).sceneEvents.classSize((uint8_t)eventId, /*nibbleLo=*/false);
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

// ORACLE: gen_func_80042258
// FUN_80042258 — two-phase dwell trigger over a scene-command record (r4). Leaf, no frame.
uint32_t SceneEvents::delayedTrigger(Core* c) {
  const uint32_t rec = c->r[4];
  const uint32_t phase = c->mem_r8(rec + CMD_PHASE);

  if (phase == 0) {
    // Phase 0: reset the dwell timer and advance to phase 1.
    const uint32_t p = c->mem_r8(rec + CMD_PHASE);   // recomp reloads the phase byte before ++
    c->mem_w16(rec + CMD_TIMER, 0);
    c->mem_w8(rec + CMD_PHASE, (uint8_t)(p + 1u));
    return 0;
  }

  if (phase == 1) {
    // Phase 1: latch the record args into the global param halves per the selector low bits.
    if (c->mem_r16(rec + CMD_SELECT) & 1u)
      c->mem_w16(LATCH_HALF_A, (uint16_t)c->mem_r16(rec + CMD_ARG_A));
    if (c->mem_r16(rec + CMD_SELECT) & 2u)   // recomp reloads the selector before testing bit 1
      c->mem_w16(LATCH_HALF_B, (uint16_t)c->mem_r16(rec + CMD_ARG_B));

    // Fire immediately once this frame's arm has fired.
    if (c->mem_r8(ARM_READY_BYTE) != 0) return 1;

    // Otherwise advance the dwell timer; fire on timeout (>= 500), else keep waiting.
    const uint32_t t = c->mem_r16(rec + CMD_TIMER) + 1u;
    c->mem_w16(rec + CMD_TIMER, (uint16_t)t);
    if ((int32_t)(int16_t)(uint16_t)t < 500) return 0;
    return 1;
  }

  return 0;
}

// ORACLE: gen_func_80042448
// FUN_80042448 — set/OR/AND a flag byte in the flag table per the record's op mode. Leaf, no frame.
uint32_t SceneEvents::applyFlagOp(Core* c) {
  const uint32_t rec = c->r[4];
  const int16_t mode = (int16_t)c->mem_r16(rec + CMD_SELECT);

  if (mode == 0) {
    // Mode 0: SET the flag byte outright.
    const uint32_t idx = (uint32_t)(int16_t)c->mem_r16(rec + CMD_ARG_A);
    const uint32_t val = c->mem_r8(rec + CMD_ARG_B);
    c->mem_w8(FLAG_TABLE_BASE + idx + 324u, (uint8_t)val);
    return 1;
  }

  if (mode == 1 || mode == 2) {
    // Mode 1: OR, mode 2: AND — read-modify-write the same flag byte.
    const uint32_t idx  = (uint32_t)(int16_t)c->mem_r16(rec + CMD_ARG_A);
    const uint32_t bits = c->mem_r8(rec + CMD_ARG_B);
    const uint32_t addr = FLAG_TABLE_BASE + idx;
    const uint32_t cur  = c->mem_r8(addr + 324u);
    const uint32_t val  = (mode == 1) ? (cur | bits) : (cur & bits);
    c->mem_w8(addr + 324u, (uint8_t)val);
  }

  // Every path (including a negative/>=3 mode) returns 1.
  return 1;
}

void SceneEvents::delayedTriggerOverride(Core* c) { c->r[2] = delayedTrigger(c); }
void SceneEvents::applyFlagOpOverride(Core* c)    { c->r[2] = applyFlagOp(c); }

extern void gen_func_80040B48(Core*);
extern void gen_func_80042258(Core*);
extern void gen_func_80042448(Core*);
extern void shard_set_override(uint32_t, void (*)(Core*));

void SceneEvents::registerOverrides(Game* /*game*/) {
  overrides::install(0x80040B48u, "SceneEvents::armBody",
                     SceneEvents::armOverride, gen_func_80040B48, shard_set_override);
  overrides::install(0x80042258u, "SceneEvents::delayedTrigger",
                     SceneEvents::delayedTriggerOverride, gen_func_80042258, shard_set_override);
  overrides::install(0x80042448u, "SceneEvents::applyFlagOp",
                     SceneEvents::applyFlagOpOverride, gen_func_80042448, shard_set_override);
}
