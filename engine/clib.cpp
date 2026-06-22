// engine/clib.cpp — PC-native libc-style helpers the engine RE references (memset/memcpy/bzero/strcmp/strncmp/strlen, the LCG rand, integer sqrt, atan2).

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

// 0x80077FB0 — integer square root (16-bit binary-search refinement). a0 = input; the body bisects
// candidate r5 bit-by-bit from 0x8000 down to 1, keeping each bit whose square stays <= a0, with a
// final +1 rounding correction. Returns v0 = isqrt(a0) (16-bit). The clamp at the top forces a0 into
// the representable range. The real `jr ra` is at L_800781D8; everything after it belongs to the next
// function (a separate distance/length helper) and is IGNORED.
static void ov_80077FB0(Core* c) {
  uint32_t a0 = c->r[4];
  // clamp: if a0 > 0x3FFFFFFF then sign-mask in -32768 → r6 base, used to seed the high bit search
  uint32_t over = (uint32_t)(0x3FFFFFFFu < a0);              // 1 if a0 exceeds max
  uint32_t r2 = (uint32_t)(0u - over) & 0xFFFF8000u;        // -over & -32768
  uint32_t r6 = r2 + 16384;                                  // seed
  uint32_t r5 = r2;
  // first probe uses (r6 & 0xC000)^2
  {
    uint32_t r3 = r6 & 49152u;
    uint32_t sq = (uint32_t)((int32_t)r3 * (int32_t)r3);
    if (a0 < sq) r5 = r5 + 8192;                             // keep low base + 8192
    else         r5 = r6 + 8192;                             // adopt r6 then +8192
  }
  // remaining bits: 4096,2048,...,1  (each: if square(r5+bit) <= a0 keep it)
  static const uint32_t bits[] = {4096,2048,1024,512,256,128,64,32,16,8,4,2,1};
  for (uint32_t bit : bits) {
    uint32_t cand = r5 + bit;
    uint32_t q = cand & 0xFFFFu;
    uint32_t sq = (uint32_t)((int32_t)q * (int32_t)q);
    if (!(a0 < sq)) r5 = cand;                               // square fits → keep
  }
  // final rounding: if (r5 low16) != 0xFFFF and ((r5)^2 + r5) < a0 then r5++
  uint32_t r3 = r5 & 0xFFFFu;
  uint32_t res;
  if (r3 == 0xFFFFu) {
    res = r3;
  } else {
    uint32_t sq = (uint32_t)((int32_t)r3 * (int32_t)r3);
    if ((sq + r3) < a0) res = (r5 + 1) & 0xFFFFu;
    else                res = r5 & 0xFFFFu;
  }
  c->r[2] = res;
}

// 0x80079528 — strlen(a0). (gen_func_80079528's body past the first `jr ra` — the 8009A640 call — is
// the next function's recompiler over-run; the real body is just the count loop.)
static void ov_80079528(Core* c) {
  uint32_t p = c->r[4], n = 0;
  if (c->mem_r8(p) != 0) { for (;;) { p++; n++; if (c->mem_r8(p) == 0) break; } }
  c->r[2] = n;
}

// 0x80085690 — atan2-style angle from (a0=dy?, a1=dx?) returning a fixed-point angle in [0,4096).
// Records sign quadrant (r6 from a1<0, r7 from a0<0), takes absolute values, picks the larger axis,
// computes ratio*1024 via integer divide, looks up a 1024-entry arctan table at 0x800AA490
// (32779<<16 - 23408, s16) and folds the result through the quadrant flags. Uses the cpu_div helper
// (the MIPS DIV instruction) and rec_break for the div traps — these are interpreter primitives, not
// function calls, so this stays a leaf override.
static void ov_80085690(Core* c) {
  uint32_t r4 = c->r[4], r5 = c->r[5];
  uint32_t r6 = 0, r7 = 0;
  if ((int32_t)r5 < 0) { r6 = 1; r5 = 0u - r5; }
  if ((int32_t)r4 < 0) { r7 = 1; r4 = 0u - r4; }

  uint32_t r3;                                                // arctan table result / angle
  // compare |r4| vs |r5| (signed compare of the now-nonneg values)
  bool take_r5_path;
  if (r5 != 0) take_r5_path = ((int32_t)r4 < (int32_t)r5);
  else {
    if (r4 == 0) { c->r[2] = 0; return; }                    // both zero → angle 0
    take_r5_path = ((int32_t)r4 < (int32_t)r5);
  }

  if (take_r5_path) {
    // |r4| < |r5|: ratio = r4/r5 scaled, table indexed directly
    uint32_t mask = 0x7FE00000u;                             // 32736<<16
    if ((r4 & mask) == 0) {
      uint32_t num = r4 << 10;
      cpu_div(c, num, r5);
      if (r5 == 0) rec_break(c, 7168u);
      if (!(r5 != 0xFFFFFFFFu)) { if (num == 0x80000000u) rec_break(c, 6144u); }
      uint32_t q = c->lo;
      r3 = (uint32_t)(int16_t)c->mem_r16(0x800AA490u + ((q << 1)));
    } else {
      uint32_t d = (int32_t)r5 >> 10;
      cpu_div(c, r4, d);
      if (d == 0) rec_break(c, 7168u);
      if (!(d != 0xFFFFFFFFu)) { if (r4 == 0x80000000u) rec_break(c, 6144u); }
      uint32_t q = c->lo;
      r3 = (uint32_t)(int16_t)c->mem_r16(0x800AA490u + ((q << 1)));
    }
  } else {
    // |r4| >= |r5|: complementary angle (1024 - table)
    uint32_t mask = 0x7FE00000u;
    if ((r5 & mask) == 0) {
      uint32_t num = r5 << 10;
      cpu_div(c, num, r4);
      if (r4 == 0) rec_break(c, 7168u);
      if (!(r4 != 0xFFFFFFFFu)) { if (num == 0x80000000u) rec_break(c, 6144u); }
      uint32_t q = c->lo;
      r3 = (uint32_t)(int16_t)c->mem_r16(0x800AA490u + ((q << 1)));
    } else {
      uint32_t d = (int32_t)r4 >> 10;
      cpu_div(c, r5, d);
      if (d == 0) rec_break(c, 7168u);
      if (!(d != 0xFFFFFFFFu)) { if (r5 == 0x80000000u) rec_break(c, 6144u); }
      uint32_t q = c->lo;
      r3 = (uint32_t)(int16_t)c->mem_r16(0x800AA490u + ((q << 1)));
    }
    r3 = 1024u - r3;                                          // complementary
  }
  // fold quadrant flags
  if (r6 != 0) r3 = 2048u - r3;                               // a1<0 → reflect across X
  uint32_t result;
  if (r7 != 0) result = 0u - r3;                              // a0<0 → negate
  else         result = r3;
  c->r[2] = result;
}

// 0x800976C8 — original spins x=13; x*=13 sixty times into a stack local with NO external effect
// (no global/pointer write), returning 0. The native form drops the dead loop. (r2 = 0 on exit.)
static void ov_800976C8(Core* c) { c->r[2] = 0; }

// 0x8009A340 — bzero(a0, a1) returning a0; returns 0 when a0==0 or (int)a1<=0.
static void ov_8009A340(Core* c) {
  uint32_t p = c->r[4]; int n = (int32_t)c->r[5];
  if (p == 0 || n <= 0) { c->r[2] = 0; return; }
  c->r[2] = p; for (int i = 0; i < n; i++) c->mem_w8(p + i, 0);
}

// 0x8009A3E0 — memcpy(a0, a1, a2) returning a0 (0 if a0==0); no-op body when (int)a2<=0.
static void ov_8009A3E0(Core* c) {
  uint32_t d = c->r[4], s = c->r[5]; int n = (int32_t)c->r[6];
  if (d == 0) { c->r[2] = 0; return; }
  c->r[2] = d; if (n <= 0) return;
  for (int i = 0; i < n; i++) c->mem_w8(d + i, c->mem_r8(s + i));
}

// 0x8009A420 — memset(a0, (uint8_t)a1, a2) returning a0; returns 0 when a0==0 or (int)a2<=0.
static void ov_8009A420(Core* c) {
  uint32_t p = c->r[4]; int n = (int32_t)c->r[6];
  if (p == 0 || n <= 0) { c->r[2] = 0; return; }
  uint8_t v = (uint8_t)c->r[5]; c->r[2] = p;
  for (int i = 0; i < n; i++) c->mem_w8(p + i, v);
}

// 0x8009A450 — rand(): seed at 0x80105EE8; seed = seed*0x41C64E6D + 12345; return (seed>>16)&0x7FFF.
// (0x8009A480, already ported, is the matching srand that writes this seed.)
static void ov_8009A450(Core* c) {
  uint32_t s = c->mem_r32(0x80105EE8u) * 0x41C64E6Du + 12345u;
  c->mem_w32(0x80105EE8u, s);
  c->r[2] = (s >> 16) & 0x7FFFu;
}

// 0x8009A480 — store a0 to the global word at 0x80105EE8 (0x8010<<16 + 0x5EE8).
static void ov_8009A480(Core* c) { c->mem_w32(0x80105EE8u, c->r[4]); }

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

