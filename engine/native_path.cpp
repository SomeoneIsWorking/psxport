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

// 0x80096390 — store u16 0 to the global at 0x80105CD8.
static void ov_80096390(Core* c) { c->mem_w16(0x80105CD8u, 0); }

// 0x8006D950 — *0x1F8000E0 (scratchpad) = *(a1+4).
static void ov_8006D950(Core* c) { c->mem_w32(0x1F8000E0u, c->mem_r32(c->r[5] + 4)); }

// 0x8006D934 — *0x1F8000DC = *(a1+0); *0x1F8000E4 = *(a1+8) (scratchpad).
static void ov_8006D934(Core* c) {
  c->mem_w32(0x1F8000DCu, c->mem_r32(c->r[5] + 0));
  c->mem_w32(0x1F8000E4u, c->mem_r32(c->r[5] + 8));
}

// 0x80089B98 — get/set the global word at 0x800ABFC0 (returns old, writes a0).
static void ov_80089B98(Core* c) { c->r[2] = c->mem_r32(0x800ABFC0u); c->mem_w32(0x800ABFC0u, c->r[4]); }

// 0x80099478 — return (*0x800AC638 ^ 1) != 0  (i.e. *0x800AC638 != 1).
static void ov_80099478(Core* c) { c->r[2] = ((c->mem_r32(0x800AC638u) ^ 1u) != 0u) ? 1u : 0u; }

// 0x80082370 — libgpu primitive/tpage word: 0xE5000000 | ((a1&2047)<<11) | (a0&2047).
static void ov_80082370(Core* c) { c->r[2] = 0xE5000000u | ((c->r[5] & 2047u) << 11) | (c->r[4] & 2047u); }

// 0x80082220 — GPU draw-mode word: base 0xE1000000, |512 if a1!=0, plus (a2&2559)|1024 if a0!=0.
static void ov_80082220(Core* c) {
  uint32_t hi = 0xE1000000u | (c->r[5] != 0 ? 512u : 0u);
  uint32_t lo = (c->r[6] & 2559u) | (c->r[4] != 0 ? 1024u : 0u);
  c->r[2] = hi | lo;
}

// 0x8009A1D0 — table read: entry = *0x800AC604 + (a0<<4); *(u16*)a1 = *(u16*)(entry+12).
static void ov_8009A1D0(Core* c) {
  uint32_t e = c->mem_r32(0x800AC604u) + (c->r[4] << 4);
  c->mem_w16(c->r[5], c->mem_r16(e + 12));
}

// 0x80097678 — read-modify-write the word at *0x800AC618: v = (v & 0xF0FFFFFF) | 0x02000000.
static void ov_80097678(Core* c) {
  uint32_t p = c->mem_r32(0x800AC618u);
  c->mem_w32(p, (c->mem_r32(p) & 0xF0FFFFFFu) | 0x02000000u);
}

// 0x80099450 — *0x800AC638 = (a0 == 1) ? 0 : 1.
static void ov_80099450(Core* c) { c->mem_w32(0x800AC638u, (c->r[4] == 1u) ? 0u : 1u); }

// 0x80099370 — *0x800AC594 = a0; *0x800AC620 = (a0 == 1) ? 1 : 0.
static void ov_80099370(Core* c) {
  c->mem_w32(0x800AC594u, c->r[4]);
  c->mem_w32(0x800AC620u, (c->r[4] == 1u) ? 1u : 0u);
}

// 0x8009A420 — memset(a0, (uint8_t)a1, a2) returning a0; returns 0 when a0==0 or (int)a2<=0.
static void ov_8009A420(Core* c) {
  uint32_t p = c->r[4]; int n = (int32_t)c->r[6];
  if (p == 0 || n <= 0) { c->r[2] = 0; return; }
  uint8_t v = (uint8_t)c->r[5]; c->r[2] = p;
  for (int i = 0; i < n; i++) c->mem_w8(p + i, v);
}

// 0x8009A3E0 — memcpy(a0, a1, a2) returning a0 (0 if a0==0); no-op body when (int)a2<=0.
static void ov_8009A3E0(Core* c) {
  uint32_t d = c->r[4], s = c->r[5]; int n = (int32_t)c->r[6];
  if (d == 0) { c->r[2] = 0; return; }
  c->r[2] = d; if (n <= 0) return;
  for (int i = 0; i < n; i++) c->mem_w8(d + i, c->mem_r8(s + i));
}

// 0x8009A450 — rand(): seed at 0x80105EE8; seed = seed*0x41C64E6D + 12345; return (seed>>16)&0x7FFF.
// (0x8009A480, already ported, is the matching srand that writes this seed.)
static void ov_8009A450(Core* c) {
  uint32_t s = c->mem_r32(0x80105EE8u) * 0x41C64E6Du + 12345u;
  c->mem_w32(0x80105EE8u, s);
  c->r[2] = (s >> 16) & 0x7FFFu;
}

// 0x8009C1FC — copy a 28-word table from 0x8009C060 to low RAM 0xDF80 (installs a fixed block).
static void ov_8009C1FC(Core* c) {
  uint32_t s = 0x8009C060u, d = 0xDF80u;
  for (int i = 0; i < 28; i++) { c->mem_w32(d, c->mem_r32(s)); s += 4; d += 4; }
}

// 0x8008CFF0 — zero the first word of `a1` stride-32 records starting at index a0 in the table at
// *0x80102728: for (i=0; i<a1; i++) *(base + (a0+i)*32) = 0.
static void ov_8008CFF0(Core* c) {
  uint32_t a0 = c->r[4], n = c->r[5]; if (n == 0) return;
  uint32_t base = c->mem_r32(0x80102728u);
  for (uint32_t i = 0; i < n; i++) c->mem_w32(base + ((a0 + i) << 5), 0);
}

// 0x8006CBD0 — scatter 6 u16 fields from a1 into the scratchpad block at 0x1F8000D0 (+2,+6,+10)
// and into the object at a0 (+58,+62,+66).
static void ov_8006CBD0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  c->mem_w16(0x1F8000D2u, c->mem_r16(a1 + 0));
  c->mem_w16(0x1F8000D6u, c->mem_r16(a1 + 2));
  c->mem_w16(0x1F8000DAu, c->mem_r16(a1 + 4));
  c->mem_w16(a0 + 58, c->mem_r16(a1 + 6));
  c->mem_w16(a0 + 62, c->mem_r16(a1 + 8));
  c->mem_w16(a0 + 66, c->mem_r16(a1 + 10));
}

// 0x800847B0 — reformat a {int,int} pair pack from a0 into a1: each 32-bit field is stored as a word
// and then its low half-word is overwritten with a 16-bit value (a fixed-point pack). Order preserved
// from the recompiled body (the w16 fixups intentionally follow the w32 stores).
static void ov_800847B0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t r9 = c->mem_r32(a0 + 0), r10 = c->mem_r32(a0 + 4);
  c->mem_w32(a1 + 4, r9); c->mem_w32(a1 + 0, r10); c->mem_w16(a1 + 0, (uint16_t)r9);
  uint32_t r11 = c->mem_r32(a0 + 8), r9b = c->mem_r32(a0 + 12);
  c->mem_w32(a1 + 12, r11); c->mem_w32(a1 + 8, r9b);
  c->mem_w16(a1 + 12, (uint16_t)r10); c->mem_w16(a1 + 8, (uint16_t)r11);
  int16_t r10b = (int16_t)c->mem_r16(a0 + 16);
  c->mem_w16(a1 + 4, (uint16_t)r9b); c->mem_w16(a1 + 16, (uint16_t)r10b);
}

// 0x800974FC — write into the u16 table at *0x800AC604, index a0: if a2==0 store a1, else store
// a1 >> (*0x800AC62C & 31).
static void ov_800974FC(Core* c) {
  uint32_t p = c->mem_r32(0x800AC604u) + (c->r[4] << 1);
  if (c->r[6] == 0) c->mem_w16(p, (uint16_t)c->r[5]);
  else c->mem_w16(p, (uint16_t)(c->r[5] >> (c->mem_r32(0x800AC62Cu) & 31)));
}

// 0x80082C68 — write four GPU primitive templates through the pointer table at 0x800A5AA8..0x800A5AB4:
// [0]=0x04000002, [1]=a0, [2]=0, [3]=0x01000401 (draw-mode / tex-window / etc. setup).
static void ov_80082C68(Core* c) {
  c->mem_w32(c->mem_r32(0x800A5AA8u), 0x04000002u);
  c->mem_w32(c->mem_r32(0x800A5AACu), c->r[4]);
  c->mem_w32(c->mem_r32(0x800A5AB0u), 0);
  c->mem_w32(c->mem_r32(0x800A5AB4u), 0x01000401u);
}

// 0x800976C8 — original spins x=13; x*=13 sixty times into a stack local with NO external effect
// (no global/pointer write), returning 0. The native form drops the dead loop. (r2 = 0 on exit.)
static void ov_800976C8(Core* c) { c->r[2] = 0; }

// 0x80097760 — if (int)a0 <= 0 return 0; else set up a request block at a1 and register it in the
// globals at 0x800AC664/668/66C. *(a1)=0x40001010; *(a1+4)=(0x10000<<(*0x800AC62C&31))-0x1010.
static void ov_80097760(Core* c) {
  if ((int32_t)c->r[4] <= 0) { c->r[2] = 0; return; }
  uint32_t sh = c->mem_r32(0x800AC62Cu) & 31;
  c->mem_w32(c->r[5] + 0, 0x40001010u);
  c->mem_w32(0x800AC66Cu, c->r[5]);
  c->mem_w32(0x800AC668u, 0);
  c->mem_w32(0x800AC664u, c->r[4]);
  c->mem_w32(c->r[5] + 4, (0x00010000u << sh) - 4112u);
  c->r[2] = c->r[4];
}

// 0x8009A340 — bzero(a0, a1) returning a0; returns 0 when a0==0 or (int)a1<=0.
static void ov_8009A340(Core* c) {
  uint32_t p = c->r[4]; int n = (int32_t)c->r[5];
  if (p == 0 || n <= 0) { c->r[2] = 0; return; }
  c->r[2] = p; for (int i = 0; i < n; i++) c->mem_w8(p + i, 0);
}

// 0x8009A540 — strcmp(a0, a1). Guard: if either is NULL, equal→0 / a0==NULL→-1 / else 1. Otherwise
// walk both; the s2 pointer advances every iteration (a delay-slot store), so the mismatch return is
// *s1 - *s2 (sign-extended bytes); both-NUL → 0.
static void ov_8009A540(Core* c) {
  uint32_t s1 = c->r[4], s2 = c->r[5];
  if (s1 == 0 || s2 == 0) {
    if (s1 == s2) c->r[2] = 0;
    else if (s1 == 0) c->r[2] = (uint32_t)-1;
    else c->r[2] = 1;
    return;
  }
  for (;;) {
    int32_t a = (int8_t)c->mem_r8(s1), b = (int8_t)c->mem_r8(s2);
    uint32_t cur = c->mem_r8(s1);
    s2++;                               // always (delay slot)
    if (a == b) { s1++; if (cur == 0) { c->r[2] = 0; return; } continue; }
    int32_t x = (int8_t)c->mem_r8(s1), y = (int8_t)c->mem_r8(s2 - 1);
    c->r[2] = (uint32_t)(x - y); return;
  }
}

// 0x8001CBA8 — write a {0,252,0,255} 4-byte pattern at offsets 72/74/76/78 of a0+(a1&0xff), for up
// to two entries (the start index folds a1's low byte; 255 wraps to 0). A palette/attr seed.
static void ov_8001CBA8(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t k = ((a1 & 0xff) == 255) ? 0u : 1u;
  if ((a1 & 0xff) == 255) a1 = 0;
  for (;;) {
    k++;
    uint32_t p = a0 + (a1 & 0xff);
    c->mem_w8(p + 72, 0); c->mem_w8(p + 74, 252); c->mem_w8(p + 76, 0); c->mem_w8(p + 78, 255);
    if ((int32_t)k < 2) a1++; else break;
  }
}

// 0x8007B2C0 — load a 4-entry u16 weight ramp into the scratchpad at 0x1F800170: a0==0 → descending
// {0x8000,0x4000,0x2000,0x1000}; a0!=0 → ascending {0x1000,0x2000,0x4000,0x8000}.
static void ov_8007B2C0(Core* c) {
  uint32_t b = 0x1F800170u;
  if (c->r[4] == 0) { c->mem_w16(b+0,0x8000); c->mem_w16(b+2,0x4000); c->mem_w16(b+4,0x2000); c->mem_w16(b+6,0x1000); }
  else              { c->mem_w16(b+0,0x1000); c->mem_w16(b+2,0x2000); c->mem_w16(b+4,0x4000); c->mem_w16(b+6,0x8000); }
}

// Additional hand-native leaf batches live in sibling files (native_path_aN.cpp) to allow parallel
// authoring; each exposes a register fn wired in below.
void games_native_path_a1_init(void);
void games_native_path_a2_init(void);
void games_native_path_a3_init(void);
void games_native_path_b1_init(void);
void games_native_path_b2_init(void);
void games_native_path_b3_init(void);
void games_native_path_b4_init(void);
void games_native_path_b5_init(void);

// Register every hand-native boot→cutscene function. Called from games_tomba2_init at startup, before
// ov_game_main runs the init prefix, so rec_dispatch routes these addresses to the native C++ bodies.
void games_native_path_init(void) {
  games_native_path_a1_init();
  games_native_path_a2_init();
  games_native_path_a3_init();
  games_native_path_b1_init();
  games_native_path_b2_init();
  games_native_path_b3_init();
  games_native_path_b4_init();
  games_native_path_b5_init();
}
