// engine/mathlib.h — PC-native MATH/PRNG leaf primitives subsystem.
// Extracted from game_tomba2.cpp into its own module (PC-game code structure). Registered in
// game_tomba2.cpp's init block by these names. ov_rand is also exposed so other engine modules
// (e.g. entity.cpp's oscillate handler) can reach the native PRNG if they need it directly.
#ifndef ENGINE_MATHLIB_H
#define ENGINE_MATHLIB_H
struct Core;
void ov_rand(Core* c);            // FUN_8009A450 — platform PRNG (glibc LCG)
void ov_trig_sin(Core* c);        // FUN_80083E80 — sin LUT
void ov_trig_cos(Core* c);        // FUN_80083F50 — cos LUT
void ov_trig_lut(Core* c);        // FUN_80083EBC — sin-quadrant lookup
void ov_bittest_4d7ec(Core* c);   // FUN_8004D7EC — pure bitmap bit-test
#endif
