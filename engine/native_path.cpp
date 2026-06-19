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

// ─── Clean-leaf batch (call-free init helpers) ──────────────────────────────────────────────────
// These call nothing (no jal/jalr/BIOS), so the native body is the whole function — porting one
// removes it from the interpreter immediately with no ordering dependency. Each is RE'd from its
// gen_func_<addr> body (the recompiler decode is the reference; the trailing duplicate `return;` and
// any post-`jr ra` register write in those bodies are the recompiler over-running into the next
// function and are not part of the real body — see ov_80089788).

// 0x800861BC / 0x80086320 / 0x800865C8 — three byte-identical word-fillers: zero `a1` 32-bit words
// starting at `a0` (`for (i=a1; i--;) *p++ = 0`). A bzero-init for object/OT tables.
static void zfill_words(Core* c) {
  uint32_t p = c->r[4], n = c->r[5];
  for (uint32_t i = 0; i < n; i++) { c->mem_w32(p, 0); p += 4; }
}

// 0x80083AF8 — memset: write byte (uint8_t)a1 to `a2` bytes at `a0`.
static void ov_80083AF8(Core* c) {
  uint32_t p = c->r[4], n = c->r[6]; uint8_t v = (uint8_t)c->r[5];
  for (uint32_t i = 0; i < n; i++) c->mem_w8(p + i, v);
}

// 0x8009A480 — store a0 to the global word at 0x80105EE8 (0x8010<<16 + 0x5EE8).
static void ov_8009A480(Core* c) { c->mem_w32(0x80105EE8u, c->r[4]); }

// 0x80080890 — EnterCriticalSection: `a0=1; syscall`. Routed through rec_syscall so the shared
// IRQ-enable state (s_irq_enabled in hle.cpp) stays consistent with the rest of the BIOS HLE.
static void ov_80080890(Core* c) { c->r[4] = 1; rec_syscall(c, 0); }

// 0x80086604 / 0x800865F0 — getter/setter for the global word at 0x800ABE20 (0x800B<<16 - 0x41E0).
// Getter returns it; setter returns the OLD value then writes a0 (read-before-write order matters).
static void ov_80086604(Core* c) { c->r[2] = c->mem_r32(0x800ABE20u); }
static void ov_800865F0(Core* c) { c->r[2] = c->mem_r32(0x800ABE20u); c->mem_w32(0x800ABE20u, c->r[4]); }

// 0x80083BF0 — initialize a 20-byte descriptor at a0 from (a1,a2,a3) and a stack-passed 5th arg.
// Layout: u16[0]=a1, u16[2]=a2, u16[4]=a3, u16[6]=arg5, u16[8..14]=0, u8[16..19]=0. arg5 is read
// from the caller's o32 frame at sp+16 (the fn has no prologue, so sp is the caller's).
static void ov_80083BF0(Core* c) {
  uint32_t p = c->r[4], arg5 = c->mem_r32(c->r[29] + 16);
  c->mem_w16(p + 0, (uint16_t)c->r[5]); c->mem_w16(p + 2, (uint16_t)c->r[6]);
  c->mem_w16(p + 4, (uint16_t)c->r[7]); c->mem_w16(p + 6, (uint16_t)arg5);
  c->mem_w16(p + 8, 0); c->mem_w16(p + 10, 0); c->mem_w16(p + 12, 0); c->mem_w16(p + 14, 0);
  c->mem_w8(p + 16, 0); c->mem_w8(p + 17, 0); c->mem_w8(p + 18, 0); c->mem_w8(p + 19, 0);
}

// 0x80051794 — init an 8-word block at a0 to {0x1000,0,0x1000,0,0x1000,0,0,0} (three 0x1000 stride
// fields with zeroed counters — a scratch/free-list header).
static void ov_80051794(Core* c) {
  uint32_t p = c->r[4];
  c->mem_w32(p + 0, 0x1000); c->mem_w32(p + 4, 0);  c->mem_w32(p + 8, 0x1000); c->mem_w32(p + 12, 0);
  c->mem_w32(p + 16, 0x1000); c->mem_w32(p + 20, 0); c->mem_w32(p + 24, 0);    c->mem_w32(p + 28, 0);
}

// 0x800963A0 — bounded register: if ((a0-1)&0xff) < 24, store (uint8_t)a0 to the global at
// 0x80105CEC and return a0 sign-extended from its low byte; else return -1.
static void ov_800963A0(Core* c) {
  uint32_t a0 = c->r[4];
  if ((uint32_t)((a0 - 1) & 0xff) < 24) { c->mem_w8(0x80105CECu, (uint8_t)a0); c->r[2] = (uint32_t)(int32_t)(int8_t)a0; }
  else c->r[2] = (uint32_t)-1;
}

// 0x80096370 — store (uint8_t)a0 to the global byte at 0x80105D28.
static void ov_80096370(Core* c) { c->mem_w8(0x80105D28u, (uint8_t)c->r[4]); }

// Register every hand-native boot→cutscene function. Called from games_tomba2_init at startup, before
// ov_game_main runs the init prefix, so rec_dispatch routes these addresses to the native C++ bodies.
void games_native_path_init(void) {
  rec_set_override(0x80089788u, ov_80089788);
  rec_set_override(0x800861BCu, zfill_words);
  rec_set_override(0x80086320u, zfill_words);
  rec_set_override(0x800865C8u, zfill_words);
  rec_set_override(0x80083AF8u, ov_80083AF8);
  rec_set_override(0x8009A480u, ov_8009A480);
  rec_set_override(0x80080890u, ov_80080890);
  rec_set_override(0x80086604u, ov_80086604);
  rec_set_override(0x800865F0u, ov_800865F0);
  rec_set_override(0x80083BF0u, ov_80083BF0);
  rec_set_override(0x80051794u, ov_80051794);
  rec_set_override(0x800963A0u, ov_800963A0);
  rec_set_override(0x80096370u, ov_80096370);
}
