// game/player/hitbox.cpp — PC-native ownership of FUN_8003B220.
//
// FUN_8003B220 — per-object 2D BOX / hitbox-corner BUILDER (a pure resident leaf; ~1.64% of the seaside
// field's sampled interpreter time, the hottest still-recomp resident CONTENT leaf that is NOT a
// render-boundary fn). 64 instructions, ZERO jal, ZERO GTE, ZERO render packets — it only reads bytes
// from the a2 parameter block and writes halfwords into the a0 output struct, so it is verifiable by a
// full main-RAM + scratchpad 0-diff A/B gate against the recomp body (`boxverify`).
//
// Signature (from the disasm): void FUN_8003B220(void* a0 = dst, int32_t a1 = base value, void* a2 = params)
//
// Semantics (traced instruction-by-instruction from MAIN.EXE 0x8003b220; all dst fields are 16-bit
// halfwords unless noted, all a2 reads are bytes):
//   M32[a0+0]  = a1                                 ; sw a1,0(a0)  (full word; low half re-read below)
//   a0[0]      = (u16)(a0+0 low half) + (s8)a2[14]  ; X origin += signed dx
//   a0[2]      = a0[2]              + (s8)a2[15]     ; Y origin += signed dy
//   a0[10]     = a0[2] (== the same just-computed value)
//   a0[16]     = a0[0]                               ; (snapshot of X origin before the ±a2[10] spread)
//   a0[8]      = a0[0] + (u8)a2[10]                  ; X far corner
//   a0[4]=a0[12]=a0[20]=a0[28] = 0                   ; zero 4 slots
//   a0[0]      = a0[0] * 5                            ; (X origin) *5
//   a0[16]     = a0[16] * 5                           ; (X snapshot) *5
//   a0[18]     = a0[2] + (u8)a2[11]                   ; Y far corner
//   a0[24]     = a0[8]                                ; snapshot of X far corner
//   a0[8]      = a0[8] * 5                            ; X far corner *5
//   a0[26]     = a0[18]                               ; snapshot of Y far corner
//   a0[24]     = a0[24] * 5                           ; X far corner snapshot *5
//   a0[2]      = a0[2] * 5                            ; Y origin *5
//   a0[10]     = a0[10] * 5                           ; Y origin copy *5
//   a0[18]     = a0[18] * 5                           ; Y far corner *5
//   a0[26]     = a0[26] * 5                           ; Y far corner snapshot *5
// (the "*5" comes from `sll v0,x,2; addu v0,v0,x` = x*4 + x). Every read uses the value LIVE in memory at
// that point (the recomp re-loads each field), so order matters — this mirrors the exact load/store order.
//
// VERIFY: `boxverify` REPL channel = full main-RAM (0x200000) + full scratchpad (0x400) + v0 A/B vs
// rec_super_call(0x8003B220). The fn touches NO scratchpad and has no callees, so no stack-exclusion
// window is needed (its own 0-byte frame: it never adjusts sp). Native run -> snapshot+rollback ->
// rec_super_call -> diff.

#include "core.h"
#include "cfg.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

void rec_super_call(Core*, uint32_t);

static inline int16_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }

// Pure native body. Mirrors the recomp's exact in-memory load/store order so the result is byte-exact.
static void hitbox_build_3b220(Core* c) {
  const uint32_t a0 = c->r[4];
  const uint32_t a1 = c->r[5];
  const uint32_t a2 = c->r[6];

  c->mem_w32(a0 + 0, a1);                                          // sw a1,0(a0)

  // X origin += (s8)a2[14]
  int32_t dx = c->mem_r8s(a2 + 14);
  uint16_t x0 = (uint16_t)((uint16_t)c->mem_r16(a0 + 0) + (uint32_t)dx);
  c->mem_w16(a0 + 0, x0);

  // Y origin += (s8)a2[15]; copy into a0[10]
  int32_t dy = c->mem_r8s(a2 + 15);
  uint16_t y0 = (uint16_t)((uint16_t)c->mem_r16(a0 + 2) + (uint32_t)dy);
  uint16_t xnow = (uint16_t)c->mem_r16(a0 + 0);                    // lhu v0,0(a0) (== x0)
  c->mem_w16(a0 + 2, y0);
  c->mem_w16(a0 + 10, y0);                                         // sh v1,10(a0)
  c->mem_w16(a0 + 16, xnow);                                       // sh v0,16(a0)

  uint32_t w = c->mem_r8(a2 + 10);                                 // lbu a1,10(a2)
  c->mem_w16(a0 + 8, (uint16_t)(xnow + w));                        // a0[8] = X far = x0 + a2[10]

  uint32_t h = c->mem_r8(a2 + 11);                                 // lbu a3,11(a2)
  int16_t la1 = s16(c->mem_r16(a0 + 0));                           // lh a1,0(a0)   (x0)
  int16_t lv1 = s16(c->mem_r16(a0 + 16));                          // lh v1,16(a0)  (x snapshot)
  int16_t la2 = s16(c->mem_r16(a0 + 8));                           // lh a2,8(a0)   (X far)

  c->mem_w16(a0 + 28, 0);
  c->mem_w16(a0 + 20, 0);
  c->mem_w16(a0 + 12, 0);
  c->mem_w16(a0 + 4, 0);

  c->mem_w16(a0 + 0, (uint16_t)((int32_t)la1 * 5));                // a0[0] = x0*5
  c->mem_w16(a0 + 16, (uint16_t)((int32_t)lv1 * 5));               // a0[16] = xsnap*5

  c->mem_w16(a0 + 18, (uint16_t)((uint16_t)c->mem_r16(a0 + 2) + h)); // a0[18] = Y far = y0 + a2[11]

  uint16_t xfar = (uint16_t)c->mem_r16(a0 + 8);                    // lhu v1,8(a0)  (X far)
  c->mem_w16(a0 + 24, xfar);                                       // a0[24] = X far snapshot
  c->mem_w16(a0 + 8, (uint16_t)((int32_t)la2 * 5));                // a0[8] = X far*5

  uint16_t yfar = (uint16_t)c->mem_r16(a0 + 18);                   // lhu v1,18(a0) (Y far)
  int16_t lxfs = s16(c->mem_r16(a0 + 24));                         // lh a1,24(a0)  (X far snapshot)
  c->mem_w16(a0 + 26, yfar);                                       // a0[26] = Y far snapshot
  int16_t ly0 = s16(c->mem_r16(a0 + 2));                           // lh v1,2(a0)   (y0)
  c->mem_w16(a0 + 24, (uint16_t)((int32_t)lxfs * 5));              // a0[24] = X far snap*5
  c->mem_w16(a0 + 2, (uint16_t)((int32_t)ly0 * 5));                // a0[2] = y0*5

  int16_t ly10 = s16(c->mem_r16(a0 + 10));                         // lh v1,10(a0)  (y0 copy)
  int16_t lyf  = s16(c->mem_r16(a0 + 18));                         // lh a1,18(a0)  (Y far)
  c->mem_w16(a0 + 10, (uint16_t)((int32_t)ly10 * 5));              // a0[10] = y0copy*5
  int16_t lyfs = s16(c->mem_r16(a0 + 26));                         // lh v1,26(a0)  (Y far snapshot)
  c->mem_w16(a0 + 18, (uint16_t)((int32_t)lyf * 5));               // a0[18] = Y far*5
  int32_t last = (int32_t)lyfs * 5;
  c->mem_w16(a0 + 26, (uint16_t)last);                            // a0[26] = Y far snap*5

  // The gen body returns v0 = the last computed value (the `sh v0,26(a0)` delay-slot store: v0 =
  // sll(v1,2)+v1 = lyfs*5, register-width). This fn is void and callers ignore v0, but mirror it so the
  // `boxverify` gate is byte-exact incl. the return register.
  c->r[2] = (uint32_t)last;
}
