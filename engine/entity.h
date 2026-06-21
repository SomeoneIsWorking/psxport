// engine/entity.h — PC-native per-object ENTITY STATE-MACHINE subsystem.
// The per-object behavior cluster: child-node spawn / sub-object builder, the object dispatcher loop,
// the state-machine head, and the oscillate / frame-toggle sub-behavior. Extracted from game_tomba2.cpp
// into its own module (PC-game code structure); registered in game_tomba2.cpp's init block by these names.
#ifndef ENGINE_ENTITY_H
#define ENGINE_ENTITY_H
struct Core;
void ov_child_spawn_40410(Core* c);   // FUN_80040410 — per-object child-node spawn / sub-object builder
void ov_disp_26c88(Core* c);          // FUN_80026C88 — per-object dispatcher loop over the object table
void ov_sm40558(Core* c);             // FUN_80040558 — per-object state-machine head
void ov_osc_fd10(Core* c);            // FUN_8003FD10 — per-object oscillate / frame-toggle sub-behavior
#endif
