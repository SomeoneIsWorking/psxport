// game/ai/release_trigger_motion.h — class ReleaseTriggerMotion: PC-native ownership of the six
// per-node[3]-type SUB-MOTION machines the FUN_80124E74 "release trigger" object-behavior jump
// table (game/ai/beh_jumptable_release_trigger.cpp) dispatches into via rec_dispatch:
//
//   FUN_80123E9C  hoverBobCycle    — node[5] 5-state timer: a back-and-forth Y-bob cadence
//                                    (node+0x40 countdown, node+0x48/0x4e velocity/accel,
//                                    node+0x56 phase ramps by 0x80 to a 0x800 wrap). jt case 6.
//   FUN_801241BC  leaderFollowSync — node[0x5e]==0: reads a "leader" node via node[0x10], tests a
//                                    bitmask (0x800BF9CC) keyed by the leader's type byte, and
//                                    either free-runs (spawn a queue-A cull entry + boundsCheck via
//                                    FUN_80123C94 + hoverBobCycle + a scratchpad position refresh)
//                                    or SNAPS to the leader's position/heading and switches to
//                                    follow mode (node[0x5e]=1, re-arms node[5]/[6]). node[0x5e]==1
//                                    hands off to FUN_8012400C (still-substrate). jt case 0.
//   FUN_801244E8  driftReposition — node[5] 2-state: seed a spawn point (camera-relative default,
//                                    or a per-type table read at DAT_801498B0[(node60-2)*6]), then
//                                    on tick: boundsCull -> 3 still-substrate finish calls; area
//                                    byte 0x800BF9DD==0xe re-rolls the spawn point (2x rng draws,
//                                    occasionally spawns a projectile via Spawn::spawnAndInit) and
//                                    rearms; <0xe defers to FUN_80124328 (still-substrate). jt
//                                    case 1 (variant=0) and case 4-v2 (variant=1).
//   FUN_801246B4  arcSwoopMotion  — node[5] 4-state SWOOP: idle (0) -> arm at a per-type X offset
//                                    from DAT_80109B44[node60] with SFX model-attach (1) -> wait
//                                    for area<0xf then re-seed + cull-driven advance (state 1
//                                    loop) -> velocity ramp (2/3) with an alternating side-offset
//                                    + SFX 0x8f trigger each pass. jt case 1 (v==1 sub-branch).
//   FUN_801249D4  doubleArcMotion — node[5] 9-state (0..8) DOUBLE ARC: waits for the leader's Y to
//                                    clear a threshold, arms from DAT_80109B50[node60-2], then
//                                    alternates a +-0x80000 X impulse and a velocity ramp across two
//                                    passes (states 1..4 outbound, 5..8 return), each terminated by
//                                    an SFX 0x8f trigger, finishing at node[0x5e]=2. jt case 4 (v==1).
//   FUN_80124C6C  circleOrbitMotion — node[5] 3-state ORBIT: arm from DAT_80109B7C[node60-2] with a
//                                    per-side (node[0x46]) velocity triad, run a boundsCull-gated
//                                    orbit tick that clamps X toward the table's stored X and bumps
//                                    Y, then on reaching the table's Y threshold flips side + SFX
//                                    0x8f. jt case 5.
//
// These are RAW-OFFSET node accessors (same idiom as the sibling beh_jumptable_release_trigger.cpp
// dispatcher and every other beh_* file): the node layout is a polymorphic blob reused differently
// per object TYPE, so a named struct isn't meaningful across call sites. RE'd via Ghidra headless
// (tools/decomp.sh) on a fresh field RAM dump (scratch/ram/band12.bin), cross-checked against
// tools/disas.py --ram for the jump-table addresses and the FUN_80077B38 model-pointer immediate
// (0x8014C808 — the same shared "release-trigger sprite" pointer used across the beh_jumptable_*
// family, e.g. beh_typed_anim_spawn.cpp's A1_MODEL).
//
// Ownership boundary: control flow + the direct node/global writes are native; every call into a
// NOT-YET-OWNED leaf (FUN_80077B5C position-commit, FUN_80051D90/FUN_80051794/FUN_800847F0/
// FUN_80084360 scratchpad-anim helpers, FUN_80123C94/FUN_8012400C/FUN_80124328 sibling sub-states)
// stays reachable by address via rec_dispatch, same as every other beh_* handler.
//
// Wired via the shared override registry (runtime/recomp/override_registry.h, `overrides::install`)
// at these six guest addresses — NOT the BehaviorDispatch per-object table (that table is for the
// OUTER per-object handler; these are internal CALL TARGETS the outer handler and each other reach
// through rec_dispatch, so they must intercept at the rec_dispatch choke point for every caller,
// substrate included).
#ifndef GAME_AI_RELEASE_TRIGGER_MOTION_H
#define GAME_AI_RELEASE_TRIGGER_MOTION_H
#include <cstdint>
class Core;

class ReleaseTriggerMotion {
public:
  Core* core = nullptr;

  void hoverBobCycle(uint32_t obj);                       // FUN_80123E9C
  void leaderFollowSync(uint32_t obj);                    // FUN_801241BC
  void driftReposition(uint32_t obj, uint32_t variant);   // FUN_801244E8
  void arcSwoopMotion(uint32_t obj);                       // FUN_801246B4
  void doubleArcMotion(uint32_t obj);                      // FUN_801249D4
  void circleOrbitMotion(uint32_t obj);                    // FUN_80124C6C

  // Install the six addresses above in the shared override registry (called once at boot).
  void registerOverrides();
};
#endif
