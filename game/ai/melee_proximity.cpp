// game/ai/melee_proximity.cpp — see melee_proximity.h for the RE writeup, field layout, and callee
// map. Transcribed from generated/shard_2.c:795 (`gen_func_8001F9DC`), the recompiler's own
// instruction-exact translation — the ground-truth source per CLAUDE.md, cross-checked against the
// Ghidra headless decompile (scratch/decomp/region_8001.c) for readability/structure only.
//
// NOTE ON OFFSET LITERALS: every actor-field offset below is a PLAIN DECIMAL integer, copied
// verbatim from generated/shard_2.c's own decimal literals (e.g. `mem_r16(self + 46u)`) — same
// convention as game/ai/actor_melee_engage.cpp, to avoid a hex/decimal transcription mismatch.
#include "melee_proximity.h"
#include "core.h"
#include "math/trig.h"

int32_t MeleeProximity::isAtApproachAnchor(uint32_t self, uint32_t other) {  // FUN_8001F9DC — UNWIRED draft
  Core* c = core;

  // ---- XZ distance test: self's position vs other's approach anchor (other.pos + other.anchorOfs) --
  // dx = self.X(+54) - (other.X(+54) + other.anchorOfsX(+96))
  const int32_t otherAnchorX = (int32_t)(uint16_t)c->mem_r16(other + 54u) +
                                (int32_t)(uint16_t)c->mem_r16(other + 96u);
  const int32_t dx = (int32_t)(int16_t)((uint16_t)c->mem_r16(self + 54u) - (uint16_t)otherAnchorX);
  // dz = self.Z(+46) - (other.Z(+46) + other.anchorOfsZ(+100))
  const int32_t otherAnchorZ = (int32_t)(uint16_t)c->mem_r16(other + 46u) +
                                (int32_t)(uint16_t)c->mem_r16(other + 100u);
  const int32_t dz = (int32_t)(int16_t)((uint16_t)c->mem_r16(self + 46u) - (uint16_t)otherAnchorZ);
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
  if (!(yThreshold < (int32_t)(uint16_t)(dy + heightSum))) return 0;  // outside the Y band

  // ---- pass: stamp the approach angle into shared scratchpad 0x1F80009C, using the ALREADY-NATIVE
  // Trig::ratan2 (guest FUN_80085690; a0=y=-dz, a1=x=dx, matching the recomp's own arg order) -------
  const int32_t angle = c->trig.ratan2(-dz, dx);
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
