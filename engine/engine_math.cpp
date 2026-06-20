// engine_math.cpp — PC-native reimplementations of the hot libgte-style MATH helpers the engine
// calls every frame. The port is interpreter-only, so each of these pure leaf routines runs as
// interpreted MIPS on the hot path; owning them native is the #1-priority lever (perf + 100%-PC-
// native — they align). These are deterministic pure-integer/math leaves with NO PSX intricacy, so
// the PC-native form is just the same math in C; bit-exactness with the recomp reference IS the
// correctness gate (the result feeds cull distance / camera / content), proven by the per-call
// `mathverify` comparator below and then registered unconditionally.
//
// Profiled hot-list (field, later-186, docs/port-progress.md §F): FUN_80077FB0 isqrt = 8.41% of all
// interpreter instructions (and a frequency leader). First port here.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <string.h>

void rec_set_override(uint32_t addr, void (*fn)(Core*));
void rec_interp(Core* c, uint32_t pc);

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80077FB0 — 16-bit ROUNDING integer square root. a0 = unsigned value, returns v0 = nearest
// integer r in [0, 0xffff] with r ≈ sqrt(a0). Algorithm (transcribed from the asm, exactly):
//   - high-bit seed: if a0 > 0x3fffffff the top result bit (0x8000) is pre-set via hi=0xffff8000.
//   - binary search bits 0x4000..0x1: accept candidate c when a0 >= (c & 0xffff)^2 (low-32 mult).
//   - final round-up: with r = result & 0xffff (unless r==0xffff), add 1 iff r^2 + r < a0
//     (i.e. a0 > r·(r+1) ⇒ a0 is closer to (r+1)^2). Return r & 0xffff.
// Pure leaf, no memory/GTE — bit-exact by construction; the comparator confirms on live calls.
static uint32_t isqrt16(uint32_t a0) {
  uint32_t hi = (a0 > 0x3fffffffu) ? 0xffff8000u : 0u;   // v0 seed (top bit)
  uint32_t a1 = hi;                                       // result accumulator
  // first step: bit 0x4000, candidate masked with 0xc000 (lower bits are 0 here)
  uint32_t a2 = hi + 0x4000u;
  {
    uint32_t v1 = a2 & 0xc000u;
    uint32_t a3 = (v1 * v1) & 0xffffffffu;
    if (!(a0 < a3)) a1 = a2;                              // accept when a0 >= candidate^2
  }
  // generic steps: bits 0x2000 .. 0x1, candidate masked with 0xffff
  for (uint32_t bit = 0x2000u; bit != 0u; bit >>= 1) {
    uint32_t v1 = a1 + bit;
    uint32_t v0 = v1 & 0xffffu;
    uint32_t a3 = (v0 * v0) & 0xffffffffu;
    if (!(a0 < a3)) a1 = v1;                              // accept
  }
  // final rounding
  uint32_t r = a1 & 0xffffu;
  if (r != 0xffffu) {
    uint32_t a3 = (r * r) & 0xffffffffu;
    uint32_t s  = (a3 + r) & 0xffffffffu;
    if (s < a0) a1 += 1;                                  // round up toward (r+1)
  }
  return a1 & 0xffffu;
}

static void ov_isqrt(Core* c) { c->r[2] = isqrt16(c->r[4]); }

// PSXPORT_DEBUG=mathverify — per-call A/B gate: run native, save v0, restore regs, run the recomp
// reference, compare. Pure leaf so only v0 (and clobbered caller-saved regs, which the caller
// reloads) matters; we restore the full reg file before the reference so both see identical input.
static void ov_isqrt_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t a0 = c->r[4];
  ov_isqrt(c);
  uint32_t mine = c->r[2];
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x80077FB0u);
  uint32_t oracle = c->r[2];
  static int nbad = 0, ngood = 0;
  if (mine != oracle) {
    if (nbad++ < 60) fprintf(stderr, "[mathverify] isqrt MISMATCH a0=%08x mine=%08x oracle=%08x\n", a0, mine, oracle);
  } else if ((ngood++ % 5000) == 0) {
    fprintf(stderr, "[mathverify] isqrt match #%d (a0=%08x -> %04x)\n", ngood, a0, oracle);
  }
  c->r[2] = mine;  // keep the native result as the live value
}

void engine_math_register(void) {
  // Verified bit-exact: 65000+ live field calls 0-diff vs the recomp reference (later-186). ov_isqrt is
  // the live path; ov_isqrt_verify is reachable as the per-call gate when the `mathverify` channel is set
  // before override install (same convention as camverify).
  rec_set_override(0x80077FB0u, cfg_dbg("mathverify") ? ov_isqrt_verify : ov_isqrt);
}
