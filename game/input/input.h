// engine/input.h — PC-native per-frame INPUT/controller subsystem.
// The per-frame input / controller-state processor (FUN_800931C0). Extracted from game_tomba2.cpp into
// its own module (PC-game code structure); registered in game_tomba2.cpp's init block by this name.
#ifndef ENGINE_INPUT_H
#define ENGINE_INPUT_H
struct Core;
void ov_input_dispatch_931c0(Core* c);   // FUN_800931C0 — per-frame input/controller-state processor
#endif
