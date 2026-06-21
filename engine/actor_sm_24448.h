// engine/actor_sm_24448.h — PC-native per-object actor STATE-MACHINE handler FUN_80024448.
// One concrete actor "move-and-collide" SM step: derive the probe args from the object's velocity
// fields, run the shared grid move-collide probe, then apply the result to the object's floor-type /
// angle / state fields. Registered from game_tomba2.cpp by name. See actor_sm_24448.cpp for the RE.
#ifndef ENGINE_ACTOR_SM_24448_H
#define ENGINE_ACTOR_SM_24448_H
void actor_sm_24448_register(void);   // installs the FUN_80024448 override
#endif
