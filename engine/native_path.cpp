// Hand-written native C++ for the boot → first-cutscene path (Tomba2Engine).
//
// DIRECTION (user, 2026-06-19): the entire boot→first-cutscene path must be HAND-WRITTEN NATIVE C++ —
// not interpreted, not machine-recompiled. C++ is the spine: these functions ARE the engine, reverse-
// engineered from the MIPS (the recompiled generated/gen_func_* bodies + disassembly are the RE
// reference only). Each function is registered as a native override so the C++ spine runs instead of
// the interpreter; un-ported functions interpret temporarily, and the burn-down (PSXPORT_INTERP_FUNCS)
// drops to 0 as coverage grows. See docs/native-port-plan.md.
//
// Ported top-down from boot. Each entry documents the source addr + what it does + the RE that justifies
// it (no blind c->r[] transcription — real logic). Gameplay beyond the cutscene is out of scope.
#include "core.h"
#include <stdint.h>

// 0x80089788 — C++ global-constructors runner (crt0 __main). A one-shot guard: if the "ctors already
// run" flag at 0x800BBEF0 is set, return; else set it and call each ctor in the table at 0x80010000.
// In this build the ctor count is 0 (MIPS: `lui s1,0; addiu s1,0` → 0; the loop is skipped), so it
// only arms the guard. (gen_func_80089788's trailing duplicate is the recompiler over-running past
// `jr ra` into the next function — not part of this body.)
#define CTORS_GUARD 0x800BBEF0u
static void ov_80089788(Core* c) {
  if (c->mem_r32(CTORS_GUARD) != 0) return;   // already run
  c->mem_w32(CTORS_GUARD, 1);
  // ctor table @0x80010000, count 0 → nothing to run.
}

// Register every hand-native boot→cutscene function. Called from games_tomba2_init at startup, before
// ov_game_main runs the init prefix, so rec_dispatch routes these addresses to the native C++ bodies.
void games_native_path_init(void) {
  rec_set_override(0x80089788u, ov_80089788);
}
