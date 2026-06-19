// Hand-written native C++ — batch A3 of the boot/engine path (Tomba2Engine).
//
// Same discipline as engine/native_path.cpp: each function below is reverse-engineered from the
// recompiler's own decode (generated/gen_func_<addr>) — that decode is the authoritative reference,
// NOT raw capstone. Trailing duplicate `return;` and any code after the real `jr ra` are the
// recompiler over-running into the next function and are NOT part of the body. Branch delay-slot
// register writes (an assignment that sits on the same source line as `if (...) goto`) execute on
// BOTH the taken and not-taken paths and are handled as such.
//
// Global addresses are computed from `(uint32_t)NNNNu<<16 + signed-off` and written as absolute
// literals with the derivation in a comment. Common bases:
//   32778<<16 = 0x800A0000   32779<<16 = 0x800B0000   32780<<16 = 0x800C0000
//   32783<<16 = 0x800F0000   32784<<16 = 0x80100000
#include "core.h"
#include <stdint.h>

// 0x80075E04 — register an entry in a 24-slot, stride-12 priority table and (if a slot was chosen)
// fill its 8 bytes. The table base is 0x800BE238; the live start index is the global at 0x800BED78
// (32780<<16 - 4744). For each index i in [start,24): the candidate row is base+i*12, and a parallel
// row base+i*12+1 is examined at +7 (key byte). The search keeps the best-priority slot (r11) using
// a0=key match plus the (a1-low / 254) comparisons on the row's byte[0]. When a slot is found it is
// populated from a1..a3 and four stack args (orig caller frame), and v0 = (slotword & ~0xFF) | savedR10.
// Uses an 8-byte stack scratch (sp-8): word[0] holds the captured r10 written back into v0's low byte.
static void ov_80075E04(Core* c) {
  uint32_t sp = c->r[29] - 8;                       // local frame (no real prologue; just scratch)
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
  int32_t r12 = (int32_t)a1;                          // running "best byte[0]" lower bound
  int32_t r13 = 254;                                  // running "best byte[0]" upper bound
  int32_t i = (int32_t)c->mem_r32(0x800BED78u);       // start index
  uint32_t row = 0x800BE238u + (uint32_t)i * 12u;     // &table[i]
  uint32_t chosen = 0;                                // r11 (0 = none yet)
  uint32_t sp0 = 0;                                   // sp+0 scratch (captured r10 for v0 low byte)

  if (i < 24) {
    uint32_t key = row + 1;                            // r9 = row + 1 (key bytes live at +7 of row+1)
    for (; i < 24; i++, key += 12, row += 12) {
      if (a0 != c->mem_r8(key + 7)) continue;          // key mismatch → skip
      uint32_t b = c->mem_r8(key + 0);                 // candidate byte[0]
      if (!((int32_t)b < r12)) {                       // b >= r12: tighten only if it also passes
        if (b != (uint32_t)r12) continue;              // strictly greater (and != lower) → skip
        uint32_t rb = c->mem_r8(row + 0);
        if (r13 < (int32_t)rb) continue;               // row byte[0] exceeds current upper → skip
      }
      // accept this slot as the new best
      chosen = row;
      sp0 = (uint32_t)i;                                // save current loop index r10 to scratch
      r12 = (int32_t)c->mem_r8(key + 0);
      r13 = (int32_t)c->mem_r8(row + 0);
    }
  }

  if (chosen == 0) { c->r[2] = 0; c->r[29] = sp + 8; return; }
  // populate the chosen slot
  c->mem_w8(chosen + 2, (uint8_t)a2);
  c->mem_w8(chosen + 3, (uint8_t)a3);
  c->mem_w8(chosen + 4, (uint8_t)c->mem_r32(sp + 24));   // stack arg @ caller sp+16
  c->mem_w8(chosen + 5, (uint8_t)c->mem_r32(sp + 28));   //                    +20
  c->mem_w8(chosen + 6, (uint8_t)c->mem_r32(sp + 32));   //                    +24
  uint32_t arg = c->mem_r32(sp + 36);                    //                    +28
  c->mem_w8(chosen + 1, (uint8_t)a1);
  c->mem_w8(chosen + 7, (uint8_t)arg);
  c->mem_w8(chosen + 0, (uint8_t)255);
  uint32_t word = c->mem_r32(chosen + 0);
  c->r[2] = (word & 0xFFFFFF00u) | sp0;                  // r4 = -256 = ~0xFF; OR the saved index (r3)
  c->r[29] = sp + 8;
}

// 0x800962B0 — bind a runtime "actor/voice" record (id a0, slot a1). Bounds: low16(a0) must be <16
// and the per-id type byte at 0x80105D18 (32784<<16 + 23832) must equal 1, and a1 (as s16) must be
// < the global s16 at 0x80105CDA (+23770). On success it copies three table words keyed by id*4 into
// the scratch globals at 0x80105CE8/CE4/CDC, records a0/a1 bytes at 0x80105CF9/CFE, derefs the slot
// record and stores its byte[8] at 0x80105CFF. On any failure: v0 = -1.
static void ov_800962B0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t r7 = a0, r8 = a1;
  if ((a0 & 0xFFFFu) >= 16u) { c->r[2] = (uint32_t)-1; return; }
  int32_t id = (int32_t)(int16_t)(uint16_t)a0;                 // sign-extend low16 of a0
  uint32_t typ = c->mem_r8(0x80105D18u + (uint32_t)id);        // +23832
  if (typ != 1u) { c->r[2] = (uint32_t)-1; return; }
  int32_t slot = (int32_t)(int16_t)(uint16_t)a1;               // sign-extend low16 of a1
  int32_t lim  = (int32_t)(int16_t)c->mem_r16(0x80105CDAu);    // +23770
  if (!(slot < lim)) { c->r[2] = (uint32_t)-1; return; }       // slot >= limit → fail

  uint32_t off = (uint32_t)id << 2;                            // id*4
  uint32_t v23632 = c->mem_r32(0x80105C50u + off);             // +23632
  uint32_t v23568 = c->mem_r32(0x80105C10u + off);             // +23568
  uint32_t v23704 = c->mem_r32(0x80105C98u + off);             // +23704
  c->mem_w8(0x80105CF9u, (uint8_t)r7);                         // +23801
  c->mem_w8(0x80105CFEu, (uint8_t)r8);                         // +23806 (= 23801 + 5)
  c->mem_w32(0x80105CE8u, v23704);                             // +23784
  uint32_t ent = ((uint32_t)slot << 4) + v23568;              // slot*16 + base
  c->mem_w32(0x80105CE4u, v23632);                             // +23780
  c->mem_w32(0x80105CDCu, v23568);                             // +23772
  uint32_t b = c->mem_r8(ent + 8);
  c->mem_w8(0x80105CFFu, (uint8_t)b);                          // +23807 (= 23801 + 6)
  c->r[2] = 0;                                                 // falls through (callers ignore v0 on success)
}

// 0x80092FD0 — refresh the active "channel/voice" hardware-shadow tables from the two indices stored
// at 0x80105D70/72 (32784<<16 + 23824/23822). Clears a 16-word bitmask block at 0x80105B70 (+23472)
// then, depending on parity of the index at +23822, reads a 16-byte record (table @ +23772, stride16)
// to update u16 shadow tables at +23086 / +23088, OR-flags bytes at +23048, and clamps a pan/spread
// field into +23090. Pure table shuffling; no external calls. See per-line comments for the maps.
static void ov_80092FD0(Core* c) {
  const uint32_t B = 0x80100000u;
  uint32_t r6 = B + 23824;                                  // &index0 (u16)
  uint16_t idx0 = (uint16_t)c->mem_r16(r6);
  uint32_t r8 = (uint32_t)idx0 << 3;                         // idx0*8 (used later, shifted)
  int32_t sidx0 = (int32_t)(int16_t)idx0;
  uint32_t r3 = ((uint32_t)((sidx0 << 3) - sidx0)) << 3;     // idx0*7*8 = idx0*56
  c->mem_w16(B + 21710 + r3, (uint16_t)32767u);             // seed clamp field for this index

  // clear 16 mask words at +23472
  uint32_t p = B + 23472;
  for (int n = 0; n < 16; n++) {
    int32_t s = (int32_t)(int16_t)c->mem_r16(r6);           // re-read idx0 each pass (matches body)
    uint32_t v = c->mem_r32(p);
    v &= ~(1u << ((uint32_t)s & 31));
    c->mem_w32(p, v);
    p += 4;
  }

  uint32_t r5 = B + 23822;                                   // &index1 (u16)
  uint16_t idx1 = (uint16_t)c->mem_r16(r5);
  uint32_t recBase = c->mem_r32(B + 23772);                  // record table base (+23772)
  uint16_t fieldA;
  // even/odd half-select within a stride-16 record chosen by ((idx1<<16>>16)-1)/2
  {
    int32_t s = (int32_t)(int16_t)idx1;
    int32_t half = (s - 1);
    half = (half + (int32_t)((uint32_t)half >> 31)) >> 1;    // arithmetic /2 toward 0
    uint32_t rec = ((uint32_t)half << 4) + recBase;
    fieldA = (uint16_t)c->mem_r16(rec + ((idx1 & 1) ? 12 : 14));
  }
  uint32_t r4hi = (r8 << 16);
  uint32_t r4 = (uint32_t)((int32_t)r4hi >> 15);             // idx0*8 *2 (sign), table stride
  c->mem_w16(B + 23086 + r4, fieldA);                        // shadow table A

  int32_t fl = (int32_t)(int16_t)c->mem_r16(r5 + 2);         // flag-index at +23824 (r5+2)
  {
    uint32_t fp = B + 23048 + (uint32_t)fl;
    c->mem_w8(fp, (uint8_t)(c->mem_r8(fp) | 8u));
  }

  uint32_t r7 = B + 23807;                                   // small 2-byte id pair @ +23807/23812
  uint32_t r6b = (uint32_t)((int32_t)(r8 << 16) >> 15);
  int32_t e0 = (int8_t)c->mem_r8(r7 + 0);
  int32_t e5 = (int8_t)c->mem_r8(r7 + 5);
  uint32_t ent = (uint32_t)((e0 << 4) + e5);
  uint32_t tbl2 = c->mem_r32(B + 23784);                     // second record base (+23784)
  uint32_t rec2 = (ent << 5) + tbl2;                         // stride 32
  uint16_t fieldB = (uint16_t)c->mem_r16(rec2 + 16);
  c->mem_w16(B + 23088 + r6b, fieldB);                       // shadow table B

  uint16_t r4w = (uint16_t)c->mem_r16(rec2 + 18);
  int32_t base = (int32_t)(int16_t)(uint16_t)c->mem_r16(B + 23696);
  int32_t sum = base + (int32_t)(r4w & 31u);
  int32_t r3v = sum;
  if (!((int32_t)(int16_t)(uint16_t)sum < 32)) r3v = 31;     // clamp the 5-bit field to 31
  uint32_t packed = (uint32_t)r3v | (r4w & 0xFFE0u);         // keep high bits, replace low5
  c->mem_w16(B + 23088 + r6b + 2, (uint16_t)packed);         // +2 of shadow table B slot

  int32_t fl2 = (int32_t)(int16_t)c->mem_r16(r7 + 17);       // second flag-index
  {
    uint32_t fp = B + 23048 + (uint32_t)fl2;
    c->mem_w8(fp, (uint8_t)(c->mem_r8(fp) | 48u));
  }
}

// 0x80094474 — fixed-point helper: given base (a0), step (a1), key (a2 low byte), wrap (a3 low byte),
// returns a 16-bit interpolated value from two pitch/scale tables at 0x800B0000 (-15260 and -15236).
// Steps: fold a3low+a1 into a /128 octave (r3) and a 0..127 remainder (r8); subtract a2low; compute
// (r5 % 12) via the magic-multiply reciprocal (0x2AAAAAAB) to get a note index r6 and semitone r5;
// look up two s16 table entries, multiply, then a barrel-shift/round by (r6-? ) producing the result.
// Pure arithmetic, returns v0 = result & 0xFFFF.
static void ov_80094474(Core* c) {
  int32_t a0 = (int32_t)c->r[4];
  int32_t a1 = (int32_t)c->r[5];
  uint32_t a2 = c->r[6], a3 = c->r[7];

  int32_t r7 = (int32_t)(int16_t)(uint16_t)((a3 & 0xFFu) + a1);   // (a3low + a1) as s16
  int32_t r3 = (r7 >= 0) ? r7 : (r7 + 127);
  r3 >>= 7;                                                       // floor-div toward 0 by 128
  int32_t r4 = a0 + r3 - (int32_t)(a2 & 0xFFu);
  int32_t r5 = r4;
  int32_t rem = r7 - (r3 << 7);                                   // r7 mod 128 (signed)
  int32_t r8;
  if ((int32_t)(rem << 16) >= 0) {                                // s16(rem) >= 0
    r8 = rem;
  } else {
    int32_t t = rem + 128;
    r8 = t;
    int32_t ts = (int32_t)(int16_t)(uint16_t)t;
    r4 = r4 - 1;
    int32_t q = (ts >= 0) ? ts : (ts + 127);
    q >>= 7;
    r5 = r4 + q;
  }

  // r5 % 12 and r5 / 12 via reciprocal 0xAAAB2AAB (0x2AAAAAAB sign-extended pattern in body)
  int32_t r4b = (int32_t)(int16_t)(uint16_t)r5;                   // sign-extend low16 of r5
  int32_t recip = (int32_t)((10922u << 16) | 43691u);            // 0x2AAAAAAB
  int64_t prod = (int64_t)r4b * (int64_t)recip;
  int32_t hi = (int32_t)((uint64_t)prod >> 32);
  int32_t sgn = (int32_t)(((int32_t)(r5 << 16)) >> 31);
  int32_t r3b = (hi >> 1) - sgn;                                  // r5 / 12
  int32_t r6 = r3b - 2;                                           // note octave index (default)
  int32_t r4c = r4b - (((r3b << 1) + r3b) << 2);                  // r4b - r3b*12 = r5 % 12
  if ((int32_t)(r4c << 16) < 0) {                                 // negative remainder → wrap
    r4c += 12;
    r6 = r3b - 3;
  }

  int32_t r5b = (int32_t)((r4c << 16)) >> 15;                     // r4c * 2 (table stride, s16)
  int32_t r2b = (int32_t)((r8  << 16)) >> 15;                     // r8  * 2 (table stride, s16)
  uint32_t t0 = c->mem_r16(0x800AC464u + (uint32_t)r5b);          // 0x800B0000 - 15260
  uint32_t t1 = c->mem_r16(0x800AC47Cu + (uint32_t)r2b);          // 0x800B0000 - 15236
  int64_t prod2 = (int64_t)(int32_t)t0 * (int64_t)(int32_t)t1;
  int32_t lo = (int32_t)(uint32_t)prod2;
  int32_t r2c = (int32_t)(int16_t)(uint16_t)r6;                   // sign-extend low16 of r6

  int32_t result;
  if (r2c < 0) {
    int32_t r5c = lo >> 16;
    int32_t shl = -r2c;                                           // r0 - r2
    int32_t shd = shl - 1;
    int32_t add = (int32_t)(1u << ((uint32_t)shd & 31));
    r5c += add;
    result = (int32_t)((uint32_t)r5c >> ((uint32_t)shl & 31));
  } else {
    result = 16383;
  }
  c->r[2] = (uint32_t)result & 0xFFFFu;
}

// 0x800782F0 — set/queue a sound-bank request. a0 (low byte) selects a sub-bank (must be <9); a1
// (low byte) indexes a stride-8 record inside it. Reads a u16 attr (+6) → extracts a 2-bit mode
// (bits 9..10) added to a per-bank base byte (table @ 0x800A55B0), then OR-sets one bit in the
// 32-bit pending mask at 0x800BFE50 (32780<<16 + 1504, via base 0x800BF870+1504). Afterwards, based
// on the bank's status byte at 0x800BF870 (==5/6/7/8) it OR-flags one of bits {2,4,8,16} into the
// status byte at 0x800BF870+363. Real final return is after the status switch.
static void ov_800782F0(Core* c) {
  uint32_t a0 = c->r[4] & 0xFFu;
  uint32_t a1 = c->r[5];
  if (a0 < 9u) {
    uint32_t tabPtr = c->mem_r32(0x800A54A8u + (a0 << 2));     // 0x800A0000 + 21672 + a0*4
    uint32_t rec = tabPtr + ((a1 & 0xFFu) << 3);              // stride-8 record
    uint32_t attr = c->mem_r16(rec + 6);
    uint32_t modeBase = c->mem_r8(0x800A55B0u + a0);          // 0x800A0000 + 21936 + a0
    uint32_t bit = modeBase + ((attr & 1536u) >> 9);          // +mode (bits 9..10)
    uint32_t maskAddr = 0x800BFE50u;                          // 0x800C0000 - 1936 + 1504
    uint32_t m = c->mem_r32(maskAddr);
    m |= (1u << (bit & 31));
    c->mem_w32(maskAddr, m);
  }
  uint32_t statusBase = 0x800BF870u;                          // 0x800C0000 - 1936
  uint32_t st = c->mem_r8(statusBase);
  uint32_t flagAddr = statusBase + 363;
  if (st == 5u)      c->mem_w8(flagAddr, (uint8_t)(c->mem_r8(flagAddr) | 2u));
  else if (st == 6u) c->mem_w8(flagAddr, (uint8_t)(c->mem_r8(flagAddr) | 4u));
  else if (st == 7u) c->mem_w8(flagAddr, (uint8_t)(c->mem_r8(flagAddr) | 8u));
  else if (st == 8u) c->mem_w8(flagAddr, (uint8_t)(c->mem_r8(flagAddr) | 16u));
}

// 0x800508A8 — propagate a 4-byte RGB+flag tuple selected by the current bank state into three
// destination color records. Reads a bit (selected by the bank's status byte at 0x800BF870) out of
// the u16 at 0x800BF870+1510, then indexes a 4-entry color table at 0x800A5500 (32778<<16 + 21760)
// keyed by (status*8 + bit*4). Fetches 4 bytes (r6,r5,r4,r7) and scatters them into records at
// 0x800E80A8 (+16540 / +8236) and +16516/+8212 areas plus the bank record at +52..55. The status
// byte at +55 becomes 129 if r6low==1 else 2.
static void ov_800508A8(Core* c) {
  uint32_t bankBase = 0x800BF870u;                            // 0x800C0000 - 1936
  uint32_t status = c->mem_r8(bankBase);                      // r3
  uint32_t attr = c->mem_r16(bankBase + 1510);
  uint32_t bit = ((attr >> (status & 31)) & 1u) << 2;        // r2 = selected bit *4
  uint32_t colBase = 0x800A5500u;                            // 0x800A0000 + 21760
  uint32_t s8 = status << 3;
  uint32_t e0 = colBase + bit + s8;                          // r7
  uint32_t e1 = colBase + bit + (s8 + 1);                    // r5
  uint32_t e2 = colBase + bit + (s8 + 2);                    // r4
  uint32_t e3 = colBase + bit + (s8 + 3);                    // r2->r7
  uint32_t r6 = c->mem_r8(e0);
  uint32_t r5 = c->mem_r8(e1);
  uint32_t r4 = c->mem_r8(e2);
  uint32_t r7 = c->mem_r8(e3);

  uint32_t base = 0x800E80A8u;                               // 0x800F0000 - 32600
  c->mem_w8(base + 16540, (uint8_t)r6);
  c->mem_w8(base + 8236,  (uint8_t)r6);
  uint32_t recA = base + 8212;                               // r2
  uint32_t recB = base + 16516;                              // r3
  uint32_t r6low = r6 & 0xFFu;
  c->mem_w8(recA + 25, (uint8_t)r5);
  c->mem_w8(recA + 26, (uint8_t)r4);
  c->mem_w8(recA + 27, (uint8_t)r7);
  c->mem_w8(recB + 25, (uint8_t)r5);
  c->mem_w8(recB + 26, (uint8_t)r4);
  c->mem_w8(recB + 27, (uint8_t)r7);                          // delay slot: always stored
  uint32_t flag = (r6low != 1u) ? 2u : 129u;
  c->mem_w8(bankBase + 55, (uint8_t)flag);
  c->mem_w8(bankBase + 52, (uint8_t)r5);
  c->mem_w8(bankBase + 53, (uint8_t)r4);
  c->mem_w8(bankBase + 54, (uint8_t)r7);
}

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

// 0x8007A12C — link a sound/voice node into one of several intrusive doubly-linked lists. a0 is the
// node (r9); a3 (r7) selects which head-table set (1, <2, ==2), a2 (r6) selects an insert mode
// (1/<2/==2/==3). The free-list head is the global at 0x800ED8D4 (32783<<16 - 10028); the alloc count
// byte at 0x800ED8C5 is decremented. Heads live in fixed globals at 0x800F-region offsets. The node's
// next/prev are at +32/+36; its type/key bytes at +0/+10/+12. Returns v0 = node ptr (or 0 if the free
// list was empty). Many list permutations — kept structurally identical to the decoded control flow.
// node fields: +32 = "next" link, +36 = "prev" link.  r4 = list-array head pointer cell,
// r3 = secondary head-cell.  These two link helpers mirror L_8007A214 / L_8007A27C.
static void a12c_link_front(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  c->mem_w32(node + 32, 0);
  uint32_t old = c->mem_r32(r4);
  c->mem_w32(node + 36, old);
  if (old == 0) c->mem_w32(r3, node);        // empty list → head-cell points at node
  else          c->mem_w32(old + 32, node);
  c->mem_w32(r4, node);
}
static void a12c_link_back(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  uint32_t old = c->mem_r32(r3);
  c->mem_w32(node + 36, 0);
  c->mem_w32(node + 32, old);
  if (old == 0) c->mem_w32(r4, node);
  else          c->mem_w32(old + 36, node);
  c->mem_w32(r3, node);
}
static void ov_8007A12C(Core* c) {
  uint32_t r9 = c->r[4];                     // a0: source/reference node
  uint32_t a2 = c->r[6], a3 = c->r[7];       // r6 = insert mode, r7 = list-select
  uint32_t r8 = c->mem_r32(0x800ED8D4u);     // free-list head (32783<<16 - 10028)
  if (r8 == 0) { c->r[2] = 0; return; }      // nothing free → return 0

  // pop the free node: free-head = freenode->prev(+36), dec the count byte at -10043
  uint8_t cnt = (uint8_t)c->mem_r8(0x800ED8C5u);
  uint32_t newHead = c->mem_r32(r8 + 36);
  c->mem_w8(0x800ED8C5u, (uint8_t)(cnt - 1));
  c->mem_w32(0x800ED8D4u, newHead);

  // choose the head-pointer pair (r4 = list array, r3 = secondary head-cell) by a3.
  // a3==1 → set1; a3==2 → set2; everything else (0 or >2) → set0.
  uint32_t r4, r3;
  if (a3 == 1u) {
    r4 = 0x800F2624u;                        // 0x800F0000 + 9764
    r3 = 0x800F239Cu;                        // 0x800F0000 + 9116
  } else if (a3 == 2u) {
    r4 = 0x800F2738u;                        // 0x800F0000 + 10040
    r3 = 0x800F23A0u;                        // 0x800F0000 + 9120
  } else {                                   // a3 == 0 (and any a3 > 2)
    r4 = 0x800FB168u;                        // 0x80100000 - 20120
    r3 = 0x800F23A8u;                        // 0x800F0000 + 9128
  }

  // insert mode by a2 (decoded: a2==0 → front-link; a2==1 → after r9->next; a2==2 → back-link;
  // a2==3 → after r9->prev; any other a2 → no linking, just stamp the bytes).
  if (a2 == 0u) {
    a12c_link_front(c, r8, r4, r3);
  } else if (a2 == 1u) {
    uint32_t nxt = c->mem_r32(r9 + 32);
    if (nxt == 0) {                           // no successor → fall back to front-link (L_8007A214)
      a12c_link_front(c, r8, r4, r3);
    } else {                                  // splice r8 between r9 and r9->next
      c->mem_w32(r8 + 32, nxt);
      c->mem_w32(r8 + 36, r9);
      c->mem_w32(c->mem_r32(r9 + 32) + 36, r8);
      c->mem_w32(r9 + 32, r8);
    }
  } else if (a2 == 2u) {
    a12c_link_back(c, r8, r4, r3);
  } else if (a2 == 3u) {
    uint32_t prv = c->mem_r32(r9 + 36);
    if (prv == 0) {                           // no predecessor → fall back to back-link (L_8007A27C)
      a12c_link_back(c, r8, r4, r3);
    } else {                                  // splice r8 between r9->prev and r9
      c->mem_w32(r8 + 32, r9);
      c->mem_w32(r8 + 36, prv);
      c->mem_w32(c->mem_r32(r9 + 36) + 32, r8);
      c->mem_w32(r9 + 36, r8);
    }
  }
  // else: a2 not in {0,1,2,3} → no list mutation (matches the L_8007A2B0 fall-through).

  // stamp the node header bytes and return the node pointer
  c->mem_w8(r8 + 10, (uint8_t)a3);
  c->mem_w8(r8 + 0,  (uint8_t)2u);            // type byte = 2 on every path that reaches the stamp
  c->mem_w8(r8 + 12, (uint8_t)c->r[5]);       // a1 → key byte
  c->r[2] = r8;
}

// 0x80094C10 — large fixed-point mixer/pan computation for the current voice (record at +23780).
// Reads a byte at record+24, runs it through a chain of magic-multiply reciprocal divides to derive
// distance/attenuation factors, then resolves per-channel pan/volume into the channel-shadow u16
// tables and the GPU/SPU pending-mask bitfields at 0x80105xxx (32784<<16 + 215xx/235xx). Entirely
// arithmetic + table I/O; no calls. The 0x1040<<16|16645 (=0x10404105) etc. constants are the C
// reciprocal magic numbers the compiler emitted for divides by small constants.
static void ov_80094C10(Core* c) {
  const uint32_t B = 0x80100000u;
  uint32_t rec = c->mem_r32(B + 23780);                       // current voice record
  uint32_t tbl = B + 23824;                                   // small param block base (r8)

  uint32_t v24 = c->mem_r8(rec + 24);                         // r4
  int32_t  p_20 = (int8_t)c->mem_r8(tbl - 20);                // signed param @ tbl-20
  // r2 = v24*16383
  uint32_t r2 = (v24 << 14) - v24;
  int64_t pr = (int64_t)p_20 * (int64_t)(int32_t)r2;
  int32_t r3 = (int32_t)(uint32_t)pr;                          // lo
  // r3 * 0x10204137  (33286<<16|4137)
  int32_t m1 = (int32_t)((33286u << 16) | 4137u);
  int64_t pr2 = (int64_t)r3 * (int64_t)m1;
  int32_t hi2 = (int32_t)((uint64_t)pr2 >> 32);
  int32_t p_14 = (int8_t)c->mem_r8(tbl - 14);
  int32_t r2b = (hi2 + r3) >> 13;
  int32_t r3sgn = r3 >> 31;
  int32_t r7 = r2b - r3sgn;
  int64_t pr3 = (int64_t)r7 * (int64_t)p_14;
  int32_t r3c = (int32_t)(uint32_t)pr3;                        // lo
  int32_t p_11 = (int8_t)c->mem_r8(tbl - 11);
  int64_t pr4 = (int64_t)r3c * (int64_t)p_11;
  int32_t r2c = (int32_t)(uint32_t)pr4;                        // lo

  // r2c * 0x040C2051 (1036<<16|8273) unsigned → hi
  uint32_t m2 = (1036u << 16) | 8273u;
  uint64_t pr5 = (uint64_t)(uint32_t)r2c * (uint64_t)m2;
  uint32_t hi5 = (uint32_t)(pr5 >> 32);
  int32_t f0 = (int32_t)(int16_t)c->mem_r16(tbl + 0);         // r3
  uint32_t r9 = c->r[5];                                       // a1 carried to shadow store
  uint32_t r10 = (uint32_t)f0 << 3;                            // index into shadow tables
  // r6 = ((r2c - hi5)>>1 + hi5) >> 13
  uint32_t r4d = (uint32_t)r2c - hi5;
  r4d >>= 1;
  uint32_t r6 = (hi5 + r4d) >> 13;

  uint32_t f_4 = c->mem_r16(tbl - 4);                         // r4
  uint32_t r5 = (f_4 & 0xFFu) << 2;
  int32_t f_4s = (int32_t)(int16_t)(uint16_t)f_4;
  int32_t r3d = (f_4s & 0xFF00) >> 8;
  int32_t r2d = (((r3d << 1) + r3d) << 2) - r3d;             // r3d*11
  uint32_t recL = c->mem_r32(B + 19504 + r5);                 // table @ +19504, stride from r5
  recL += (uint32_t)(r2d << 4);
  uint32_t r7b;
  if (f_4s == 33) {
    r7b = r6;
  } else {
    // two more reciprocal divides producing r7b (left) and r6 (right) pan factors
    uint32_t v88 = c->mem_r16(recL + 88);
    int64_t q1 = (int64_t)(int32_t)r6 * (int64_t)(int32_t)v88;
    uint32_t lo1 = (uint32_t)(uint32_t)(int64_t)q1;
    uint32_t v90 = c->mem_r16(recL + 90);
    int64_t q2 = (int64_t)(int32_t)r6 * (int64_t)(int32_t)v90;
    uint32_t lo2 = (uint32_t)(int64_t)q2;
    uint32_t m3 = (516u << 16) | 2065u;                       // 0x02040811
    uint64_t d1 = (uint64_t)lo1 * (uint64_t)m3; uint32_t h1 = (uint32_t)(d1 >> 32);
    uint64_t d2 = (uint64_t)lo2 * (uint64_t)m3; uint32_t h2 = (uint32_t)(d2 >> 32);
    r7b = (h1 + ((lo1 - h1) >> 1)) >> 6;
    r6  = (h2 + ((lo2 - h2) >> 1)) >> 6;
  }

  // distance scale by tbl-10 (<64: scale; else 127-val complement)
  uint32_t d10 = c->mem_r8(tbl - 10);
  uint32_t r4e, r5e;
  uint32_t m4 = (1040u << 16) | 16645u;                       // 0x10404105
  if (d10 < 64u) {
    int64_t q = (int64_t)(int32_t)r6 * (int64_t)(int32_t)d10; uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r5e = (h + ((lo - h) >> 1)) >> 5;
    r4e = r7b;
  } else {
    int64_t q = (int64_t)(int32_t)r7b * (int64_t)(int32_t)(127 - (int32_t)d10); uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r4e = (h + ((lo - h) >> 1)) >> 5;
    r5e = r6;
  }

  // second distance scale by byte @ +23811
  uint32_t d2v = c->mem_r8(B + 23811);
  if (d2v < 64u) {
    int64_t q = (int64_t)(int32_t)r5e * (int64_t)(int32_t)d2v; uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r5e = (h + ((lo - h) >> 1)) >> 5;
  } else {
    int64_t q = (int64_t)(int32_t)r4e * (int64_t)(int32_t)(127 - (int32_t)d2v); uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r4e = (h + ((lo - h) >> 1)) >> 5;
  }

  // third distance scale by byte @ +23805
  uint32_t d3v = c->mem_r8(B + 23805);
  if (d3v < 64u) {
    int64_t q = (int64_t)(int32_t)r5e * (int64_t)(int32_t)d3v; uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r5e = (h + ((lo - h) >> 1)) >> 5;
  } else {
    int64_t q = (int64_t)(int32_t)r4e * (int64_t)(int32_t)(127 - (int32_t)d3v); uint32_t lo = (uint32_t)(int64_t)q;
    uint64_t d = (uint64_t)lo * (uint64_t)m4; uint32_t h = (uint32_t)(d >> 32);
    r4e = (h + ((lo - h) >> 1)) >> 5;
  }

  // pick max(r4e,r5e) when the mode word @ +23768 == 1
  int32_t mode = (int32_t)(int16_t)c->mem_r16(B + 23768);
  if (mode == 1) {
    if (r4e < r5e) r4e = r5e; else r5e = r4e;
  }

  // when the slot word @ +23820 != 33, blend the two squares
  int32_t slot = (int32_t)(int16_t)c->mem_r16(B + 23820);
  if (slot != 33) {
    int64_t s4 = (int64_t)(int32_t)r4e * (int64_t)(int32_t)r4e; uint32_t lo4 = (uint32_t)(int64_t)s4;
    int64_t s5 = (int64_t)(int32_t)r5e * (int64_t)(int32_t)r5e; uint32_t lo5 = (uint32_t)(int64_t)s5;
    uint32_t m5 = (4u << 16) | 17u;                            // 0x00040011
    uint64_t d4 = (uint64_t)lo4 * (uint64_t)m5; uint32_t h4 = (uint32_t)(d4 >> 32);
    uint64_t d5 = (uint64_t)lo5 * (uint64_t)m5; uint32_t h5 = (uint32_t)(d5 >> 32);
    r4e = (h4 + ((lo4 - h4) >> 1)) >> 13;
    r5e = (h5 + ((lo5 - h5) >> 1)) >> 13;
  }

  // store final left/right into the channel shadow tables at +23084/23080/23082, keyed by r10
  uint32_t r2k = (r10 & 0xFFFFu) << 1;
  c->mem_w16(B + 23084 + r2k, (uint16_t)r9);
  c->mem_w16(B + 23080 + r2k, (uint16_t)r4e);
  c->mem_w16(B + 23082 + r2k, (uint16_t)r5e);

  // OR-flag the per-channel attr byte (key word @ +23824 = slot block r6) at +23048, bits 7
  int32_t key = (int32_t)(int16_t)c->mem_r16(tbl + 4);
  {
    uint32_t fp = B + 23048 + (uint32_t)key;
    c->mem_w8(fp, (uint8_t)(c->mem_r8(fp) | 7u));
  }
  // seed clamp field @ +21708 keyed by key*56
  {
    int32_t k = (int32_t)(int16_t)c->mem_r16(tbl + 4);
    uint32_t off = (uint32_t)(((k << 3) - k) << 3);           // k*56
    c->mem_w16(B + 21708 + off, (uint16_t)r9);
  }

  // build the channel bit-pair (r6=low mask, r5=high mask) from key, then update the SPU voice masks.
  int32_t key2 = (int32_t)(int16_t)c->mem_r16(tbl + 4);
  uint32_t r6m, r5m;
  if (key2 < 16) { r6m = (1u << ((uint32_t)key2 & 31)); r5m = 0; }
  else           { r6m = 0; r5m = (1u << (((uint32_t)key2 - 16) & 31)); }

  uint32_t st = c->mem_r8(tbl - 6);                            // +23818 status byte
  if (st & 4u) {
    // set bits in the "key on"/active masks at +21692/21694
    uint32_t a = c->mem_r16(B + 21692) | r6m;
    uint32_t b2 = c->mem_r16(B + 21694) | r5m;
    c->mem_w16(B + 21692, (uint16_t)a);
    c->mem_w16(B + 21694, (uint16_t)b2);
  } else {
    // clear them
    uint32_t a = c->mem_r16(B + 21692) & ~r6m;
    c->mem_w16(B + 21692, (uint16_t)a);
    uint32_t b2 = c->mem_r16(B + 21694) & ~r5m;
    c->mem_w16(B + 21694, (uint16_t)b2);
  }

  // clear the corresponding bits in the secondary mask pairs at +21696/21698/21690/21688 + 23536/23538
  uint32_t m21696 = c->mem_r16(B + 21696) & ~r6m;
  uint32_t m21690v = c->mem_r16(B + 21690);
  c->mem_w16(B + 21696, (uint16_t)m21696);
  uint32_t m21698 = c->mem_r16(B + 21698) & ~r5m;
  uint32_t m21688v = c->mem_r16(B + 21688);
  m21690v |= r5m;
  c->mem_w16(B + 21690, (uint16_t)m21690v);
  c->mem_w16(B + 21698, (uint16_t)m21698);
  uint32_t m23536 = c->mem_r16(B + 23536);
  m21688v |= r6m;
  c->mem_w16(B + 21688, (uint16_t)m21688v);
  m23536 &= ~m21688v;
  c->mem_w16(B + 23536, (uint16_t)m23536);
  uint32_t m23538 = c->mem_r16(B + 23538) & ~m21690v;
  c->mem_w16(B + 23538, (uint16_t)m23538);
}

// Register every batch-A3 native override. Called from games_tomba2_init alongside the other
// games_native_path_*_init runners.
void games_native_path_a3_init(void) {
  rec_set_override(0x80075E04u, ov_80075E04);
  rec_set_override(0x800962B0u, ov_800962B0);
  rec_set_override(0x80092FD0u, ov_80092FD0);
  rec_set_override(0x80094474u, ov_80094474);
  // TODO(verify): rec_set_override(0x80094C10u, ov_80094C10); — DISABLED: A/B RAM-diff fails (15 bytes vs interp). fixed-point mixer/pan — densest, reciprocal-magic divides. Re-enable after fixing gen_func_80094C10 + re-A/B.
  rec_set_override(0x800782F0u, ov_800782F0);
  rec_set_override(0x800508A8u, ov_800508A8);
  // TODO(verify): rec_set_override(0x80077FB0u, ov_80077FB0); — DISABLED: A/B RAM-diff fails (4 bytes vs interp). 16-bit integer sqrt — wrong result (cascades into 0x800E806x). Re-enable after fixing gen_func_80077FB0 + re-A/B.
  rec_set_override(0x80085690u, ov_80085690);
  rec_set_override(0x8007A12Cu, ov_8007A12C);
}
