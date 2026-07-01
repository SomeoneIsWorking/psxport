// engine/script.h — PC-native per-object SCRIPT-VM subsystem.
// The per-object script-VM tick (FUN_8004CE14) — the most-called field function. Extracted from
// game_tomba2.cpp into its own module (PC-game code structure); registered in game_tomba2.cpp's
// init block by this name.
#ifndef ENGINE_SCRIPT_H
#define ENGINE_SCRIPT_H
struct Core;
void ov_script_vm_4ce14(Core* c);   // FUN_8004CE14 — per-object script-VM tick
#endif
