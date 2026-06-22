// Hand-written native C++ for the boot → first-cutscene path (Tomba2Engine) — batch A2.
//
// DIRECTION (user, 2026-06-19): the boot→first-cutscene path is HAND-WRITTEN NATIVE C++ — not
// interpreted, not machine-recompiled. The recompiler's own decode (generated/gen_func_*) is the
// authoritative RE reference; this file re-expresses each body as readable native C++ that preserves
// exact behavior. Each function is registered as a runtime override so the C++ runs instead of the
// interpreter. Companion to engine/native_path.cpp (same conventions/comment density).
//
// Conventions: c->r[4..7]=a0..a3, c->r[2]=v0 return, c->r[29]=sp, c->r[0]==0. 16/8-bit reads are
// zero-extended unless the body casts (int16_t)/(int8_t). Globals shown as `(N)<<16 + signed-off`
// are folded to absolute addresses (literal + comment). The trailing duplicate `return;` and any
// post-`jr ra` register write in the gen_func bodies are the recompiler over-running into the next
// function and are NOT part of the real body — they are ignored here.
#include "core.h"
#include <stdint.h>

// 0x8009A640 — strncmp(a0, a1, a2): NUL-guard like strcmp, else compare up to a2 bytes.
// Guard: if a0==NULL or a1==NULL → equal pointers→0, a0==NULL→-1, else 1. Otherwise loop while
// count (a2, treated signed and pre-decremented) stays >=0: read signed bytes; the s2 pointer
// advances every iteration (delay-slot increment), so the mismatch result is *s1 - *(s2 already
// advanced) read back from s2-1. Equal+NUL → 0. Count exhausted with all-equal → 0.
static void ov_8009A640(Core* c) {
  uint32_t s1 = c->r[4], s2 = c->r[5];
  if (s1 == 0 || s2 == 0) {
    if (s1 == s2) c->r[2] = 0;            // both NULL → equal
    else if (s1 == 0) c->r[2] = (uint32_t)-1;
    else c->r[2] = 1;
    return;
  }
  int32_t n = (int32_t)c->r[6] - 1;       // r6-- before first iter
  if (n < 0) { c->r[2] = 0; return; }     // zero-length compare → equal
  for (;;) {
    int32_t a = (int8_t)c->mem_r8(s1);
    int32_t b = (int8_t)c->mem_r8(s2);
    s2++;                                  // always (delay slot)
    if (a != b) {                          // mismatch: *s1 - *(s2-1)
      int32_t x = (int8_t)c->mem_r8(s1);
      int32_t y = (int8_t)c->mem_r8(s2 - 1);
      c->r[2] = (uint32_t)(x - y);
      return;
    }
    s1++;                                  // delay-slot increment on the a==b path
    if (a == 0) { c->r[2] = 0; return; }   // matched NUL → equal
    n--;
    if (n < 0) { c->r[2] = 0; return; }    // count exhausted, all-equal → 0
  }
}

// 0x8008A00C — binary frame-count → packed BCD time at a1, the encoder paired with 0x8008A110's
// decoder. a0 is biased by +150, then split into three base-60 BCD digit-pairs written as bytes
// a1[0..2]. The three magic multiplies are constant-division reciprocals:
//   /900  via 0x1B4E81B5 (mulhi, >>3, minus sign), /9 via 0x88888889, /5 via 0x66666667.
// Transcribed register-faithfully (the div-by-constant strength reduction is preserved exactly so
// the rounding/sign behavior matches the MIPS); a1 bytes get the low 8 bits of each packed result.
static void ov_8008A00C(Core* c) {
  auto mulhi = [](int32_t a, int32_t b) -> int32_t {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
  };
  int32_t x  = (int32_t)(c->r[4] + 150u);    // r4 = a0 + 150
  uint32_t out = c->r[5];                     // r2 = a1 (output ptr)

  // r7 = x / 900  : mulhi(x,0x1B4E81B5) >>3 (arith), minus sign(x)
  int32_t r7 = (mulhi(x, 0x1B4E81B5) >> 3) - (x >> 31);
  int32_t mh9 = mulhi(r7, 0x88888889);        // saved for r8 below
  // r4 = x - 75*r7  (r3 = ((5*r7)<<4) - (5*r7))
  int32_t f5 = (r7 << 2) + r7;                // 5*r7
  x = x - ((f5 << 4) - f5);                   // r4 ← x - 75*r7

  // r8 = (mh9 + r7) >>5 (arith), minus sign(r7)   == r7 / 9
  int32_t r8 = ((mh9 + r7) >> 5) - (r7 >> 31);
  // r7 = r7 - 60*r8   (r3 = ((r8<<4)-r8)<<2)
  r7 = r7 - (((r8 << 4) - r8) << 2);

  int32_t mh5_r7 = mulhi(r7, 0x66666667);     // mulhi of the post-subtract r7 (used for a1+1)
  // r5 = x / 5  : mulhi(x,0x66666667) >>2, minus sign(x)
  int32_t r5 = (mulhi(x, 0x66666667) >> 2) - (x >> 31);
  int32_t r6 = r5 << 4;                        // 16*r5
  x = x - (((r5 << 2) + r5) << 1);             // r4 ← x - 10*r5
  r6 = r6 + x;
  c->mem_w8(out + 2, (uint8_t)r6);             // a1[2]

  int32_t mh5_r8 = mulhi(r8, 0x66666667);      // for a1+0
  // a1+1: r4 = r7 / 5 (mulhi(r7,..)>>2 - sign(r7)); r5 = 16*r4 + (r7 - 10*r4)
  int32_t r4b = (mh5_r7 >> 2) - (r7 >> 31);
  int32_t r5b = r4b << 4;
  r7 = r7 - (((r4b << 2) + r4b) << 1);
  r5b = r5b + r7;
  c->mem_w8(out + 1, (uint8_t)r5b);            // a1[1]

  // a1+0: r4 = r8 / 5 (mulhi(r8,..)>>2 - sign(r8)); r5 = 16*r4 + (r8 - 10*r4)
  int32_t r4c = (mh5_r8 >> 2) - (r8 >> 31);
  int32_t r5c = r4c << 4;
  r8 = r8 - (((r4c << 2) + r4c) << 1);
  r5c = r5c + r8;
  c->mem_w8(out + 0, (uint8_t)r5c);            // a1[0]
}

// 0x8008A110 — packed BCD time at a0 → binary frame count (decoder, inverse of 0x8008A00C). Reads
// bytes a0[0],a0[1],a0[2]; each byte is a BCD pair decoded as (hi_nibble*10 + lo_nibble). Then
//   v0 = 75 * (d0*60 + d1) + d2 - 150.
// Transcribed exactly from the shift/add reduction in the body (×60 = (n<<4-n)<<2; ×75 = (m<<2+m)<<4 - (m<<2+m)).
static void ov_8008A110(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t b0 = c->mem_r8(a0 + 0);
  uint32_t d0 = (b0 >> 4) * 10u + (b0 & 15u);   // BCD digit-pair 0
  uint32_t acc = (d0 << 4) - d0;                // 15*d0
  acc <<= 2;                                    // 60*d0

  uint32_t b1 = c->mem_r8(a0 + 1);
  uint32_t d1 = (b1 >> 4) * 10u + (b1 & 15u);   // BCD digit-pair 1
  acc = acc + d1;                               // 60*d0 + d1

  uint32_t m = (acc << 2) + acc;                // 5*acc
  uint32_t r2 = (m << 4) - m;                   // 75*acc

  uint32_t b2 = c->mem_r8(a0 + 2);
  uint32_t d2 = (b2 >> 4) * 10u + (b2 & 15u);   // BCD digit-pair 2
  r2 = r2 + d2;
  c->r[2] = r2 + (uint32_t)-150;
}

// 0x80082240 / 0x800822D8 — build a GPU draw-area / clip primitive word. The two are byte-identical
// except the command byte: 0x80082240 uses 0xE3000000, 0x800822D8 uses 0xE4000000 (set-draw-area-TL
// vs set-draw-area-BR). Args a0,a1 are signed 16-bit X,Y clamped to a screen-mode-dependent range:
//   X clamped to [0, *(0x800F59A4)-1] (signed 16-bit field at 0x800A0000+22948);
//   Y clamped to [0, *(0x800F59A6)-1] (field at +22950).
// A negative input clamps to 0; an input >= limit clamps to (limit-1); otherwise the in-range case
// passes the raw arg through (the body uses original a0/a1 there — low-10-bit-identical to the
// sign-extended value after the final &1023). The packed result is cmd | (Y&1023)<<10 | (X&1023).
// (Fields read both signed (for the < test) and unsigned (for the upper clamp value), per the body.)
static void clip_word(Core* c, uint32_t cmd) {
  // X (a0) clamp against limit at 0x800A59A4
  int32_t x = (int16_t)c->r[4];
  uint32_t xout;
  if (x < 0) xout = 0;
  else {
    int32_t lim_s = (int16_t)c->mem_r16(0x800A59A4u);   // signed limit
    uint32_t lim_u = c->mem_r16(0x800A59A4u);            // unsigned limit
    if ((lim_s - 1) < x) xout = lim_u - 1;               // above range → limit-1
    else xout = (uint32_t)x;
  }
  // Y (a1) clamp against limit at 0x800A59A6
  int32_t y = (int16_t)c->r[5];
  uint32_t yout;
  if (y < 0) yout = 0;
  else {
    int32_t lim_s = (int16_t)c->mem_r16(0x800A59A6u);
    uint32_t lim_u = c->mem_r16(0x800A59A6u);
    if ((lim_s - 1) < y) yout = lim_u - 1;
    else yout = (uint32_t)y;
  }
  c->r[2] = cmd | ((yout & 1023u) << 10) | (xout & 1023u);
}
static void ov_80082240(Core* c) { clip_word(c, 0xE3000000u); }
static void ov_800822D8(Core* c) { clip_word(c, 0xE4000000u); }

// 0x80090160 — consume one 7-bit-continuation varint from a per-row stream cursor and add 10× its
// value into that row's accumulator. a0 (signed 16-bit) ×4 indexes a row-base-pointer table at
// 0x80100000 + a0*4 + 19504; a1 (signed 16-bit) ×176 offsets into that row to the cursor slot `s`:
//   s = *(0x80100000 + a0*4 + 19504) + 176*a1.
// Read byte b at *s (advancing the cursor). b==0 → no-op. Else if b<0x80, val=b; else multi-byte:
// val = b&0x7F, then while the continuation bit is set fold (val = (val<<7) + (next&0x7F)). Finally
// accumulator[s+136] += 10*val  (r2 = (val<<2 + val)<<1 = 10*val). Cursor advances one byte per read.
static void ov_80090160(Core* c) {
  int32_t a0x4 = (int32_t)(c->r[4] << 16) >> 14;   // (int16)a0 * 4
  int32_t idx  = (int32_t)(c->r[5] << 16) >> 16;   // (int16)a1
  int32_t row  = (((idx << 1) + idx) << 2) - idx;  // 11*idx
  row <<= 4;                                        // 176*idx
  uint32_t rowbase = c->mem_r32((uint32_t)(0x80100000 + a0x4) + 19504u);
  uint32_t s = rowbase + (uint32_t)row;             // cursor-slot address

  uint32_t cur = c->mem_r32(s);
  uint32_t b = c->mem_r8(cur);
  c->mem_w32(s, cur + 1);                            // advance cursor
  if (b == 0) return;                               // empty → nothing accumulated

  uint32_t val;
  if ((b & 128u) == 0) {
    val = b;                                         // single-byte
  } else {
    val = b & 127u;                                 // multi-byte varint
    for (;;) {
      uint32_t cur2 = c->mem_r32(s);
      val <<= 7;
      uint32_t nb = c->mem_r8(cur2);
      c->mem_w32(s, cur2 + 1);
      val += (nb & 127u);
      if ((nb & 128u) == 0) break;
    }
  }
  uint32_t add = ((val << 2) + val) << 1;            // 10*val
  c->mem_w32(s + 136, c->mem_r32(s + 136) + add);
}

// 0x8005082C — latch a controller rumble / actuator command. Reads three actuator bytes from a fixed
// pad-state block at 0x800E80A8 (0x800F0000-32600 + 8236/8237/8238/8239), sets a "dirty" flag, and
// scatters them (plus the caller's a0,a1,a2) into two staging regions and the SPU/pad HW shadow at
// 0x800BF870 (0x80100000-1936). Layout faithfully reproduced from the body.
static void ov_8005082C(Core* c) {
  uint32_t base = 0x800E80A8u;                       // 0x800F0000 - 32600
  uint32_t r7 = c->mem_r8(base + 8236);              // saved actuator[0]
  c->mem_w8(base + 16540, 1);                        // dirty flag B
  c->mem_w8(base + 8236, 1);                         // dirty flag A
  uint32_t blk = base + 8212;                        // staging block 1
  uint32_t r8 = c->mem_r8(base + 8237);
  uint32_t r9 = c->mem_r8(base + 8238);
  uint32_t r10 = c->mem_r8(base + 8239);
  uint32_t blk2 = base + 16516;                      // staging block 2
  c->mem_w8(blk + 25, (uint8_t)c->r[4]);
  c->mem_w8(blk + 26, (uint8_t)c->r[5]);
  c->mem_w8(blk + 27, (uint8_t)c->r[6]);
  c->mem_w8(blk2 + 25, (uint8_t)c->r[4]);
  c->mem_w8(blk2 + 26, (uint8_t)c->r[5]);
  c->mem_w8(blk2 + 27, (uint8_t)c->r[6]);
  uint32_t hw = 0x800BF870u;                         // 0x80100000 - 1936
  c->mem_w8(hw + 55, (uint8_t)((r7 << 7) | 1u));     // command byte: actuator[0]<<7 | enable
  c->mem_w8(hw + 52, (uint8_t)r8);
  c->mem_w8(hw + 53, (uint8_t)r9);
  c->mem_w8(hw + 54, (uint8_t)r10);
}

// 0x8008B040 — push a 4-byte pad/memcard command sequence through the SIO-ish HW shadow pointer
// table at 0x800AC280/284/288/28C (each holds a HW byte-register address). Writes: *p_ctrl=2,
// *p_data=a0[0], *p_aux=a0[1], *p_ctrl=3, *p_data2=a0[2], *p_aux=a0[3], *p_aux2=32. Returns 0.
static void ov_8008B040(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t p_ctrl = c->mem_r32(0x800AC280u);   // 0x800B0000 - 15744
  c->mem_w8(p_ctrl, 2);
  uint32_t p_data = c->mem_r32(0x800AC288u);   // -15736
  c->mem_w8(p_data, (uint8_t)c->mem_r8(a0 + 0));
  uint32_t p_data2 = c->mem_r32(0x800AC28Cu);  // -15732
  c->mem_w8(p_data2, (uint8_t)c->mem_r8(a0 + 1));
  p_ctrl = c->mem_r32(0x800AC280u);
  c->mem_w8(p_ctrl, 3);
  uint32_t p_aux = c->mem_r32(0x800AC284u);    // -15740
  c->mem_w8(p_aux, (uint8_t)c->mem_r8(a0 + 2));
  p_data = c->mem_r32(0x800AC288u);
  c->mem_w8(p_data, (uint8_t)c->mem_r8(a0 + 3));
  p_data2 = c->mem_r32(0x800AC28Cu);
  c->mem_w8(p_data2, 32);
  c->r[2] = 0;
}

// 0x80083DE0 — build a GPU sprite/quad primitive (3 words) at a0 from flags a1,a2 and an optional
// color/UV source a3plus (5th stack arg at sp+16). Word layout:
//   a0[3] (a byte) = 2;
//   a0[4] (word) = 0xE1000000 | (a2!=0 ? 0 : 0x200) | (a1!=0 ? (a3 & 0x9FF) : 0)   [draw-mode]
//     — precisely: r3 = 0xE1000000; if a2(==r6)==0 keep else |0x200; r2 = a3(==r7)&0x9FF; if
//       a1(==r5)==0 keep else |0x400; word = r3|r2;
//   a0[8] (word) = if 5th-arg ptr (sp+16) == 0 → 0; else a packed 555 color + offset from that 8-byte
//     source: color = 0xE2000000 | (src[2]>>3<<15) | (src[0]>>3<<10) | ((-src16[6])<<2 & 0x3E0)
//              | (((-src16[4]) & 0xFF) >>(arith) 3).
// Transcribed exactly (the negations are signed-16 reads negated; the &0x3E0 / >>3 masks preserved).
static void ov_80083DE0(Core* c) {
  uint32_t a0 = c->r[4];                         // r8
  c->mem_w8(a0 + 3, 2);

  uint32_t r3 = 0xE1000000u;
  if (c->r[6] != 0) r3 |= 512u;                  // a2 != 0
  uint32_t r2 = c->r[7] & 2559u;                 // a3 & 0x9FF
  if (c->r[5] != 0) r2 |= 1024u;                 // a1 != 0
  c->mem_w32(a0 + 4, r3 | r2);

  uint32_t src = c->mem_r32(c->r[29] + 16);      // 5th stack arg (o32, no prologue)
  if (src == 0) { c->mem_w32(a0 + 8, 0); return; }

  uint32_t w = 0xE2000000u;                       // 57856<<16
  uint32_t hi = c->mem_r8(src + 2);  hi = (hi >> 3) << 15;
  uint32_t lo = c->mem_r8(src + 0);  lo = (lo >> 3) << 10;
  w = lo | w;
  w = hi | w;
  int32_t g = (int16_t)c->mem_r16(src + 6);
  uint32_t gg = ((uint32_t)(0 - g) << 2) & 992u;  // (-g)<<2 & 0x3E0
  w |= gg;
  int32_t r = (int16_t)c->mem_r16(src + 4);
  uint32_t rr = ((uint32_t)(0 - r)) & 255u;
  rr = (uint32_t)((int32_t)rr >> 3);              // arithmetic >>3 of a masked byte (== logical here)
  w |= rr;
  c->mem_w32(a0 + 8, w);
}

// 0x800998E4 — fill the 24-byte status array at a0 from two parallel 16-element bitmask/table globals.
// For each i in [0,24): entry = *(0x800AC604) + (i<<4); mask = *(0x800AC590) & (1<<i);
//   tag = *(uint16_t)(entry+12);
//   a0[i] = mask ? (tag ? 1 : 3) : (tag ? 2 : 0).
static void ov_800998E4(Core* c) {
  uint32_t out = c->r[4];
  uint32_t table = c->mem_r32(0x800AC604u);        // 0x800B0000 - 14844
  uint32_t bits  = c->mem_r32(0x800AC590u);        // 0x800B0000 - 14960
  for (int i = 0; i < 24; i++) {
    uint32_t entry = table + ((uint32_t)i << 4);
    uint32_t mask = bits & (1u << (i & 31));
    uint32_t tag = c->mem_r16(entry + 12);
    uint8_t v;
    if (mask != 0) v = tag ? 1 : 3;
    else           v = tag ? 2 : 0;
    c->mem_w8(out + i, v);
  }
}

// 0x80097A90 — multi-pass coalescing/defragmentation over the GPU/heap free-list. The list lives at
// base B = *0x800AC66C with count-driver N = *0x800AC668; entries are 8 bytes {word0=flags|addr,
// word1=size}. Masks: USED=0x80000000, SENT=0x0CFFFFFF (12287<<16|0xFFFF), ADDR=0x0FFFFFFF
// (4095<<16|0xFFFF), TAG=0x40000000 (16384<<16). It is dense pointer/list surgery whose exact merge
// order and delay-slot side effects matter, so this is a faithful 1:1 transcription of the recompiler
// decode (the authoritative reference) — register-level, control flow preserved verbatim. Restructuring
// it into "clean" loops risks a subtly-wrong reorder, so deliberately NOT done. If N<0 the body no-ops.
static void ov_80097A90(Core* c) {
  uint32_t* r = c->r;
  const uint32_t USED = 0x80000000u;   // r12 (pass1) / r8 (pass3)/ r14(pass2)
  const uint32_t SENT = 0x0CFFFFFFu;   // r10 (pass1) / r7 (pass3)/ r6(pass4)
  const uint32_t ADDR = 0x0FFFFFFFu;   // r11 (pass1) / r12(pass2)/ r6(pass3 lo)

  // ── Pass 1 (L_80097A90 .. L_80097B6C) ──────────────────────────────────────────────────────────
  r[2] = c->mem_r32(0x800AC668u);                     // N
  if ((int32_t)r[2] < 0) goto L_80097B6C;             // delay: r9=0
  r[9] = 0;
  r[8] = c->mem_r32(0x800AC66Cu);                     // B
  r[13] = r[2];                                       // N
  r[7] = r[8];                                        // walker
L_80097AC8:
  if ((c->mem_r32(r[7] + 0) & USED) == 0) { r[6] = r[9] + 1; goto L_80097B4C; }
  r[6] = r[9] + 1;
  r[3] = (r[6] << 3) + r[8];
L_80097AE4:
  r[2] = c->mem_r32(r[3] + 0);
  r[3] = r[3] + 8;                                     // delay slot — always executes
  if (r[2] != SENT) goto L_80097AFC;
  r[6] = r[6] + 1; goto L_80097AE4;
L_80097AFC:
  r[5] = (r[6] << 3) + r[8];
  r[3] = c->mem_r32(r[5] + 0);
  if ((r[3] & USED) == 0) { r[2] = r[3] & ADDR; goto L_80097B4C; }
  r[2] = r[3] & ADDR;
  r[3] = (c->mem_r32(r[7] + 0) & ADDR) + c->mem_r32(r[7] + 4);
  if (r[2] != r[3]) goto L_80097B4C;
  c->mem_w32(r[5] + 0, SENT);
  c->mem_w32(r[7] + 4, c->mem_r32(r[7] + 4) + c->mem_r32(r[5] + 4));
  goto L_80097B54;
L_80097B4C:
  r[7] = r[7] + 8;
  r[9] = r[9] + 1;
L_80097B54:
  if (!((int32_t)r[13] < (int32_t)r[9])) goto L_80097AC8;
  r[2] = c->mem_r32(0x800AC668u);
L_80097B6C:
  // ── Pass 2 (.. L_80097BB0) : tag entries whose size==0 with SENT ───────────────────────────────
  if ((int32_t)r[2] < 0) goto L_80097BB0;             // delay: r9=0
  r[9] = 0;
  r[5] = SENT;
  r[4] = r[2];                                        // N
  r[3] = c->mem_r32(0x800AC66Cu);                     // B
L_80097B8C:
  r[2] = c->mem_r32(r[3] + 4);
  if (r[2] != 0) goto L_80097BA0;
  c->mem_w32(r[3] + 0, r[5]);
L_80097BA0:
  r[9] = r[9] + 1;
  if (!((int32_t)r[4] < (int32_t)r[9])) { r[3] = r[3] + 8; goto L_80097B8C; }
L_80097BB0:
  // ── Pass 3 (.. L_80097C7C) : address-sort the non-TAG'd run via swaps ──────────────────────────
  r[3] = c->mem_r32(0x800AC668u);                     // N
  if ((int32_t)r[3] < 0) goto L_80097C7C;             // delay: r9=0
  r[9] = 0;
  r[14] = 0x40000000u;                                // TAG
  r[12] = ADDR;
  r[13] = c->mem_r32(0x800AC66Cu);                    // B
  r[10] = r[13];                                      // outer
L_80097BDC:
  r[2] = c->mem_r32(r[10] + 0) & r[14];
  if (r[2] != 0) goto L_80097C7C;                     // TAG'd → stop
  r[6] = r[9] + 1;
  if ((int32_t)r[3] < (int32_t)r[6]) { r[2] = r[6] << 3; goto L_80097C64; }
  r[8] = r[10];                                       // anchor
  r[11] = c->mem_r32(0x800AC668u);                    // N (inner bound)
  r[4] = (r[6] << 3) + r[13];                         // inner
L_80097C10:
  r[5] = c->mem_r32(r[4] + 0);
  if ((r[5] & r[14]) != 0) { r[2] = r[5] & r[12]; goto L_80097C64; }
  r[2] = r[5] & r[12];
  r[7] = c->mem_r32(r[8] + 0);
  r[3] = r[7] & r[12];
  if (!(r[2] < r[3])) goto L_80097C54;                // anchor addr <= inner → no swap
  c->mem_w32(r[8] + 0, r[5]);
  r[2] = c->mem_r32(r[4] + 4);
  r[3] = c->mem_r32(r[8] + 4);
  c->mem_w32(r[8] + 4, r[2]);
  c->mem_w32(r[4] + 0, r[7]);
  c->mem_w32(r[4] + 4, r[3]);
L_80097C54:
  r[6] = r[6] + 1;
  if (!((int32_t)r[11] < (int32_t)r[6])) { r[4] = r[4] + 8; goto L_80097C10; }
L_80097C64:
  r[3] = c->mem_r32(0x800AC668u);
  r[9] = r[9] + 1;
  if (!((int32_t)r[3] < (int32_t)r[9])) { r[10] = r[10] + 8; goto L_80097BDC; }
L_80097C7C:
  // ── Pass 4a (.. L_80097D00) : pull the size-0/SENT head entry forward into TAG'd slots ─────────
  r[5] = c->mem_r32(0x800AC668u);                     // N
  if ((int32_t)r[5] < 0) goto L_80097D00;             // delay: r9=0
  r[9] = 0;
  r[8] = 0x40000000u;                                 // TAG
  r[7] = SENT;
  r[6] = c->mem_r32(0x800AC66Cu);                     // B
  r[4] = r[6];                                        // walker
L_80097CA8:
  r[3] = c->mem_r32(r[4] + 0);
  if ((r[3] & r[8]) != 0) goto L_80097D00;            // TAG'd → stop
  if (r[3] != r[7]) { r[2] = r[5] << 3; goto L_80097CE8; }   // not SENT → continue scan
  r[2] = (r[5] << 3) + r[6];                          // copy head entry[N] into this slot
  r[3] = c->mem_r32(r[2] + 0);
  c->mem_w32(r[4] + 0, r[3]);
  r[2] = c->mem_r32(r[2] + 4);
  c->mem_w32(0x800AC668u, r[9]);                       // latch index
  c->mem_w32(r[4] + 4, r[2]);
  goto L_80097D00;
L_80097CE8:
  r[5] = c->mem_r32(0x800AC668u);
  r[9] = r[9] + 1;
  if (!((int32_t)r[5] < (int32_t)r[9])) { r[4] = r[4] + 8; goto L_80097CA8; }
L_80097D00:
  // ── Pass 4b (.. L_80097D88) : walk backward from entry N-1, OR in TAG and accumulate sizes ─────
  r[2] = c->mem_r32(0x800AC668u);
  r[9] = r[2] + (uint32_t)-1;                          // N-1
  if ((int32_t)r[9] < 0) { r[2] = r[9] << 3; goto L_80097D88; }
  r[8] = USED;                                         // 0x80000000
  r[6] = ADDR;
  r[7] = 0x40000000u;                                  // TAG
  r[5] = c->mem_r32(0x800AC66Cu);                      // B
  r[4] = (r[9] << 3) + r[5];                           // &entry[N-1]
L_80097D38:
  r[3] = c->mem_r32(r[4] + 0);
  if ((r[3] & r[8]) == 0) { r[2] = r[3] & r[6]; goto L_80097D88; }   // unused → stop
  r[2] = r[3] & r[6];
  r[3] = c->mem_r32(0x800AC668u);
  r[2] = r[2] | r[7];                                  // OR in TAG
  c->mem_w32(r[4] + 0, r[2]);
  r[2] = c->mem_r32(r[4] + 4);
  c->mem_w32(0x800AC668u, r[9]);                       // latch index
  r[3] = ((r[3] << 3) + r[5]);
  r[3] = c->mem_r32(r[3] + 4);                          // entry[count].size
  r[9] = r[9] + (uint32_t)-1;
  r[2] = r[2] + r[3];
  c->mem_w32(r[4] + 4, r[2]);
  if ((int32_t)r[9] >= 0) { r[4] = r[4] + (uint32_t)-8; goto L_80097D38; }
L_80097D88:
  return;
}

// Register every batch-A2 hand-native function (called from games_tomba2_init at startup).
void games_native_path_a2_init(void) {
  // TODO(verify): rec_set_override(0x80090160u, ov_80090160); — DISABLED: A/B RAM-diff fails (80 bytes vs interp). varint stream consumer — wrong accumulation. Re-enable after fixing gen_func_80090160 + re-A/B.
}
