// game/ai/actor_melee_engage.cpp — see actor_melee_engage.h for the full RE writeup, field layout,
// and callee map. Transcribed 1:1 from generated/ov_a00_shard_1.c:3527 (`ov_a00_gen_80112188`); the
// original is a pure DAG (no back-edges), so the straight-line-with-early-returns shape below follows
// the recompiler's own control flow exactly rather than risking a mis-restructure under time pressure.
//
// NOTE ON OFFSET LITERALS: every object-field offset below is written as a PLAIN DECIMAL integer
// (e.g. `self + 46u`), copied verbatim from the recompiler's own decimal literals
// (`c->mem_r16((c->r[16] + (uint32_t)46))`) — deliberately NOT hex, so there is no hex/decimal
// transcription step (and no chance of a 46-vs-0x46 mixup) between the generated source and this file.
//
// TRANSCRIPTION CAVEAT (honest flag, not a hedge): this function is a dense MIPS branch-delay-slot
// DAG (~250 lines, ~30 conditional edges) hand-transcribed without a decompiler pass. Several
// condition-polarity and register-lifetime mistakes were found and fixed during this session's own
// re-reading (see git history of this file) — a reminder that this draft is UNWIRED/UNVERIFIED and
// should get an independent Ghidra decompile cross-check (or at minimum a byte-level SBS gate once
// wired) before anyone trusts it as byte-exact. Do not promote to "verified" on the strength of this
// comment alone.
#include "actor_melee_engage.h"
#include "core.h"
#include "math/trig.h"

// Still-substrate leaves this session did not chase (see .h banner). rec_dispatch already declared
// by core.h.

int32_t ActorMeleeEngage::doIt(uint32_t self, uint32_t target, uint32_t anchor) {  // FUN_80112188 — UNWIRED draft
  Core* c = core;

  // ---- kind-based anchor-Z bias: target.kind==5 -> 0, else -70 -----------------------------------
  const uint8_t targetKind = c->mem_r8(target + 3u);
  const int32_t kindZBias = (targetKind == 5) ? 0 : -70;

  // ---- XZ distance test ---------------------------------------------------------------------------
  // dz = self.Z(+46) - (anchor.Z(+44) + kindZBias); dx = self.X(+54) - anchor.X(+52)
  const int32_t dz = (int32_t)((uint16_t)c->mem_r16(self + 46u) -
                                (uint16_t)(c->mem_r16(anchor + 44u) + kindZBias));
  const int32_t dzS = (int16_t)dz;
  const int32_t dx = (int32_t)((uint16_t)c->mem_r16(self + 54u) - (uint16_t)c->mem_r16(anchor + 52u));
  const int32_t dxS = (int16_t)dx;
  const int32_t sumSq = dzS * dzS + dxS * dxS;

  // FUN_80084080 — still-substrate distance/sqrt-shaped leaf (see .h; NOT Math::isqrt16).
  c->r[4] = (uint32_t)sumSq;
  rec_dispatch(c, 0x80084080u);
  const uint16_t dist16 = (uint16_t)c->r[2];

  const int32_t radiusSum1 = (int16_t)c->mem_r16(self + 128u) + (int16_t)c->mem_r16(target + 128u);
  if (radiusSum1 < (int32_t)dist16) return 0;  // too far in XZ

  // ---- Y-band test ----------------------------------------------------------------------------------
  const int32_t dy = (int32_t)((uint16_t)c->mem_r16(self + 50u) - (uint16_t)c->mem_r16(anchor + 48u));
  const uint32_t heightSum = (uint16_t)c->mem_r16(self + 132u) + (uint16_t)c->mem_r16(target + 132u);
  const uint16_t yBandSum = (uint16_t)((uint32_t)dy + heightSum);
  const int32_t yThreshold = (int16_t)c->mem_r16(self + 134u) + (int16_t)c->mem_r16(target + 134u);
  if (!(yThreshold < (int32_t)yBandSum)) return 0;  // outside the Y band

  // ---- reach bounds (r19/r21 in the recomp) used by the angle-window test below -------------------
  int32_t reachHi, reachLo;
  if (dy >= 0) {
    const int32_t targetYFull = (uint16_t)c->mem_r16(target + 134u);
    const int32_t targetHeight = (uint16_t)c->mem_r16(target + 132u);
    const int32_t selfYFull = (uint16_t)c->mem_r16(self + 134u);
    const int32_t selfHeight = (uint16_t)c->mem_r16(self + 132u);
    const int32_t v = (targetYFull - targetHeight) + (selfYFull - selfHeight);
    reachHi = v; reachLo = v;
  } else {
    reachHi = (int32_t)heightSum;
    reachLo = -(int32_t)heightSum;
  }

  // ---- angle-window test ---------------------------------------------------------------------------
  const int32_t angle = c->trig.ratan2(-dzS, dxS);
  const int32_t radiusSum2 = (int16_t)c->mem_r16(self + 128u) + (int16_t)c->mem_r16(target + 128u);
  const int32_t margin = radiusSum2 - (int16_t)dist16;
  const int32_t bandWidth = (int16_t)reachHi - (int16_t)reachLo;

  bool doReposition;
  if (margin < bandWidth) {
    c->mem_w32(0x1F80009Cu, (uint32_t)angle);   // stamp the shared approach-angle scratch word
    doReposition = true;
  } else {
    doReposition = (margin < 3);
  }

  if (doReposition) {
    // ---- L_80112320: reposition self toward target along `angle`, scaled by the combined radius ----
    const int32_t radiusSum3 = (int16_t)c->mem_r16(self + 128u) + (int16_t)c->mem_r16(target + 128u);
    const int32_t sinV = c->trig.rsin(angle);
    const int32_t sinScaled = (int32_t)(((int64_t)sinV * (int64_t)radiusSum3) >> 12);
    const int32_t cosV = c->trig.rcos(angle);
    const int32_t cosScaled = (int32_t)(((int64_t)cosV * (int64_t)radiusSum3) >> 12);

    const int32_t self320 = c->mem_r16s(self + 320u);  // angleCmp call1's a1 (NOT a dead read)

    const int32_t newZ = sinScaled + (c->mem_r16(anchor + 44u) + kindZBias);
    c->mem_w16(self + 46u, (uint16_t)newZ);
    c->mem_w8(self + 96u, 1);  // "engaged" latch

    const int32_t newX = (int16_t)c->mem_r16(anchor + 52u) - cosScaled;
    c->mem_w16(self + 54u, (uint16_t)newX);

    const int32_t angleS16 = (int16_t)c->mem_r32(0x1F80009Cu);
    // CALL1: angleCmp(angle, self+320, mode=1) -> turnA; stamped (+2) into self+95.
    const int32_t turnA = c->trig.angleCmp(angleS16, self320, 1);
    c->mem_w8(self + 95u, (uint8_t)(turnA + 2));

    // CALL2: angleCmp(self+86, angle, mode=0) -> turnB. turnB==0 is a plain early return (no state
    // stamp on this path — only the two branches below the target[+95] check ever write state 19).
    const int32_t turnB = c->trig.angleCmp((int16_t)c->mem_r16s(self + 86u), angleS16, 0);
    if (turnB == 0) return 1;

    // target[+95]==1 branch vs self's own "lock owner" (+357) branch
    const uint8_t targetTurnState = c->mem_r8(target + 95u);
    if (targetTurnState == 1) {
      const uint8_t selfState = c->mem_r8(self + 4u);
      if (selfState != 1) return 1;
      const uint8_t selfSub = c->mem_r8(self + 5u);
      if (selfSub == 19) return 1;
      const uint32_t cooldown = c->mem_r32(0x1F80009Cu);
      c->mem_w8(self + 5u, 19);
      c->mem_w8(self + 6u, 0);
      c->mem_w8(self + 7u, 0);
      c->mem_w8(self + 43u, (uint8_t)(cooldown >> 4));
      return 1;
    }

    const uint8_t ownerId = c->mem_r8(self + 357u);
    if (ownerId == 0) return 1;
    const uint32_t ownerSlot = c->mem_r32(0x1F800098u);  // absolute scratch +152
    if (ownerSlot != 1) return 1;

    const int32_t facing = c->mem_r16s(self + 68u);
    const int32_t facingAbs = (facing >= 0) ? (int32_t)(uint16_t)c->mem_r16(self + 68u)
                                             : -(int32_t)(uint16_t)c->mem_r16(self + 68u);
    // NOTE: bails (returns 1) when facingAbs < 6657 — continues to the cooldown-arm block below
    // only once the facing delta has grown large enough (>=6657, ~140 degrees in PSX units).
    if ((int16_t)facingAbs < 6657) return 1;
    if (c->mem_r8(self + 4u) != 1) return 1;
    if (c->mem_r8(self + 5u) == 19) return 1;

    uint32_t cd = c->mem_r32(0x1F80009Cu);
    c->mem_w8(self + 5u, 19);
    c->mem_w8(self + 6u, 0);
    c->mem_w8(self + 7u, 0);
    c->mem_w8(self + 43u, (uint8_t)(cd >> 4));
    cd = (cd + (uint32_t)-1024) & 4095u;
    c->mem_w32(0x1F80009Cu, cd);
    c->mem_w8(target + 43u, (uint8_t)(((int32_t)cd < 2048) ? 2 : 3));
    return 1;
  }

  // ---- L_801124C0: already close enough — arm directly without repositioning --------------------
  // Shared abort tail (L_801125AC in the recomp): cleanup(self) + target+94=0, always returns 2.
  auto abortToDisengage = [&]() -> int32_t {
    c->r[4] = self;
    rec_dispatch(c, 0x80022C78u);
    c->mem_w8(target + 94u, 0);
    return 2;
  };

  if ((c->mem_r8(self + 325u) & 1u) != 0) return 0;  // bail if this bit is ALREADY set (already armed)

  c->mem_w8(self + 41u, 1);  // armed flag
  const int32_t reachY = (int16_t)reachLo;
  const uint32_t newY = (uint32_t)((c->mem_r32(anchor + 48u) + reachY) << 16);
  c->mem_w32(self + 48u, newY);  // coarse Y position

  const uint16_t selfFacing = c->mem_r16(self + 68u);
  const int32_t targetFaceCheck = c->mem_r16s(target + 98u);
  c->mem_w8(target + 41u, 1);  // armed flag
  // NOTE: this write happens UNCONDITIONALLY in the recomp (a branch-delay-slot instruction ahead
  // of the targetFaceCheck==0 test below) — every path through this tail stamps it, not just the
  // "mirror facing" one.
  c->mem_w16(target + 68u, selfFacing);

  if (targetFaceCheck == 0) return abortToDisengage();
  if ((c->mem_r8(self + 0u) & 4u) != 0) return abortToDisengage();

  const uint8_t lockOwner = c->mem_r8(self + 325u);  // reloaded (raw byte, not masked this time)
  if (lockOwner != 2) return abortToDisengage();
  if (!(c->mem_r8(self + 4u) < 2)) return abortToDisengage();

  // FUN_80055844 — still-substrate "may this actor attack now" permission check (see .h). NOTE: the
  // recomp does not (re)set a0(r4) anywhere in this tail before this call — it runs with whatever r4
  // last held from earlier in the function (NOT necessarily `self`); transcribed as-is rather than
  // guessing an argument that isn't actually there in the guest code.
  rec_dispatch(c, 0x80055844u);

  if (c->r[2] != 0) {
    // GRANTED: arm state 23, stamp target+94 with the RELOADED self+325 value (== 2, `lockOwner`).
    c->mem_w8(self + 325u, 0);
    c->mem_w8(target + 94u, lockOwner);
    c->mem_w8(self + 328u, 0);
    c->mem_w16(self + 80u, 0);
    c->mem_w8(self + 5u, 23);
    c->mem_w8(self + 6u, 0);
    c->mem_w8(self + 7u, 0);
    return 2;
  }

  // NOT granted (r2==0): L_80112568.
  if (c->mem_r8(self + 5u) == 23) {
    if (c->mem_r16s(self + 74u) < 11265) return abortToDisengage();
    // fallthrough: self+5==23 AND self+74>=11265 -> re-stamp same as the "not-yet-23" arm below,
    // but target+94 gets the ORIGINAL constant 1 here, not `lockOwner` (a real recomp asymmetry).
  }
  c->mem_w8(self + 325u, 0);
  c->mem_w8(target + 94u, 1);
  c->mem_w8(self + 328u, 0);
  c->mem_w16(self + 80u, 0);
  c->mem_w8(self + 5u, 23);
  c->mem_w8(self + 6u, 0);
  c->mem_w8(self + 7u, 0);
  return 2;
}

// doItFramed — guest-ABI-facing twin, mirrors the real 64-byte frame (spills s0..s7/s8/ra) around
// doIt(). UNWIRED/UNUSED this session (nothing calls it yet) but kept per the CLAUDE.md "mirror the
// guest stack, never revert/exclude a leaf because it pushes a frame" directive so wiring later needs
// no re-RE. a0/a1/a2 come off the incoming guest registers exactly as the recomp reads them.
void ActorMeleeEngage::doItFramed() {
  Core* c = core;
  const uint32_t self = c->r[4], target = c->r[5], anchor = c->r[6];
  const uint32_t save16 = c->r[16], save17 = c->r[17], save18 = c->r[18], save19 = c->r[19];
  const uint32_t save20 = c->r[20], save21 = c->r[21], save22 = c->r[22], save23 = c->r[23];
  const uint32_t save30 = c->r[30], saveRa = c->r[31];

  c->r[29] -= 64;
  c->mem_w32(c->r[29] + 28, save17);
  c->mem_w32(c->r[29] + 32, save18);
  c->mem_w32(c->r[29] + 56, save30);
  c->mem_w32(c->r[29] + 48, save22);
  c->mem_w32(c->r[29] + 60, saveRa);
  c->mem_w32(c->r[29] + 52, save23);
  c->mem_w32(c->r[29] + 44, save21);
  c->mem_w32(c->r[29] + 40, save20);
  c->mem_w32(c->r[29] + 36, save19);
  c->mem_w32(c->r[29] + 24, save16);

  c->r[2] = (uint32_t)doIt(self, target, anchor);

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
  c->r[29] += 64;
}
