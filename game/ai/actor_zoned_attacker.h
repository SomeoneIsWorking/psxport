// game/ai/actor_zoned_attacker.h — PC-native bodies for the "zoned attacker" per-object sub-
// behavior cluster (FUN_8014047C/80140544/801409C0/80143A00/80144928/80144B50). See
// actor_zoned_attacker.cpp for the full RE trace and wiring rationale.
//
// These six functions are SUB-BEHAVIOR callees of the already-native
// game/ai/beh_id_compare_motion_dispatch.cpp (FUN_80145230, guest 0x8014xxxx OVERLAY area). That
// caller reaches them all via `rec_dispatch(c, addr)` (never a direct substrate jal), so installing
// each guest address in the shared override registry with no setter is sufficient — no
// `shard_set_override` / g_override[] dual-registration is needed here (unlike ActorReward, whose
// sole caller is substrate and calls by a direct C function pointer).
#pragma once
struct Core;
class  Game;

class ActorZonedAttacker {
public:
  static void gateCheck(Core* c);                // FUN_8014047C(node) -> bool v0
  static void typeInit(Core* c);                  // FUN_80140544(node)
  static void pickAttackByRange(Core* c);         // FUN_801409C0(node[, unused a1]) -> byte v0
  static void defaultSubStateMachine(Core* c);    // FUN_80143A00(node)
  static void approachAndFace(Core* c);           // FUN_80144928(node) -> v0
  static void idleTick(Core* c);                  // FUN_80144B50(node)

  // Install all six guest addresses in the shared override registry so rec_dispatch(c, addr) from
  // the (native) caller lands here, traced via the `dispatch` debug channel.
  static void registerOverrides(Game* game);
};
