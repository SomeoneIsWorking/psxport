// game/player/grid_offset.cpp — PC-native collision-grid CELL-RELATIVE OFFSET transform.
//
// FUN_80048360 — a hot collision-grid LEAF (~5% of field interp time across its internal jump-table
// entries 0x80048360/410/594/630; ~14400 calls/run). No args, void return; returns t3 (the cell
// record's low byte @rec[7]) in v0. Reads/writes the collision scratchpad ONLY (base 0x1F800000) —
// NO GTE, NO render packets, NO callees. Sibling of the owned grid family (engine/collision.cpp):
// it maps the probe's raw position into a cell-local 6-bit offset, then applies the cell's tag-encoded
// orientation (mirror / axis-swap / slope-shear / sign) so the caller sees the probe in canonical
// cell-local space, and finally re-accumulates the transformed offset back onto the working coords.
//
// Scratchpad fields (base SP = 0x1F800000):
//   sh[0x1AA],sh[0x1AC] = grid origin (X,Z)
//   sh[0x1BC],sh[0x1C0] = working probe coords (X,Z)  [updated at the tail]
//   sh[0x1C2]           = dx  (X offset inside the 64-unit cell, transformed)
//   sh[0x1C6]           = dz  (Z offset inside the 64-unit cell, transformed)
//   w [0x1E0]           = cell record pointer (latched by the grid query FUN_80047CBC)
//   rec[0] = tag (u16); rec[6] = slope param (u16, hi=a2 / lo=a3==t3)
//
// Tag bit usage (TAG = rec[0]):
//   TAG & 3  : cell orientation quadrant — drives the pre-mirror (^0x3F), the post sign-negate, and
//              the post re-mirror (^0x3F) of (dx,dz).
//   TAG & 4  : swap the X/Z offsets (around the slope step, applied twice — i.e. transpose).
//   TAG & 8  : slope mode select (sheared X-from-Z vs the divided Z-from-X form).
//
// `gridoffset` REPL channel = full main-RAM + scratchpad A/B vs rec_super_call (no dispatched callees,
// so a pure 0-diff over both regions + v0 is the exact gate; no stack-window exclusion needed).
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grid_offset.h"
void rec_super_call(Core*, uint32_t);

// MIPS-exact signed 32x32->lo multiply (low word).
static inline uint32_t mlo(int32_t a, int32_t b) { return (uint32_t)((uint64_t)(int64_t)a * (int64_t)b); }

// MIPS-exact signed 32/32 division (quotient). The PSX `div` traps on /0 and on INT_MIN/-1 via the
// recomp's `break`; the recomp body never actually divides by zero here (the divisor a3^0x3F is the
// 6-bit-complemented cell low byte, non-zero in practice) — mirror the C99 truncating-toward-zero
// quotient, guarding the two UB cases exactly as MIPS hardware leaves them.
static inline int32_t mdiv(int32_t a, int32_t b) {
  if (b == 0) return (a >= 0) ? -1 : 1;           // MIPS div-by-0 quotient
  if (a == (int32_t)0x80000000 && b == -1) return a;  // overflow case
  return a / b;
}

static uint32_t grid_offset_48360(Core* c) {
  const uint32_t SP = 0x1F800000u;
  // ---- block 1: raw cell-local offset (probe - origin) & 0x3F ----
  uint32_t a1 = ((uint32_t)c->mem_r16(SP + 0x1BC) - (uint32_t)c->mem_r16(SP + 0x1AA)) & 0x3Fu;  // dx
  uint32_t t4 = a1;                                            // keep the pre-transform copy
  c->mem_w16(SP + 0x1C2, (uint16_t)a1);
  uint32_t a0 = ((uint32_t)c->mem_r16(SP + 0x1C0) - (uint32_t)c->mem_r16(SP + 0x1AC)) & 0x3Fu;  // dz
  uint32_t t5 = a0;
  c->mem_w16(SP + 0x1C6, (uint16_t)a0);

  uint32_t rec = c->mem_r32(SP + 0x1E0);
  uint32_t t2  = c->mem_r16(rec + 0);                          // TAG
  uint32_t q   = t2 & 3u;

  // ---- block 2: pre-mirror (^0x3F) of dx/dz by TAG&3 ----
  //   q==0: none   q==1: dz   q==2: dx   q==3: dx & dz
  if (q == 1 || q == 3) c->mem_w16(SP + 0x1C6, (uint16_t)(a0 ^ 0x3Fu));
  if (q == 2 || q == 3) c->mem_w16(SP + 0x1C2, (uint16_t)(a1 ^ 0x3Fu));

  // ---- block 3: TAG&4 swap dx<->dz ----
  if (t2 & 4u) {
    uint16_t dx = c->mem_r16(SP + 0x1C2), dz = c->mem_r16(SP + 0x1C6);
    c->mem_w16(SP + 0x1C2, dz);
    c->mem_w16(SP + 0x1C6, dx);
  }

  // ---- block 4: slope shear / divide (TAG&8) ----
  rec = c->mem_r32(SP + 0x1E0);
  uint32_t slope = c->mem_r16(rec + 6);
  uint32_t a2 = slope >> 8;                                    // hi byte
  uint32_t a3 = slope & 0xFFu;                                 // lo byte (== t3, the return value)
  uint32_t t3 = a3;
  if (t2 & 8u) {
    // sheared X-from-Z: dz -= a3 + ((((a2 - a3) * dx) ) >> 6);  dx := 0
    int32_t  dxs = c->mem_r16s(SP + 0x1C2);
    int32_t  prod = (int32_t)mlo((int32_t)(a2 - t3), dxs);
    c->mem_w16(SP + 0x1C2, 0);
    uint32_t dz  = c->mem_r16(SP + 0x1C6);
    int32_t  shift = prod >> 6;                                // sra
    uint32_t sub = a3 + (uint32_t)shift;
    c->mem_w16(SP + 0x1C6, (uint16_t)(dz - sub));
  } else {
    // divided Z-from-X: dz -= ((dx_signed - a3) * a2) / (a3 ^ 0x3F);  dx := 0
    int32_t  dxs = c->mem_r16s(SP + 0x1C2);
    int32_t  num = (int32_t)mlo((int32_t)(a2 & 0xFFFFu), (dxs - (int32_t)(t3 & 0xFFFFu)));
    int32_t  den = (int32_t)(a3 ^ 0x3Fu);
    int32_t  qv  = mdiv(num, den);
    c->mem_w16(SP + 0x1C2, 0);
    uint32_t dz  = c->mem_r16(SP + 0x1C6);
    c->mem_w16(SP + 0x1C6, (uint16_t)(dz - (uint32_t)qv));
  }

  // ---- block 5: TAG&4 swap back ----
  if (t2 & 4u) {
    uint16_t dx = c->mem_r16(SP + 0x1C2), dz = c->mem_r16(SP + 0x1C6);
    c->mem_w16(SP + 0x1C2, dz);
    c->mem_w16(SP + 0x1C6, dx);
  }

  // ---- block 6: sign-negate by TAG&3 ----
  //   q==0: dx & dz   q==1: dx   q==2: dz   q==3: none
  if (q == 0) {
    c->mem_w16(SP + 0x1C2, (uint16_t)(0u - c->mem_r16(SP + 0x1C2)));
    c->mem_w16(SP + 0x1C6, (uint16_t)(0u - c->mem_r16(SP + 0x1C6)));
  } else if (q == 1) {
    c->mem_w16(SP + 0x1C2, (uint16_t)(0u - c->mem_r16(SP + 0x1C2)));
  } else if (q == 2) {
    c->mem_w16(SP + 0x1C6, (uint16_t)(0u - c->mem_r16(SP + 0x1C6)));
  }

  // ---- block 7: accumulate offset back onto the working coords + re-mirror/re-swap ----
  uint32_t dxv = c->mem_r16(SP + 0x1C2);
  uint32_t dzv = c->mem_r16(SP + 0x1C6);
  uint32_t nx  = (c->mem_r16(SP + 0x1BC) + dxv) & 0xFFFFu;     // probeX + dx
  uint32_t nz  = (c->mem_r16(SP + 0x1C0) + dzv) & 0xFFFFu;     // probeZ + dz
  uint32_t ox  = (t4 + dxv) & 0xFFFFu;                         // (orig dx) + dx
  uint32_t oz  = (t5 + dzv) & 0xFFFFu;                         // (orig dz) + dz
  c->mem_w16(SP + 0x1C0, (uint16_t)nz);
  c->mem_w16(SP + 0x1BC, (uint16_t)nx);
  c->mem_w16(SP + 0x1C2, (uint16_t)ox);
  c->mem_w16(SP + 0x1C6, (uint16_t)oz);

  // post re-mirror (^0x3F) by TAG&3 (operates on ox/oz now in sh[0x1C2]/sh[0x1C6])
  if (q == 2 || q == 3) c->mem_w16(SP + 0x1C2, (uint16_t)(ox ^ 0x3Fu));
  if (q == 1 || q == 3) c->mem_w16(SP + 0x1C6, (uint16_t)(oz ^ 0x3Fu));

  // post re-swap by TAG&4
  if (t2 & 4u) {
    uint16_t dx = c->mem_r16(SP + 0x1C2), dz = c->mem_r16(SP + 0x1C6);
    c->mem_w16(SP + 0x1C2, dz);
    c->mem_w16(SP + 0x1C6, dx);
  }
  return t3;                                                   // v0 = t3 = rec[7] (slope lo byte)
}
