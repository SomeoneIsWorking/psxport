// engine/animation.h — PC-native per-object ANIMATION-VM subsystem.
// The per-object animation-sequence VM stepper (FUN_80076D68). Extracted from game_tomba2.cpp into its
// own module (PC-game code structure); registered in game_tomba2.cpp's init block by this name.
#ifndef ENGINE_ANIMATION_H
#define ENGINE_ANIMATION_H
struct Core;
void ov_anim_vm_76d68(Core* c);   // FUN_80076D68 — per-object animation-sequence VM stepper
#endif
