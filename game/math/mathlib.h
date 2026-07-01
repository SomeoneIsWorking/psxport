// engine/mathlib.h — PC-native MATH/PRNG leaf primitives subsystem.
// Extracted from game_tomba2.cpp into its own module (PC-game code structure). Registered in
// game_tomba2.cpp's init block by these names. ov_rand is also exposed so other engine modules
// (e.g. entity.cpp's oscillate handler) can reach the native PRNG if they need it directly.
#ifndef ENGINE_MATHLIB_H
#define ENGINE_MATHLIB_H
struct Core;
void ov_rand(Core* c);            // FUN_8009A450 — platform PRNG (glibc LCG); prefer c->rng.next()
void ov_bittest_4d7ec(Core* c);   // FUN_8004D7EC — pure bitmap bit-test
void ov_bittest_4d868(Core* c);   // FUN_8004D868 — sibling bitmap bit-test @0x800BFDB4
// FUN_80083E80 sin / FUN_80083F50 cos: use Trig::rsin(c, x) / Trig::rcos(c, x) directly
// (game/math/trig.h). The old ov_trig_sin / ov_trig_cos / ov_trig_lut orphan override handlers
// were removed 2026-07-02; callers migrated to the class.
#endif
