// game/ai/melee_proximity.h — class MeleeProximity: FAITHFUL DRAFT port of guest FUN_8001F9DC
// (generated/shard_2.c:795, `gen_func_8001F9DC`), the SIMPLEST member of a ~9-function family of
// near-duplicate "is actor A within melee range + facing-cone of actor B's approach anchor" tests
// living at 0x8001CB00-0x80020000 (this session's assigned WIDE-RE band). See
// docs/engine_re.md "0x8001Cxxx-0x80020000 — melee-proximity/cone-arbitration family" for the full
// region survey (which sibling functions exist, what's shared, what's still unmapped).
//
// STATUS: RE'd via Ghidra headless decompile (scratch/decomp/region_8001.c) + cross-checked against
// generated/shard_2.c:795 (the recompiler's instruction-exact C, the actual ground truth per
// CLAUDE.md), then INDEPENDENTLY RE-VERIFIED line-by-line — 3 real bugs found and fixed (see
// melee_proximity.cpp's inline "BUG FIX" comments): the +96/+100 anchor-offset fields were swapped
// between X and Z, a condition-polarity inversion on the Y-band test (same class of bug as its
// ActorMeleeEngage sibling), and a dx/dz argument swap in the ratan2 call. WIRED via
// `overrides::install` passing shard_set_override as the setter (the only real callers are direct
// `func_8001F9DC(c)` sites in shard_1.c/shard_5.c), which also makes it reachable via rec_dispatch.
// SBS-gated 0-diff; see registerOverrides().
//
// Guest ABI: a0 = self (r19 in the recomp — the actor being tested), a1 = other (r16 — the actor
// whose approach-anchor offset is the target point). Returns v0: 1 = self is within combined-radius
// AND Y-band of other's approach anchor (and stamps the approach angle into shared scratchpad
// 0x1F80009C), 0 = out of range/band.
//
// SAME actor struct as the already-drafted `ActorMeleeEngage` (game/ai/actor_melee_engage.h) —
// corroborated by IDENTICAL field offsets: +46/+50/+54 = Z/Y/X position (per that file's own
// labelling), +128/+132/+134 = hitbox radius / height / vertical half-extent. This function reads a
// THIRD offset triple, +96/+98/+100, on the "other" actor — an "approach anchor" position OFFSET
// (relative to other's own +46/+50/+54), i.e. "the spot next to `other` where `self` should stand to
// engage it", embedded on the actor itself rather than passed as a separate anchor argument (unlike
// ActorMeleeEngage's separate anchor param).
//
// Field layout used (RAW DECIMAL OFFSETS, copied verbatim from the recompiler's own decimal literals
// — same convention as actor_melee_engage.h, no hex/decimal transcription step):
//   self+46 / other+46      Z position
//   self+50 / other+50      Y position
//   self+54 / other+54      X position
//   other+96/+98/+100       approach-anchor offset (Z/Y/X) relative to other's own position — the
//                           point self must occupy to be "at" the anchor
//   self+128 / other+128    hitbox radius (XZ, combined additively)
//   self+132 / other+132    hitbox height (Y-band, combined additively)
//   self+134 / other+134    vertical half-extent / Y-band tolerance (combined additively)
//   SCRATCHPAD 0x1F80009C (absolute) — shared "approach angle" word, written as
//               Trig::ratan2(-dz, dx) when the test passes (same shared slot ActorMeleeEngage uses).
//
// Callees:
//   FUN_80084080 — still-substrate distance/sqrt-shaped leaf (a0 = sum of squares, v0 = magnitude).
//                  Decompiled this session (see docs/engine_re.md): reads GTE coprocessor leading-
//                  zero-count (setCopReg(2,0xF000)/getCopReg(2,0xF800)) to normalize the input into a
//                  1024-entry Q12 reciprocal-sqrt table at guest 0x800A6310, then rescales by the
//                  zero count. NOT the same algorithm as the already-native `Math::isqrt16`
//                  (FUN_80077FB0) — left substrate, reached via rec_dispatch, exactly as
//                  ActorMeleeEngage does (same leaf, same reason).
//   FUN_80085690 -> already-native `Trig::ratan2` (game/math/trig.h) — used directly, no
//                  rec_dispatch needed (same rationale as ActorMeleeEngage: Trig is RE'd-correct but
//                  currently orphaned/unwired, so using it here doesn't require wiring anything).
#ifndef GAME_AI_MELEE_PROXIMITY_H
#define GAME_AI_MELEE_PROXIMITY_H
#include <cstdint>
class Core;
class Game;

class MeleeProximity {
public:
  Core* core = nullptr;

  // isAtApproachAnchor (FUN_8001F9DC): native-C-stack call, NOT guest-ABI framed. Returns v0 (0/1).
  int32_t isAtApproachAnchor(uint32_t self, uint32_t other);

  // isAtApproachAnchorFramed: guest-ABI-facing twin used by the shard_set_override trampoline (see
  // .cpp) — mirrors the real 40-byte guest frame (spills s0..s2/ra at their RE'd offsets:
  // r19->sp+28, r16->sp+16, ra->sp+32, r18->sp+24, r17->sp+20) around isAtApproachAnchor(), per the
  // CLAUDE.md "mirror the guest stack" directive. Reads a0/a1 from c->r[4]/c->r[5] (guest ABI),
  // writes result to c->r[2] (v0), matching gen_func_8001F9DC's own entry/exit convention.
  void isAtApproachAnchorFramed();

  // Wire isAtApproachAnchor onto guest address 0x8001F9DC via `overrides::install`, passing
  // shard_set_override (the recompiler's own global call table — the only real callers found are
  // direct `func_8001F9DC(c)` sites) as the setter so both that call shape and rec_dispatch
  // (native-caller tracing) reach the native. See .cpp.
  static void registerOverrides(Game* game);
};
#endif
