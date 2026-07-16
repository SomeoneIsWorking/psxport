// game/ai/beh_actor_tomba_proximity_combat.h — PC-native per-object AI think function FUN_800527C8
// (MAIN.EXE, generated/shard_3.c:13494 `gen_func_800527C8`, ground truth).
//
// STATUS: VERIFIED (line-by-line vs generated/shard_3.c ground truth — body logic had NO bugs; the
// one real bug was a missing guest-stack frame, fixed) + WIRED via the shared override registry
// (RegisterBehActorTombaProximityCombatOverride, called from register_engine_overrides(),
// `overrides::install`). Reached ONLY via rec_dispatch (an indirect function-pointer table read
// from a per-object "think" slot) — no static `func_800527C8(c)` call site exists in any generated
// shard (confirmed by grep), so the registry's oracle-gated dispatch intercepts it regardless of
// which object's spawn stamped the pointer; no setter needed (no direct intra-shard call bypasses
// rec_dispatch). NOT YET
// SBS-gated to a confirmed FIRE — autonav on the intro-area build may not reach an enemy encounter
// that exercises this leaf; if `ovhit` shows zero hits, the 0-diff gate proves no regression to the
// frames reached, not correctness of this handler (that rests on the RE verification above).
//
// WHAT IT IS: an "enemy actor vs Tomba" proximity-combat state machine. a0 = the acting object (an
// enemy/hostile actor record, generic layout — same field convention as game/ai/actor_melee_engage.h
// and game/ai/melee_proximity.h: +3 kind, +46/+50/+54 Z/Y/X position triple, +64/+66/+74 timers,
// +68 rotation, +86/+96/+98/+100/+102/+104 secondary position/turn scratch, +184/+186/+188 a
// counter+snapshot pair). It reads and WRITES the fixed Tomba G-block (ActorTomba::G_ADDR =
// 0x800E7E80, matched by the local `G` in the .cpp) as its "target": Tomba's own +46/+50/+54/+86/
// +378/+1 fields get stamped by several branches below — this is the native mirror of "the enemy
// bumps/interrupts Tomba's walk state on engage".
//
// Outer dispatch on obj+4 ("mode"):
//   mode==0            -> INIT: gate on FUN_800519E0 (GraphicsBind::recordArrayInit, ALREADY
//                         NATIVE — game/world/graphics_bind.h) succeeding; on success seeds several
//                         fields (obj+124/+60 via still-substrate FUN_80041718) then, keyed on
//                         obj+3 (kind gate), either arms Tomba via Engine::walkStart(G, mode=228,
//                         sub=0) [obj+3==0 path] or (obj+3!=0) re-derives the same via a second
//                         FUN_80041718 + Engine::walkStart(G, 228, 0) call — i.e. TWO slightly
//                         different init sequences depending on the enemy kind gate.
//   mode==1            -> ACTIVE: the 5-way SUBSTATE machine below, keyed on obj+5, selected from
//                         one of TWO parallel jump tables by obj+3 (kind/mode gate):
//                           obj+3==0 -> table A @ 0x80016DB0+obj5*4 (decoded literal, NOT re-derived
//                                       from the table read — see .cpp switch statement for the 5
//                                       literal case targets, read 1:1 off the generated code):
//                             substate 0 (0x800529EC) — "engage": pulls Tomba's Y toward self
//                             (self+50-60 -> G+50), then on a countdown (self+64) hitting -1 calls
//                             FUN_80042728 (still-substrate predicate); on success re-derives a
//                             secondary position (self+100/102/104) via two Math::applyMatrixLV
//                             calls (already-native — game/math/gte_math.h) against fixed matrix/
//                             vector tables at 0x1F800118/0x1F8000C0/0x1F800014 (scratchpad-
//                             relative), i.e. an attack-approach-vector recompute.
//                             substate 1 (0x80052B18) — countdown/arm: decrements self+64; at 0,
//                             stamps a fixed hitbox-table entry (0x1F7FF800+7/+49) and re-arms
//                             self+64=20, bumps self+5 (advance substate), copies self+96/98 into
//                             self+100/104 (commit the pending approach vector).
//                             substate 2 (0x80052B70) — orbit/circle: Sfx::trigger (already-native
//                             — game/audio/sfx.h) gated by a global mode mask (0x1F800178&7), then
//                             a countdown; at 0, ORs 0x80 into the hitbox-table flag byte and re-
//                             arms self+5 (advance), stamps a global cooldown byte (0x1F7FF800-32660
//                             = 0x1F7F800C-ish scratchpad byte) to 15.
//                             substate 3 (0x80052C10) — same Sfx::trigger + countdown shape as
//                             substate 2, but on countdown expiry checks a GLOBAL state byte
//                             (0x1F800278? see 8064<<16 + 634) == 2 to bail to the "reset" tail
//                             (L_80052D1C) instead of advancing, else claims a shared lock
//                             (0x1F7FF800+566 = 6) and clears bit 0x80 of the hitbox flag, advances
//                             self+5.
//                             substate 4 (0x80052CB8) — same Sfx::trigger/countdown shape, common
//                             tail only (no state-specific write) — falls into the two "helper
//                             leaf" calls FUN_80052720/FUN_8005262C (still-substrate, addresses
//                             just below this session's assigned band) then the RESET tail
//                             (L_80052D1C: sync self's position triple onto G, clear self+1).
//                           obj+3!=0 -> table B @ 0x80016DC8+obj5*4, a DIFFERENT 5-way bank with
//                             the SAME substate index (0..4) but different bodies:
//                             substate 0 (0x80052DA0) — mirrors table-A substate-0's approach-
//                             vector recompute but ALSO stamps G+1=1 (marks Tomba "engaged") and
//                             calls Engine::walkStart(self, 6144, 0) [note: self, not G, unlike the
//                             mode==0 init path] plus a still-substrate FUN_800782B0 "random-ish
//                             offset" leaf.
//                             substate 1 (0x80052E68) — two still-substrate helper-leaf calls
//                             (FUN_80052720/FUN_80052694) then a "did the approach vector change"
//                             gate (self+50 vs self+102) advancing self+5 via still-substrate
//                             FUN_80041768 (a small per-substate counter bump, same leaf table-A's
//                             substates 0/1 use) if it did change.
//                             substate 2 (0x80052EB0) — builds a 3-halfword vector from self+100/
//                             102/104 into a LOCAL scratch buffer (see .cpp for the scoped-sp-
//                             carve rationale — this handler is native-call-convention, not guest-
//                             ABI-framed, so it can't reuse the guest's own 72-byte frame the way
//                             MAIN.EXE's compiled body did) and calls still-substrate FUN_8006CEC4
//                             (a distance/clamp-shaped leaf, judging by the boolean success gate),
//                             advancing self+5 on success (return code 1) via FUN_80041768.
//                             substate 3 (0x80052F00) — still-substrate FUN_800776F8 (an angle-step
//                             leaf, args self+96/self+86/128) recomputing self+86; on convergence
//                             (result == self+96) advances self+5 AND fires Engine::walkStart
//                             (G, 2, 16) — a Tomba anim-state kick.
//                             substate 4 (0x80052F50) — a countdown-to-zero on self+184 (wrapping
//                             256..) that on expiry (a) resets 3 unrelated global scratch bytes and
//                             re-arms obj+4=3 (transition to despawn mode next frame) OR (b) just
//                             nudges both G+50 and self+50 by +10 each tick (a slow mutual pull-
//                             together) and clamps G+50 to not exceed self+98; falls into the
//                             "commit self+184 snapshot into self+186/188" common tail.
//                         COMMON TAIL (both tables, all substates): if self+1 (a "position changed"
//                         flag several substates set) is nonzero, sync self's position (46/50-with-
//                         a-clearance-check/54/86) onto G, THEN unconditionally call Engine::
//                         animTick(self) [already-native] + Engine::objMatrixCompose(self)
//                         [already-native].
//   mode in {2,3}      -> despawn: Spawn::despawn semantics via still-substrate FUN_8007A624 (the
//                         SAME address game/world/spawn.h's Spawn::despawn already owns natively —
//                         this draft still routes through rec_dispatch per the "prefer rec_dispatch
//                         for wired addresses" convention rather than calling Spawn::despawn(self)
//                         directly, since the draft is a mechanical 1:1 transliteration this
//                         session did not re-verify against Spawn::despawn's exact semantics).
//   mode>=4            -> no-op (idle).
//
// Field offsets are RAW DECIMAL LITERALS matching the recompiler's own decimal output verbatim (same
// convention as actor_melee_engage.h/melee_proximity.h) — no hex/decimal transcription step.
//
// Callees already NATIVE (routed via rec_dispatch anyway, per the "uniform dispatch" override
// convention — calling rec_dispatch on an address installed in the override registry reaches the
// native automatically once wired; this draft does not itself call the C++ methods directly):
//   FUN_800519E0 -> GraphicsBind::recordArrayInit (game/world/graphics_bind.h)
//   FUN_80054D14 -> Engine::walkStart            (game/core/engine.h)
//   FUN_8004190C -> Engine::animTick             (game/core/engine.h)
//   FUN_800518FC -> Engine::objMatrixCompose     (game/core/engine.h)
//   FUN_80074590 -> Sfx::trigger                 (game/audio/sfx.h)
//   FUN_80084470 -> Math::applyMatrixLV          (game/math/gte_math.h)
//   FUN_8007A624 -> Spawn::despawn               (game/world/spawn.h)
//
// Callees still STAGE-substrate (RE'd only to the extent needed to place their args; not chased
// into this session, reached via rec_dispatch exactly as the generated code does):
//   FUN_80041718, FUN_80041768  — small per-object counter/state-bump leaves (2-3 args).
//   FUN_800782B0                — writes a value derived from two s16 inputs back through a0
//                                  (looks RNG/offset-shaped; return feeds a position field).
//   FUN_80042728                — niladic predicate (bool-shaped return in v0).
//   FUN_8006CEC4                — vector-in (a1=ptr to 3 halfwords) distance/clamp leaf, boolean
//                                  success in v0.
//   FUN_800776F8                — angle-step leaf (a0=current, a1=target, a2=max-step), returns
//                                  the stepped value.
//   FUN_80052720, FUN_8005262C  — niladic "helper" leaves called at the tail of several substates
//                                  (just below this session's 0x800527C8 band, not re-derived here).
//
#ifndef GAME_AI_BEH_ACTOR_TOMBA_PROXIMITY_COMBAT_H
#define GAME_AI_BEH_ACTOR_TOMBA_PROXIMITY_COMBAT_H
class Core;

// FUN_800527C8 — see the file banner above for the full state-machine writeup. a0 (c->r[4]) = the
// acting object; no return value (v0 unused by any caller shape found).
void beh_actor_tomba_proximity_combat(Core* c);

#endif
