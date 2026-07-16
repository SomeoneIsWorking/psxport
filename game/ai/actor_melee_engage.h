// game/ai/actor_melee_engage.h — class ActorMeleeEngage: FAITHFUL port of guest FUN_80112188
// (generated/ov_a00_shard_1.c:3527, function `ov_a00_gen_80112188`), an A00-overlay leaf that decides
// whether an AI actor should CLOSE IN on / mutually ARM a melee attack against a target, given a third
// "anchor" reference point (an approach-origin position, e.g. the actor's patrol/home spot).
//
// STATUS: RE'd + transcribed, then INDEPENDENTLY RE-VERIFIED line-by-line against
// generated/ov_a00_shard_1.c (the recompiler's instruction-exact ground truth) — 6 real bugs found
// and fixed in that pass (see actor_melee_engage.cpp's inline "BUG FIX" comments): a condition-
// polarity inversion on the kind-based Z-bias, a condition-polarity inversion on the Y-band test, a
// dx/dz argument swap in the ratan2 call, an rsin/rcos swap feeding the Z/X reposition update, a
// register (r21 vs r23) conflation in the bandWidth formula, and a missing register-lifetime setup
// (c->r[4]=angle) before the FUN_80055844 dispatch. WIRED via `overrides::install` passing
// ov_a00_set_override as the setter (the only real callers are DIRECT `ov_a00_func_80112188(c)`
// sites inside ov_a00_shard_1.c itself — see .cpp), which also makes it reachable via rec_dispatch
// (native-caller tracing). SBS-gated 0-diff; see registerOverrides().
//
// Guest ABI: a0 = self (the AI actor evaluating the engage), a1 = target (the actor it might attack),
// a2 = anchor (a position record — read-only, e.g. the patrol anchor or camera-follow point; NOT a
// full actor, only offsets +44/+48/+52 are read). Returns v0: 0 = no action (too far / wrong band /
// not ready), 1 = repositioned toward target this frame (still closing), 2 = armed/engaged (mutual
// attack-state transition performed), see doIt() for the exact branches.
//
// Guest frame: real 64-byte descent (`addiu sp,-64`), spills s0..s7 (r16..r23) + s8/fp (r30) + ra at
// +24/+28/+32/+36/+40/+44/+48/+52/+56/+60, one local (+16, the computed XZ distance). Every register
// spilled here is a pure scratch temporary (no meaning survives past this call), so the C++ locals
// below don't reproduce them by name — but doItFramed() reproduces the SP descent/ascent and the ra
// spill byte-for-byte per the CLAUDE.md "mirror the guest stack, never revert" directive, using the
// (currently uncalled) `object_table.cpp`/`cull.cpp` Framed() idiom as the reference shape.
//
// Field layout used (RAW DECIMAL OFFSETS, matching the recompiler's own decimal literals verbatim —
// this is a generic per-object AI record reused differently per object TYPE, same "no named struct"
// convention as game/ai/release_trigger_motion.h):
//   self+3/target+3  kind byte (only target's is read here, to pick a -70 vs 0 anchor-Z bias)
//   +4/+5/+6/+7      outer state / sub-state / two state bytes cleared together on transition
//   +41         "armed" one-shot flag (self AND target both stamped when a mutual arm fires)
//   +43         facing byte, stamped from the approach angle >> 4 when a state->19 attack-wind-up
//               transition fires
//   +46/+50/+54 self position triple (Z/Y/X order, matches the anchor's +44/+48/+52 triple)
//   +48         self Y position, stored as a 32-bit value with only the upper 16 bits meaningful
//               (`(anchor.Y + reach) << 16`) — a coarse/latched Y used only on the "already close,
//               arm directly" path
//   +68         facing/rotation word (u16); mirrored onto target+68 on one arm path
//   +74         a timer (s16) gating the state-23 re-arm window (>= 11265)
//   +80         timer reset to 0 on arm
//   +86         s16 passed as the SECOND Trig::angleCmp() probe's "a" argument (turnB, gates the
//               target[+95] branch below)
//   +94         target's "armed BY self" stamp (this actor's engage id, or 0 to clear)
//   +95         turn-cooldown byte (self and target each read/write their own)
//   +96         "engaged" latch, set to 1 when a reposition-with-lock-on begins
//   +98         target's facing-check (s16); ==0 selects the "mirror facing" quick-arm path
//   +128/+132/+134  hitbox radius / height / vertical half-extent (self and target both)
//   +320        s16 passed as the FIRST Trig::angleCmp() probe's "b" argument (turnA, mode=1) —
//               NOT a dead read (an earlier draft of this port mis-transcribed it as unused)
//   +325        bit 0 = "already armed" gate — the close-range arm path bails with 0 if ALREADY set
//   +328        timer reset to 0 on arm
//   +357        self's "lock owner" id byte; compared against the shared scratch owner slot
//   anchor+44/+48/+52 — anchor position triple (Z/Y/X)
//   SCRATCHPAD 0x1F800098 (absolute) — shared "current lock owner" id slot (compared to 1)
//   SCRATCHPAD 0x1F80009C (absolute) — shared "approach angle / stagger" word: written as the
//               atan2 angle when a reposition begins, later reinterpreted as a 12-bit wrapping
//               cooldown counter (decremented by 1024, masked to 0xFFF) on the close-range arm path.
//
// Callees (still substrate, reached via rec_dispatch — RE'd but this session did not chase or own
// them, out of this session's assigned address band):
//   FUN_80084080  a distance/sqrt-shaped leaf (a0 = sum of squares, v0 = magnitude). NOT the same
//                 address as the already-native `Math::isqrt16` (FUN_80077FB0) — a different
//                 fixed-point sqrt variant used only by AI code. Left substrate.
//   FUN_80055844  an opaque "may this actor attack now" permission check (RE not attempted this
//                 session — return value 0 vs nonzero gates the whole arm branch).
//   FUN_80022C78  a disengage/reset cleanup call (a0 = self) fired on every "abort the arm" tail.
// Callees ALREADY NATIVE — routed through the existing `Trig` class (game/math/trig.h) instead of
// rec_dispatch, since Trig::rsin/rcos/ratan2/angleCmp are RE'd-correct ports of FUN_80083E80/
// 80083F50/80085690/80077768 (confirmed via `tools/codemap.py --addr` returning "NO native owner" —
// i.e. Trig exists and is correct but is currently ORPHANED/unwired; using it here doesn't require
// wiring it, since this whole method is itself unwired dead code this session):
//   FUN_80083E80 -> Trig::rsin, FUN_80083F50 -> Trig::rcos, FUN_80085690 -> Trig::ratan2,
//   FUN_80077768 -> Trig::angleCmp.
#ifndef GAME_AI_ACTOR_MELEE_ENGAGE_H
#define GAME_AI_ACTOR_MELEE_ENGAGE_H
#include <cstdint>
class Core;
class Game;

class ActorMeleeEngage {
public:
  Core* core = nullptr;

  // doIt (FUN_80112188): native-C-stack call, NOT guest-ABI framed. Use this from any future NATIVE
  // caller (no guest stack frame needed). Returns v0 (0/1/2), see the .h banner above.
  int32_t doIt(uint32_t self, uint32_t target, uint32_t anchor);

  // doItFramed: guest-ABI-facing twin used by the ov_a00_set_override trampoline (see .cpp) —
  // mirrors the real 64-byte guest frame (spills s0..s7/s8/ra at their RE'd offsets) around doIt(),
  // per the CLAUDE.md "mirror the guest stack, never revert/exclude" directive.
  void doItFramed();

  // Wire doIt onto the guest address 0x80112188 via `overrides::install`, passing ov_a00_set_override
  // (the recompiler's own per-overlay call table — the only real callers found are direct
  // `ov_a00_func_80112188(c)` sites) as the setter so both that call shape and rec_dispatch
  // (native-caller tracing) reach the native. See .cpp.
  static void registerOverrides(Game* game);
};
#endif
