// game/ai/melee_proximity.cpp — see melee_proximity.h for the RE writeup, field layout, and callee
// map. Transcribed from generated/shard_2.c:795 (`gen_func_8001F9DC`), the recompiler's own
// instruction-exact translation — the ground-truth source per CLAUDE.md, cross-checked against the
// Ghidra headless decompile (scratch/decomp/region_8001.c) for readability/structure only.
//
// NOTE ON OFFSET LITERALS: every actor-field offset below is a PLAIN DECIMAL integer, copied
// verbatim from generated/shard_2.c's own decimal literals (e.g. `mem_r16(self + 46u)`) — same
// convention as game/ai/actor_melee_engage.cpp, to avoid a hex/decimal transcription mismatch.
#include "melee_proximity.h"
#include "game_ctx.h"
#include "core.h"
#include "game.h"
#include "override_registry.h"
#include "math/trig.h"

int32_t MeleeProximity::isAtApproachAnchor(uint32_t self, uint32_t other) {  // FUN_8001F9DC — UNWIRED draft
  Core* c = core;

  // ---- XZ distance test: self's position vs other's approach anchor (other.pos + other.anchorOfs) --
  // BUG FIX (RE cross-check against generated/shard_2.c:795): the +96/+100 anchor-offset fields were
  // swapped in the original draft. Ground truth's FIRST block reads other+46 (Z) paired with
  // other+96, and its SECOND block reads other+54 (X) paired with other+100 — i.e. +96 is the Z
  // anchor offset and +100 is the X anchor offset (matching this file's own .h banner, which the
  // .cpp itself contradicted).
  // dz = self.Z(+46) - (other.Z(+46) + other.anchorOfsZ(+96))
  const int32_t otherAnchorZ = (int32_t)(uint16_t)c->mem_r16(other + 46u) +
                                (int32_t)(uint16_t)c->mem_r16(other + 96u);
  const int32_t dz = (int32_t)(int16_t)((uint16_t)c->mem_r16(self + 46u) - (uint16_t)otherAnchorZ);
  // dx = self.X(+54) - (other.X(+54) + other.anchorOfsX(+100))
  const int32_t otherAnchorX = (int32_t)(uint16_t)c->mem_r16(other + 54u) +
                                (int32_t)(uint16_t)c->mem_r16(other + 100u);
  const int32_t dx = (int32_t)(int16_t)((uint16_t)c->mem_r16(self + 54u) - (uint16_t)otherAnchorX);
  const int32_t sumSq = dx * dx + dz * dz;

  // FUN_80084080 — still-substrate GTE-LZCS-table sqrt (see .h). Left substrate, rec_dispatch'd.
  c->r[4] = (uint32_t)sumSq;
  rec_dispatch(c, 0x80084080u);
  const uint16_t dist16 = (uint16_t)c->r[2];

  const int32_t radiusSum = (int16_t)c->mem_r16(other + 128u) + (int16_t)c->mem_r16(self + 128u);
  if (radiusSum < (int32_t)dist16) return 0;  // too far in XZ

  // ---- Y-band test: self.Y vs (other.Y + other.anchorOfsY(+98)) -----------------------------------
  const int32_t otherAnchorY = (int32_t)(uint16_t)c->mem_r16(other + 50u) +
                                (int32_t)(uint16_t)c->mem_r16(other + 98u);
  const int32_t dy = (int32_t)(int16_t)((uint16_t)c->mem_r16(self + 50u) - (uint16_t)otherAnchorY);
  const int32_t heightSum = (int16_t)c->mem_r16(other + 132u) + (int16_t)c->mem_r16(self + 132u);
  const int32_t yThreshold = (int16_t)c->mem_r16(other + 134u) + (int16_t)c->mem_r16(self + 134u);
  // BUG FIX (RE cross-check, same polarity class as ActorMeleeEngage's Y-band test): ground truth's
  // `{ int _t=(r3!=0)/*yThreshold<yBandSum*/; r4=-dx(delay); if(_t) goto returnFalse; }` fails (out
  // of band -> return 0) when yThreshold < yBandSum, not the negated form the original draft used.
  if (yThreshold < (int32_t)(uint16_t)(dy + heightSum)) return 0;  // outside the Y band

  // ---- pass: stamp the approach angle into shared scratchpad 0x1F80009C, using the ALREADY-NATIVE
  // Trig::ratan2. BUG FIX (RE cross-check): ground truth sets a0=-dx (r17=dx, negated, in the
  // branch's delay slot), a1=dz (r18=dz, NOT negated) before the FUN_80085690 dispatch — i.e.
  // ratan2(-dx, dz). The original draft swapped dx/dz (ratan2(-dz, dx)); ActorMeleeEngage's sibling
  // function has the identical convention (ratan2(-dxS, dzS)), confirming this argument order.
  const int32_t angle = trigOf(c).ratan2(-dx, dz);
  c->mem_w32(0x1F80009Cu, (uint32_t)angle);
  return 1;
}

void MeleeProximity::isAtApproachAnchorFramed() {  // guest-ABI twin, mirrors gen_func_8001F9DC's frame
  Core* c = core;
  const uint32_t self = c->r[4];
  const uint32_t other = c->r[5];
  const uint32_t savedSp = c->r[29];
  c->r[29] -= 40u;
  c->mem_w32(c->r[29] + 28u, c->r[19]);
  c->mem_w32(c->r[29] + 16u, c->r[16]);
  c->mem_w32(c->r[29] + 32u, c->r[31]);
  c->mem_w32(c->r[29] + 24u, c->r[18]);
  c->mem_w32(c->r[29] + 20u, c->r[17]);
  c->r[19] = self;
  c->r[16] = other;

  c->r[2] = (uint32_t)isAtApproachAnchor(self, other);

  c->r[31] = c->mem_r32(c->r[29] + 32u);
  c->r[19] = c->mem_r32(c->r[29] + 28u);
  c->r[18] = c->mem_r32(c->r[29] + 24u);
  c->r[17] = c->mem_r32(c->r[29] + 20u);
  c->r[16] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = savedSp;
}

// ---------------------------------------------------------------------------------------------
// Wiring: the only real callers found for 0x8001F9DC are DIRECT `func_8001F9DC(c)` sites in
// shard_1.c/shard_5.c — calls through the recompiler's OWN global g_override[] table, never
// through rec_dispatch. Installing without a setter would be invisible to that call shape, so
// `overrides::install` is passed shard_set_override as the setter, same pattern as
// game/core/pc_scheduler.cpp / game/object/actor_sm_reward.cpp.
// ---------------------------------------------------------------------------------------------
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_8001F9DC(Core*);   // substrate body — kept alive for psx_fallback (core B)

namespace {
void ov_meleeProximity(Core* c) { eng(c).meleeProximity.isAtApproachAnchorFramed(); }
}  // namespace

void MeleeProximity::registerOverrides(Game* /*game*/) {
  overrides::install(0x8001F9DCu, "MeleeProximity::isAtApproachAnchor",
                     ov_meleeProximity, gen_func_8001F9DC, shard_set_override);
}
