// engine/sound_voice.cpp — PC-native sound/voice helpers (channel/voice attribute & pan/volume tables, key-on/off, sound-bank requests).

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

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

// ── 0x80090A60 — channel/voice attribute setup. a0=ch (r4, &0xFFFF), a1=val (r5), a2=flags (r6). If
// ch<3 (signed) return 0. Table base = *0x800AC448 (=0x800B0000-15288), entry = base+ch*16. Writes
// entry+4 = 0, entry+8 = (u16)a1, then the attribute word: 72 base, |16 if (flags&4096) → store to
// entry+4. NOTE: the body has branch arms for ch<2 / ch==2 (r7=73/584, |256), but those are STRUCTURALLY
// UNREACHABLE once ch>=3 (the `(ch<2)` test fails → goes straight to L_80090AB8, and `ch!=2` is always
// true → L_80090AD4); so for every value the override is ever called with, r7 = (flags&0x1000)?88:72.
// (Everything after the L_80090AF4 `return` is the NEXT function — dropped.)
static void ov_80090A60(Core* c) {
  uint32_t ch = c->r[4] & 0xFFFFu;
  uint32_t val = c->r[5], flags = c->r[6];
  if ((int32_t)ch < 3) { c->r[2] = 0; return; }
  uint32_t base = c->mem_r32(0x800AC448u);              // *(0x800B0000-15288)
  uint32_t e = base + (ch << 4);
  c->mem_w16(e + 4, 0);
  c->mem_w16(e + 8, (uint16_t)val);
  uint32_t r7 = 72;                                     // 0x48
  if (flags & 4096u) r7 |= 16u;                         // → 88 (0x58)
  uint32_t base2 = c->mem_r32(0x800AC448u);             // reloaded in body (same value)
  c->mem_w16(base2 + (ch << 4) + 4, (uint16_t)r7);
  c->r[2] = 0;
}

// ── 0x80091B50 — voice-object table builder. a0=base ptr (r4), a1=rows (r5, s16), a2=cols (r6, s16).
// Stores row/col counts to *0x801054B0 / *0x801054B2 (=+21680/+21682). First loop: fill `rows` pointer
// slots at 0x80104C30 (=+19504): slot[i] = base + i*cols*176  (176 = 0xB0 object stride; the multiply is
// ((((r3<<1)+r3)<<2)-r3)<<4 = (12r3-r3)<<4 = 11r3<<4 = 176*r3, with r3 advancing by `cols` each row).
// Second loop: OR a one-hot bit (1<<i) for i=a1..31 into the busy mask *0x80104C28 (=+19496). Third
// nested loop (rows × cols): for each object o = slot[row] + col*176, zero a batch of fields and set the
// 0x7F sentinels at +88/+90/+92/+94 and -1 at +34. Faithful per-field stores from the body.
static void ov_80091B50(Core* c) {
  int32_t rows = (int16_t)c->r[5];
  int32_t cols = (int16_t)c->r[6];
  uint32_t base = c->r[4];
  c->mem_w16(0x801054B0u, (uint16_t)rows);              // (+21680)
  c->mem_w16(0x801054B2u, (uint16_t)cols);             // (+21682)

  // first loop: pointer slots at 0x80104C30, slot[i] = base + (i*cols)*176
  if (rows > 0) {
    uint32_t ptab = 0x80104C30u;                        // (+19504)
    int32_t acc = 0;                                     // r3, advances by cols each iter
    for (int32_t i = 0; i < rows; i++) {
      c->mem_w32(ptab + (uint32_t)i * 4, base + (uint32_t)(acc * 176));
      acc += cols;
    }
  }

  // second loop: OR (1<<i) into busy mask for i = a1(=rows-as-s16 of r9) .. 31.
  // NOTE: r9 was a1's original (r5) value before sign cleanup; body re-derives r7 = (s16)r9.
  int32_t r7 = (int16_t)c->r[5];
  if (r7 < 32) {
    for (int32_t i = r7; i < 32; i++) {
      uint32_t m = c->mem_r32(0x80104C28u);             // (+19496)
      c->mem_w32(0x80104C28u, m | (1u << (i & 31)));
    }
  }

  // third loop: rows × cols object init. row count re-read from *0x801054B0 (s16).
  int32_t nrows = (int16_t)c->mem_r16(0x801054B0u);
  if (nrows > 0) {
    uint32_t ptab = 0x80104C30u;
    for (int32_t row = 0; row < nrows; row++) {
      int32_t ncols = (int16_t)c->mem_r16(0x801054B2u);
      uint32_t slotptr = ptab + (uint32_t)row * 4;
      uint32_t o4 = 0;                                   // r4: col*176 offset within row
      for (int32_t col = 0; col < ncols; col++) {
        uint32_t o = c->mem_r32(slotptr) + o4;           // object = slot[row] + col*176
        c->mem_w32(o + 152, 0);
        c->mem_w8 (o + 34, (uint8_t)0xFF);               // -1
        c->mem_w8 (o + 35, 0);
        c->mem_w16(o + 72, 0);
        c->mem_w16(o + 74, 0);
        c->mem_w32(o + 156, 0);
        c->mem_w32(o + 160, 0);
        c->mem_w16(o + 76, 0);
        c->mem_w32(o + 172, 0);
        c->mem_w32(o + 168, 0);
        c->mem_w32(o + 164, 0);
        c->mem_w16(o + 78, 0);
        c->mem_w16(o + 88, 127);
        c->mem_w16(o + 90, 127);
        c->mem_w16(o + 92, 127);
        c->mem_w16(o + 94, 127);
        o4 += 176;
      }
    }
  }
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

// 0x8009440C — fixed-point lookup wrapper around 0x80094474. Builds a signed offset from two byte
// params at 0x80105CFF/0x80105D04, indexes the table pointer at 0x80105CE8 to fetch (a2,a3) bytes,
// then calls 0x80094474((int16)a0, (int16)a1, a2, a3) and returns its result & 0xFFFF.
static void ov_8009440C(Core* c) {
  int32_t a0s = (int16_t)c->r[4], a1s = (int16_t)c->r[5];
  int32_t v = (int32_t)(int8_t)c->mem_r8(0x80105D04u) + ((int32_t)(int8_t)c->mem_r8(0x80105CFFu) << 4);
  int32_t off = (int32_t)((uint32_t)v << 16) >> 11;
  uint32_t ptr = c->mem_r32(0x80105CE8u) + (uint32_t)off;
  c->r[6] = c->mem_r8(ptr + 4); c->r[7] = c->mem_r8(ptr + 5);
  c->r[4] = (uint32_t)a0s; c->r[5] = (uint32_t)a1s;
  call_fn(c, 0x80094474u);
  c->r[2] &= 0xFFFFu;
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

// ── 0x80094B50 — voice key-on/key-off mask split across two 16-voice SPU register halves. Reads the
// voice index n = u16 *0x80105D10. Builds a one-hot bit: if n<16 → loset bit n (lo half), hiset=0; else
// → hiset bit (n-16) (hi half), loset=0. Clears the per-voice byte at base 0x801054E5 + n*0x38 (n*7*8).
// Updates: OR the bit into the "key-on" shadow at 0x80105BF0/2 and AND-clear it out of the "key-off"
// shadow at 0x801054B8/A (lo/hi halves respectively). Also zeroes the two u16 at +21708/+21704 of the
// per-voice slot. (n*0x38: ((n<<3 - n) << 3) = (7n)<<3 = 56n.)
static void ov_80094B50(Core* c) {
  uint32_t n = c->mem_r16(0x80105D10u);                 // *(0x80100000+23824)
  uint32_t lo = 0, hi = 0;
  if (n < 16) lo = 1u << n; else hi = 1u << (n - 16);
  uint32_t slot = n * 56u;                              // n * 0x38 byte stride
  c->mem_w8(0x801054E5u + slot, 0);                     // per-voice byte (0x80100000+21733)
  c->mem_w16(0x801054CCu + slot, 0);                    // per-voice u16 (0x80100000+21708)
  c->mem_w16(0x801054C8u + slot, 0);                    // per-voice u16 (0x80100000+21704)
  // key-on shadows are read FIRST (before the OR), then updated; key-off shadows are masked by the NEW
  // key-on value: koff &= ~(kon | bit).
  uint32_t konLo = c->mem_r16(0x80105BF0u);             // (0x80100000+23536)
  uint32_t konHi = c->mem_r16(0x80105BF2u);             // (0x80100000+23538)
  uint32_t koffLo = c->mem_r16(0x801054B8u);            // (0x80100000+21688)
  uint32_t newKonLo = konLo | lo;
  c->mem_w16(0x80105BF0u, (uint16_t)newKonLo);
  c->mem_w16(0x801054B8u, (uint16_t)(koffLo & ~newKonLo));
  uint32_t koffHi = c->mem_r16(0x801054BAu);            // (0x80100000+21690)
  uint32_t newKonHi = konHi | hi;
  c->mem_w16(0x80105BF2u, (uint16_t)newKonHi);
  c->mem_w16(0x801054BAu, (uint16_t)(koffHi & ~newKonHi));
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

// 0x80096370 — store (uint8_t)a0 to the global byte at 0x80105D28.
static void ov_80096370(Core* c) { c->mem_w8(0x80105D28u, (uint8_t)c->r[4]); }

// 0x80096390 — store u16 0 to the global at 0x80105CD8.
static void ov_80096390(Core* c) { c->mem_w16(0x80105CD8u, 0); }

// 0x800963A0 — bounded register: if ((a0-1)&0xff) < 24, store (uint8_t)a0 to the global at
// 0x80105CEC and return a0 sign-extended from its low byte; else return -1.
static void ov_800963A0(Core* c) {
  uint32_t a0 = c->r[4];
  if ((uint32_t)((a0 - 1) & 0xff) < 24) { c->mem_w8(0x80105CECu, (uint8_t)a0); c->r[2] = (uint32_t)(int32_t)(int8_t)a0; }
  else c->r[2] = (uint32_t)-1;
}

// ── 0x80097540 — sound/voice region-bound helper. If *0x800AC628 (=0x800B0000-14808) is zero, skip the
// clamp; else divide a1 by *0x800AC630, and if the remainder (hi) != 0, round a1 up to the next multiple
// of the divisor with the mask ~*0x800AC634 (a1 = (a1+div) & ~mask). Then a1 >>= (*0x800AC62C & 31) → r7.
// Selection on a0 (==r6): a0==-2 → return a1; a0==-1 → return r7 & 0xFFFF (no store); else → store r7
// (u16) into the table at *0x800AC604 (-14844) indexed by a0*2 and return a1. The recompiler's
// div-by-zero guard (rec_break 7168) only fires when the divisor is 0; reproduced faithfully.
// (Everything after L_800975DC's `return;` is the next function — dropped.)
static void ov_80097540(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];                  // a0 = r6
  if (c->mem_r32(0x800AC628u) != 0) {                   // *(0x800B0000-14808)
    uint32_t div = c->mem_r32(0x800AC630u);             // *(0x800B0000-14800)
    cpu_divu(c, a1, div);
    if (div == 0) rec_break(c, 7168u);                  // div-by-zero trap (faithful to body)
    if (c->hi != 0) {                                   // remainder != 0 → round up
      uint32_t mask = c->mem_r32(0x800AC634u);          // *(0x800B0000-14796)
      a1 = (a1 + div) & ~mask;
    }
  }
  uint32_t sh = c->mem_r32(0x800AC62Cu) & 31;           // *(0x800B0000-14804)
  uint32_t r7 = a1 >> sh;
  if (a0 == (uint32_t)-2) { c->r[2] = a1; return; }
  if (a0 == (uint32_t)-1) { c->r[2] = r7 & 0xFFFFu; return; }
  uint32_t tbl = c->mem_r32(0x800AC604u) + (a0 << 1);   // *(0x800B0000-14844), index a0
  c->mem_w16(tbl, (uint16_t)r7);
  c->r[2] = a1;
}

// 0x80097E10 / 0x80098DB0 — call 0x80097E40 (a0,a1 passed through) with fixed (a2,a3) id pairs.
static void ov_80097E10(Core* c) { c->r[6] = 202; c->r[7] = 203; call_fn(c, 0x80097E40u); }

// ── 0x80097E40 — voice-pan/volume table read-modify-write returning a 24-bit clamp value. Args:
// a0=mode (r9), a1=packed value (r8: lo16 = the index-r6 word, hi byte = the index-r7 word), a2=idx_lo
// (r6), a3=idx_hi (r7). Active u16 table = static *0x800AC604 (-14844) if bit0 of *0x800AC5F0 (-14864)
// is set, else dynamic @0x80105D40 (-23872... =+23872). r10 is first composed from the two existing
// table words: r10 = word[idx_lo] | ((word[idx_hi] & 0xFF) << 16). Only THREE modes do a write:
//   a0==1 : OR  the packed value into word[idx_lo] (lo16) and word[idx_hi] (hi byte); r10 |= val&0xFFFFFF.
//   a0==0 : AND-clear those bits out of the two words; r10 &= ~(val&0xFFFFFF).
//   a0==8 : OVERWRITE the two words with the packed value; r10 = val & 0xFFFFFF.
// Any other a0 returns r10 unchanged. The busy shadow *0x800AC5BC (-14916) gets a (1 << ((idx_lo-198)>>1))
// bit OR'd in — but ONLY on the dynamic-table path (static path skips it). Always returns r10 & 0xFFFFFF.
static void ov_80097E40(Core* c) {
  int32_t  mode = (int32_t)c->r[4];  // r9
  uint32_t val  = c->r[5];           // r8 (packed: lo16 + hi byte)
  uint32_t iLo  = c->r[6];           // r6 — slot for the low 16 bits
  uint32_t iHi  = c->r[7];           // r7 — slot for the high byte
  const uint32_t DYN = 0x80105D40u;  // dynamic table (0x80100000+23872)
  const uint32_t MASK = 0x00FFFFFFu;

  bool dyn = (c->mem_r32(0x800AC5F0u) & 1u) != 0;        // bit0 SET → keep DYN; CLEAR → static *-14844
  uint32_t tbl = dyn ? DYN : c->mem_r32(0x800AC604u);    // active table for compose AND writes

  uint32_t loWord = c->mem_r16(tbl + (iLo << 1));
  uint32_t hiByte = c->mem_r16(tbl + (iHi << 1)) & 0xFFu;
  uint32_t r10 = loWord | (hiByte << 16);

  uint32_t hb = (val >> 16) & 0xFFu;
  auto bump_busy = [&]() {                               // only on dynamic path
    if (dyn) {
      uint32_t shamt = (uint32_t)(((int32_t)(iLo - 198)) >> 1) & 31;
      c->mem_w32(0x800AC5BCu, c->mem_r32(0x800AC5BCu) | (1u << shamt));   // -14916
    }
  };

  if (mode == 1) {                                       // OR
    c->mem_w16(tbl + (iLo << 1), (uint16_t)(c->mem_r16(tbl + (iLo << 1)) | val));
    c->mem_w16(tbl + (iHi << 1), (uint16_t)(c->mem_r16(tbl + (iHi << 1)) | hb));
    bump_busy();
    r10 = r10 | (val & MASK);
  } else if (mode == 0) {                                // AND-clear
    c->mem_w16(tbl + (iLo << 1), (uint16_t)(c->mem_r16(tbl + (iLo << 1)) & ~val));
    c->mem_w16(tbl + (iHi << 1), (uint16_t)(c->mem_r16(tbl + (iHi << 1)) & ~hb));
    bump_busy();
    r10 = r10 & ~(val & MASK);
  } else if (mode == 8) {                                // overwrite
    c->mem_w16(tbl + (iLo << 1), (uint16_t)val);
    c->mem_w16(tbl + (iHi << 1), (uint16_t)hb);
    bump_busy();
    r10 = val & MASK;
  }
  c->r[2] = r10 & MASK;
}

