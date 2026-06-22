// peripheral_misc.cpp — small PSX-platform peripheral natives (pad/SIO command push, memcard retry wrappers, BCD<->frame time, rumble).

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

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

// 0x80089B98 — get/set the global word at 0x800ABFC0 (returns old, writes a0).
static void ov_80089B98(Core* c) { c->r[2] = c->mem_r32(0x800ABFC0u); c->mem_w32(0x800ABFC0u, c->r[4]); }

// 0x80089BAC — retry wrapper (up to 4 attempts) around 0x8008AC34. idx = a0 & 255.
// Returns 1 on success (3rd call returns 0), 0 if all attempts fail.
static void ov_80089BAC(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  uint32_t idx = a0 & 255u;
  uint32_t slot = 0x800ABF34u + idx * 4u;
  uint32_t saved = c->mem_r32(0x800ABFBCu);
  int attempt = 3;
  for (;;) {
    c->mem_w32(0x800ABFBCu, 0);
    if (idx != 1u && (c->mem_r8(0x800ABFC8u) & 16u) != 0u) {
      c->r[4]=1; c->r[5]=0; c->r[6]=0; c->r[7]=0; call_fn(c, 0x8008AC34u);
    }
    bool do_main = true;                     // the L_C78 block
    if (a1 != 0u && c->mem_r32(slot) != 0u) {
      c->r[4]=2; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] != 0u) do_main = false;    // -> retry (L_C9C)
    }
    if (do_main) {
      c->mem_w32(0x800ABFBCu, saved);
      c->r[4]=idx; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] == 0u) { c->r[2] = 1; return; }   // success
    }
    if (--attempt < 0) break;                // r16 reached -1
  }
  c->mem_w32(0x800ABFBCu, saved);
  c->r[2] = 0;
}

// 0x80089E1C — like 80089BAC but on success additionally calls 0x8008A6EC(0,a2) and returns
// (its ret == 2). Returns 0 if all attempts fail. idx = a0 & 255.
static void ov_80089E1C(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  uint32_t idx = a0 & 255u;
  uint32_t slot = 0x800ABF34u + idx * 4u;
  uint32_t saved = c->mem_r32(0x800ABFBCu);
  int attempt = 3;
  for (;;) {
    c->mem_w32(0x800ABFBCu, 0);
    if (idx != 1u && (c->mem_r8(0x800ABFC8u) & 16u) != 0u) {
      c->r[4]=1; c->r[5]=0; c->r[6]=0; c->r[7]=0; call_fn(c, 0x8008AC34u);
    }
    bool do_main = true;
    if (a1 != 0u && c->mem_r32(slot) != 0u) {
      c->r[4]=2; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] != 0u) do_main = false;    // -> retry
    }
    if (do_main) {
      c->mem_w32(0x800ABFBCu, saved);
      c->r[4]=idx; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] == 0u) {                    // success
        c->r[4]=0; c->r[5]=a2; call_fn(c, 0x8008A6ECu);
        c->r[2] = (c->r[2] == 2u) ? 1u : 0u;
        return;
      }
    }
    if (--attempt < 0) break;
  }
  c->mem_w32(0x800ABFBCu, saved);
  c->r[2] = 0;
}

// 0x80089F68 — call 0x8008B040 (args passed through from caller) then return 1.
static void ov_80089F68(Core* c) { call_fn(c, 0x8008B040u); c->r[2] = 1; }

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

// 0x8008BBC8 — strncmp(a0, a1, 12) via 0x8009A640; return 1 if equal (result < 1 unsigned == result 0), else 0.
static void ov_8008BBC8(Core* c) { c->r[6] = 12; call_fn(c, 0x8009A640u); c->r[2] = (c->r[2] < 1u) ? 1u : 0u; }

// 0x8008BEAC — search a 128-entry, stride-44 table at 0x80102D6C for an entry whose id (+0) == a0 AND
// whose name (+8) strcmp-matches a1 (via 0x8009A540). Returns index+1 on match, -1 on miss / id==0 end.
static void ov_8008BEAC(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t idp = 0x80102D6Cu, namep = 0x80102D74u;   // 0x80100000 + 11628 / +11636
  for (int i = 0; i < 128; i++) {
    uint32_t id = c->mem_r32(idp);
    if (id == 0) { c->r[2] = (uint32_t)-1; return; }
    if (id == a0) {
      c->r[4] = a1; c->r[5] = namep; call_fn(c, 0x8009A540u);   // strcmp(a1, name)
      if (c->r[2] == 0) { c->r[2] = (uint32_t)(i + 1); return; }
    }
    idp += 44; namep += 44;
  }
  c->r[2] = (uint32_t)-1;
}

