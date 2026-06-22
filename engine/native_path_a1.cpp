// Hand-written native C++ for the boot → first-cutscene path (Tomba2Engine) — batch A1.
//
// Same direction as engine/native_path.cpp: these functions ARE the engine, RE'd from the recompiler's
// own decode (generated/gen_func_<addr> bodies are the reference). Each is a LEAF (no jal/jalr into
// other guest funcs, no rec_dispatch); the helpers cpu_divu / rec_break used below are interpreter
// primitives (integer divide + break-exception), not guest calls. Trailing duplicate `return;` and any
// code AFTER the function's real final return are the recompiler over-running into the NEXT function and
// are NOT part of these bodies — they are dropped here.
//
// Global-base note: `(uint32_t)32779u << 16` = 0x800B0000, `32784u<<16` = 0x80100000,
// `32780u<<16` = 0x800C0000, `32768u<<16` = 0x80000000. Absolute addresses are computed and written as
// literals with a comment showing the (base + signed-offset) that produced them.
#include "core.h"
#include <stdint.h>

// ── 0x8008B19C — controller/pad config seed. Through ptr *0x800AC294 (=0x800B0000-15724): if the u16 at
// +440 is zero AND the u16 at +442 is zero, seed +384/+386 with 16383; then always store 16383 to
// +432/+434 and 49153 to +426. Then write a small fixed pad descriptor through five indirect pointers
// (*0x800AC280/284/288/28C): bytes {2 ,128 ,0 ,3 ,128 ,0 ,32}. Returns 0. (The original uses a 4-byte
// stack scratch buffer of {128,0,128,0}; reproduced as locals.)
static void ov_8008B19C(Core* c) {
  uint32_t p = c->mem_r32(0x800AC294u);                 // *(0x800B0000-15724)
  if (c->mem_r16(p + 440) == 0 && c->mem_r16(p + 442) == 0) {
    c->mem_w16(p + 384, 16383);
    c->mem_w16(p + 386, 16383);
    p = c->mem_r32(0x800AC294u);                        // reloaded in the body (same value)
  }
  c->mem_w16(p + 432, 16383);
  c->mem_w16(p + 434, 16383);
  c->mem_w16(p + 426, 49153);                           // 0xC001
  // fixed pad descriptor written through indirect target pointers
  uint8_t buf[4] = { 128, 0, 128, 0 };                  // stack scratch: [0]=[2]=128, [1]=[3]=0
  c->mem_w8(c->mem_r32(0x800AC280u) + 0, 2);            // *(0x800B0000-15744)
  c->mem_w8(c->mem_r32(0x800AC288u) + 0, buf[0]);       // *(0x800B0000-15736) <- 128
  c->mem_w8(c->mem_r32(0x800AC28Cu) + 0, buf[1]);       // *(0x800B0000-15732) <- 0
  c->mem_w8(c->mem_r32(0x800AC280u) + 0, 3);            // *(0x800B0000-15744)
  c->mem_w8(c->mem_r32(0x800AC284u) + 0, buf[2]);       // *(0x800B0000-15740) <- 128
  c->mem_w8(c->mem_r32(0x800AC288u) + 0, buf[3]);       // *(0x800B0000-15736) <- 0
  c->mem_w8(c->mem_r32(0x800AC28Cu) + 0, 32);           // *(0x800B0000-15732) <- 32
  c->r[2] = 0;
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

// ── 0x800982A0 — range-overlap test against a list. a0 <<= (*0x800AC62C & 31). If the list head
// *0x800AC5F0... wait: the head is *0x800AC638? No — head ptr = *0x800AC5F0? Body: r3 = *0x800AC5F0
// (=0x800B0000-14740). If head==0 return 0. Walk entries (stride 8): for each entry word w=*entry:
//   if (w & 0x80000000) → next; else if (w & 0x40000000) → return 0; else r3 = w & 0x0FFFFFFF; if
//   (r3 < a0) → next; if (a0 < r3 + *(entry+4)) → next; else return 1. Returns 1 when a0 lands inside a
//   live [start,start+len) span, 0 if it falls before the span or off the end of the list.
static void ov_800982A0(Core* c) {
  uint32_t sh = c->mem_r32(0x800AC62Cu) & 31;           // *(0x800B0000-14804)
  uint32_t a0 = c->r[4] << sh;
  uint32_t e = c->mem_r32(0x800AC66Cu);                 // *(0x800B0000-14740) — list head
  if (e == 0) { c->r[2] = 0; return; }
  for (;;) {
    uint32_t w = c->mem_r32(e + 0);
    if (w & 0x80000000u) { e += 8; continue; }          // flagged → skip
    if (w & 0x40000000u) { c->r[2] = 0; return; }        // terminator
    uint32_t start = w & 0x0FFFFFFFu;
    if (start >= a0) { c->r[2] = 1; return; }            // a0 < start → "1" (matches: r3<a0 false → ret 1)
    uint32_t end = start + c->mem_r32(e + 4);
    if (a0 < end) { c->r[2] = 1; return; }               // inside span → 1
    e += 8;                                              // past this span → next entry
  }
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

// ── 0x80098F90 — voice key-on/key-off request. a1 &= 0x00FFFFFF; lo = a1 (low 16 bits via w16), hi =
// a1 >> 16 (the byte in bits16..23). Only a0==0 and a0==1 do work; any other a0 returns immediately.
// The "dynamic block" (@0x80105EC8 = 0x80100000+24264) vs the "static block" (*0x800AC604 = -14844) is
// gated on bit0 of *0x800AC5F0 (-14864): bit set → dynamic, clear → static. Globals: busy *0x800AC5BC
// (-14916), pending *0x800AC5B8 (-14920), enable *0x800AC590 (-14960).
//   a0==1 (key-ON):  dyn → DYN+0=lo, DYN+2=hi, busy|=1, pending|=a1, then AND-clear a1 out of DYN+4/+6.
//                    static → blk+392=lo, blk+394=hi, enable|=a1.
//   a0==0 (key-OFF): dyn → DYN+4=lo, DYN+6=hi, busy|=1, pending&=~a1, then AND-clear a1 out of DYN+0/+2.
//                    static → blk+396=lo, blk+398=hi, enable&=~a1.
static void ov_80098F90(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t a1 = c->r[5] & 0x00FFFFFFu;
  uint32_t lo = a1 & 0xFFFFu, hi = a1 >> 16;
  const uint32_t DYN = 0x80105EC8u;                      // dynamic voice block (0x80100000+24264)
  bool dyn = (c->mem_r32(0x800AC5F0u) & 1u) != 0;        // bit0 set → dynamic

  if (a0 == 1) {                                         // key-ON
    if (dyn) {
      c->mem_w16(DYN + 0, (uint16_t)lo);
      c->mem_w16(DYN + 2, (uint16_t)hi);
      c->mem_w32(0x800AC5BCu, c->mem_r32(0x800AC5BCu) | 1u);            // busy |= 1
      c->mem_w32(0x800AC5B8u, c->mem_r32(0x800AC5B8u) | a1);           // pending |= a1
      if (c->mem_r16(DYN + 4) & lo) c->mem_w16(DYN + 4, (uint16_t)(c->mem_r16(DYN + 4) & ~lo));
      if (c->mem_r16(DYN + 6) & hi) c->mem_w16(DYN + 6, (uint16_t)(c->mem_r16(DYN + 6) & ~hi));
    } else {
      uint32_t blk = c->mem_r32(0x800AC604u);
      c->mem_w16(blk + 392, (uint16_t)lo);
      c->mem_w16(blk + 394, (uint16_t)hi);
      c->mem_w32(0x800AC590u, c->mem_r32(0x800AC590u) | a1);           // enable |= a1
    }
  } else if (a0 == 0) {                                  // key-OFF
    if (dyn) {
      c->mem_w16(DYN + 4, (uint16_t)lo);
      c->mem_w16(DYN + 6, (uint16_t)hi);
      c->mem_w32(0x800AC5BCu, c->mem_r32(0x800AC5BCu) | 1u);            // busy |= 1
      c->mem_w32(0x800AC5B8u, c->mem_r32(0x800AC5B8u) & ~a1);          // pending &= ~a1
      if (c->mem_r16(DYN + 0) & lo) c->mem_w16(DYN + 0, (uint16_t)(c->mem_r16(DYN + 0) & ~lo));
      if (c->mem_r16(DYN + 2) & hi) c->mem_w16(DYN + 2, (uint16_t)(c->mem_r16(DYN + 2) & ~hi));
    } else {
      uint32_t blk = c->mem_r32(0x800AC604u);
      c->mem_w16(blk + 396, (uint16_t)lo);
      c->mem_w16(blk + 398, (uint16_t)hi);
      c->mem_w32(0x800AC590u, c->mem_r32(0x800AC590u) & ~a1);          // enable &= ~a1
    }
  }
  // any other a0 → no-op (body falls straight to L_80099144 return).
}

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

// ── 0x800752B4 — classify 24 entries into a priority byte at +8 of each stride-12 record in the table
// at 0x800C0000-7624 (=0x800B8C58... no: 0x800C0000-7624). For i in 0..23: pick a band by comparing the
// loop index i against thresholds derived from a0 (24-a0, 16-a0, 12-a0, 8-a0): i<24-a0 → 4; i<16-a0 → 1;
// i<12-a0 → 3; i<8-a0 → 2; else → 0. Store that byte to record[i].byte8. (Record addr = i*12 + table.)
static void ov_800752B4(Core* c) {
  int32_t a0 = (int32_t)c->r[4];
  uint32_t table = 0x800BE238u;                        // 0x800C0000 - 7624
  int32_t t24 = 24 - a0, t16 = 16 - a0, t12 = 12 - a0, t8 = 8 - a0;
  for (int32_t i = 0; i < 24; i++) {
    uint32_t rec = table + (uint32_t)(i * 12);
    uint8_t band;
    if (i < t24) band = 4;
    else if (i < t16) band = 1;
    else if (i < t12) band = 3;
    else if (i < t8)  band = 2;
    else              band = 0;
    c->mem_w8(rec + 8, band);
  }
}

// ── 0x80098810 — voice param scatter (key bits 0..18). a0 = ptr to a record (r4); first word *a0 is a
// bitmask r5 (and r6 = (r5<1) i.e. r5==0 forces all-fields when the mask is 0/negative-as-unsigned?). For
// each bit k, if the mask bit is set OR r6!=0 (mask is 0 → write everything; matches `bltz`/`<1` guard),
// copy the u16 at a0 + (4 + 2k) into the active voice block (*0x800AC604) at +(448 + 2k). 32 params at
// +448..+510 (bits 0..31), with the last guarded on the sign bit. r6 is true when r5==0 (or top bit set,
// via the signed `>=0` test on the final entry). Faithful per-bit transcription.
static void ov_80098810(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t mask = c->mem_r32(a0 + 0);
  bool all = (mask < 1u);                                // r6 = (r5 < 1) → true iff mask==0
  uint32_t blk = c->mem_r32(0x800AC604u);                // *(0x800B0000-14844)
  // bits 0..30 → field offset +448 + 2*bit, source +4 + 2*bit
  for (int bit = 0; bit < 31; bit++) {
    if (all || (mask & (1u << bit)))
      c->mem_w16(blk + 448 + 2 * bit, c->mem_r16(a0 + 4 + 2 * bit));
  }
  // bit 31 guarded on the sign: write iff r6!=0 OR (s32)mask < 0.
  if (all || ((int32_t)mask < 0))
    c->mem_w16(blk + 448 + 2 * 31, c->mem_r16(a0 + 4 + 2 * 31));
  // (body returns no value of interest before its real `return` at L_80098CD8.)
}

// ── 0x80098D30 — voice param scatter, bits 1 and 2 only (a small sibling of 0x80098810). a0=record ptr;
// r5=*a0 mask, r6=(r5==0). If (mask&2) or r6: copy u16 a0+8 → *0x800AC604 +388, and also shadow it to
// *0x800AC5AC (=-14932). If (mask&4) or r6: copy u16 a0+10 → block +390, shadow to *0x800AC5AE (-14930).
// Returns 0.
static void ov_80098D30(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t mask = c->mem_r32(a0 + 0);
  bool all = (mask < 1u);                                // r6 = (mask < 1) → mask==0
  if (all || (mask & 2u)) {
    uint32_t blk = c->mem_r32(0x800AC604u);              // *(-14844)
    uint16_t v = c->mem_r16(a0 + 8);
    c->mem_w16(blk + 388, v);
    c->mem_w16(0x800AC5ACu, v);                          // shadow (-14932)
  }
  if (all || (mask & 4u)) {
    uint32_t blk = c->mem_r32(0x800AC604u);
    uint16_t v = c->mem_r16(a0 + 10);
    c->mem_w16(blk + 390, v);
    c->mem_w16(0x800AC5AEu, v);                          // shadow (-14930)
  }
  c->r[2] = 0;
}

// Register every batch-A1 native function.
void games_native_path_a1_init(void) {
  // TODO(verify): rec_set_override(0x80097540u, ov_80097540); — DISABLED: A/B RAM-diff fails (2 bytes vs interp). return-selection edge case (a0==-1/-2 path). Re-enable after fixing gen_func_80097540 + re-A/B.
  // TODO(verify): rec_set_override(0x800752B4u, ov_800752B4); — DISABLED: A/B RAM-diff fails (167 bytes vs interp). stride-12 band-classify table @0x800BE238 — wrong records. Re-enable after fixing gen_func_800752B4 + re-A/B.
}
