// engine/gpu_lib.cpp — PC-native libgpu-style primitive builders (draw-mode/tpage/sprite words, GPU packet build/finalize, GPU heap alloc, cel-upload flags).

#include "core.h"
#include "cfg.h"
#include "gpu_gpu.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

// Engine-owned SCREEN FADE entry — replaces the PSX full-screen semi OT rect (the old FUN_8007e9c8
// screen-fade callers, OT slot 0/4). Mirrors that entry's blend RE (FUN_80083de0 E1 ABR field): a1!=0 =>
// ABR=1 ADDITIVE (fade to/from white), a1==0 => ABR=2 SUBTRACTIVE (fade to/from black). The callers still
// compute the exact brightness curve (the grey level in `color`); we only change DELIVERY from a PSX rect
// to engine fade state, applied PC-native in present.frag + the headless readback. NOT a PSX packet.
void engine_fade_set(Core* core, uint32_t color, uint32_t a1) {
  int mode = (a1 != 0u) ? 1 /*additive/white*/ : 2 /*subtractive/black*/;
  gpu_set_fade(core, mode, (uint8_t)(color & 0xffu), (uint8_t)((color >> 8) & 0xffu), (uint8_t)((color >> 16) & 0xffu));
}

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

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

// 0x80050738 — build four viewport/clip descriptors (0x80083B30 ×2, 0x80083BF0 ×2, all with stack
// arg5=240) in the block at 0x800EA0BC (and its +8304 mirror), then patch their extent fields from
// the screen dims at 0x800E7E70/72. The descriptor builders read arg5 at sp+16; both this fn and the
// callees read arg5 at sp+16, so allocate the gen's 40-byte frame (decrement c->r[29]) so that slot
// is in THIS function's frame, not the caller's (writing the caller's frame hangs the interp caller).
static void ov_80050738(Core* c) {
  uint32_t r16 = 0x800EA0BCu, r18 = r16 + 8304;     // 0x800F0000-24388 ; +0x2070
  c->r[29] -= 40; uint32_t sp = c->r[29];
  c->r[4] = r16;        c->r[5] = 0; c->r[6] = 0;   c->r[7] = 320; c->mem_w32(sp + 16, 240); call_fn(c, 0x80083B30u);
  c->r[4] = r18;        c->r[5] = 0; c->r[6] = 256; c->r[7] = 320; c->mem_w32(sp + 16, 240); call_fn(c, 0x80083B30u);
  c->r[4] = r16 - 20;   c->r[5] = 0; c->r[6] = 256; c->r[7] = 320; c->mem_w32(sp + 16, 240); call_fn(c, 0x80083BF0u);
  c->r[4] = r16 + 8284; c->r[5] = 0; c->r[6] = 0;   c->r[7] = 320; c->mem_w32(sp + 16, 240); call_fn(c, 0x80083BF0u);
  c->r[29] += 40;                                    // restore sp (frame freed)
  uint32_t r4 = r16 - 12, r2 = r16 + 8292;
  uint32_t w = c->mem_r16(0x800E7E70u), h = c->mem_r16(0x800E7E72u);
  c->mem_w16(r4 + 4, 256); c->mem_w16(r4 + 6, 224);
  c->mem_w16(r2 + 4, 256); c->mem_w16(r2 + 6, 224);
  c->mem_w8(r16 + 25, 0); c->mem_w8(r16 + 26, 0); c->mem_w8(r16 + 27, 0);
  c->mem_w8(r18 + 25, 0); c->mem_w8(r18 + 26, 0); c->mem_w8(r18 + 27, 0);
  c->mem_w16(r16 - 12, (uint16_t)w); c->mem_w16(r4 + 2, (uint16_t)h);
  c->mem_w16(r16 + 8292, (uint16_t)w); c->mem_w16(r2 + 2, (uint16_t)h);
}

// 0x8007E9C8 — build a GPU primitive into a 12-word pool slot (pool head at 0x800BF544, indexed
// table ptr at 0x800ED8C8). Composes a header via scratchpad, links into the OT bucket a2, then
// calls 0x80083DE0(slot, 0, 0, len, /*arg5*/0) — STACK ARG at sp+16, so the gen 40-byte frame is
// replicated (c->r[29] -= 40 .. += 40) so the callee reads arg5 correctly without smashing the caller.
static void ov_8007E9C8(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  const uint32_t S = 0x1F800000u;            // scratchpad scratch struct
  c->mem_w32(S + 4, a0);
  c->mem_w8 (S + 7, 98);
  uint32_t p = c->mem_r32(0x800BF544u);
  c->mem_w16(S + 12, 320);
  c->mem_w16(S + 8, 0); c->mem_w16(S + 10, 0); c->mem_w16(S + 14, 240);
  uint32_t tabp = c->mem_r32(0x800ED8C8u) + a2 * 4u;
  c->mem_w32(p, c->mem_r32(tabp) | 0x03000000u);
  c->mem_w32(tabp, p);
  p += 4;
  c->mem_w32(p, c->mem_r32(S + 4));  p += 4;
  c->mem_w32(p, c->mem_r32(S + 8));  p += 4;
  c->mem_w32(p, c->mem_r32(S + 12)); p += 4;
  c->mem_w32(0x800BF544u, p);
  uint32_t len = ((a1 & 255u) != 0u) ? 32u : 64u;
  c->r[29] -= 40;                            // replicate gen frame for stack arg5
  c->mem_w32(c->r[29] + 16, 0);
  c->r[4] = p; c->r[5] = 0; c->r[6] = 0; c->r[7] = len;
  call_fn(c, 0x80083DE0u);
  c->r[29] += 40;
  tabp = c->mem_r32(0x800ED8C8u) + a2 * 4u;
  c->mem_w32(p, c->mem_r32(tabp) | 0x02000000u);
  c->mem_w32(tabp, p);
  uint32_t q = c->mem_r32(0x800BF544u);
  c->mem_w32(0x800BF544u, q + 12u);
}

// 0x80082220 — GPU draw-mode word: base 0xE1000000, |512 if a1!=0, plus (a2&2559)|1024 if a0!=0.
static void ov_80082220(Core* c) {
  uint32_t hi = 0xE1000000u | (c->r[5] != 0 ? 512u : 0u);
  uint32_t lo = (c->r[6] & 2559u) | (c->r[4] != 0 ? 1024u : 0u);
  c->r[2] = hi | lo;
}

static void ov_80082240(Core* c) { clip_word(c, 0xE3000000u); }

static void ov_800822D8(Core* c) { clip_word(c, 0xE4000000u); }

// 0x80082370 — libgpu primitive/tpage word: 0xE5000000 | ((a1&2047)<<11) | (a0&2047).
static void ov_80082370(Core* c) { c->r[2] = 0xE5000000u | ((c->r[5] & 2047u) << 11) | (c->r[4] & 2047u); }

// 0x80082C68 — write four GPU primitive templates through the pointer table at 0x800A5AA8..0x800A5AB4:
// [0]=0x04000002, [1]=a0, [2]=0, [3]=0x01000401 (draw-mode / tex-window / etc. setup).
static void ov_80082C68(Core* c) {
  c->mem_w32(c->mem_r32(0x800A5AA8u), 0x04000002u);
  c->mem_w32(c->mem_r32(0x800A5AACu), c->r[4]);
  c->mem_w32(c->mem_r32(0x800A5AB0u), 0);
  c->mem_w32(c->mem_r32(0x800A5AB4u), 0x01000401u);
}

// 0x800834A0 — call 0x80085900(-1); store ret+240 at 0x800A5ADC and 0 at 0x800A5AE0.
static void ov_800834A0(Core* c) {
  c->r[4] = (uint32_t)-1; call_fn(c, 0x80085900u);
  c->mem_w32(0x800A5ADCu, c->r[2] + 240u);
  c->mem_w32(0x800A5AE0u, 0);
}

// 0x80083AF8 — memset: write byte (uint8_t)a1 to `a2` bytes at `a0`.
static void ov_80083AF8(Core* c) {
  uint32_t p = c->r[4], n = c->r[6]; uint8_t v = (uint8_t)c->r[5];
  for (uint32_t i = 0; i < n; i++) c->mem_w8(p + i, v);
}

// 0x80083B30 — initialize a ~28-byte sprite/object descriptor at a0 from (a1,a2,a3) + a stack 5th arg,
// reading the global flag at 0x800ABE20 (via 0x80086604) to pick the +23 bound (arg5<257 vs <289).
static void ov_80083B30(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
  uint32_t arg5 = c->mem_r32(c->r[29] + 16);
  call_fn(c, 0x80086604u);                       // r2 = *0x800ABE20
  uint32_t flag = c->r[2];
  c->mem_w16(a0 + 0, (uint16_t)a1); c->mem_w16(a0 + 2, (uint16_t)a2); c->mem_w16(a0 + 4, (uint16_t)a3);
  c->mem_w16(a0 + 12, 0); c->mem_w16(a0 + 14, 0); c->mem_w16(a0 + 16, 0); c->mem_w16(a0 + 18, 0);
  c->mem_w8(a0 + 25, 0); c->mem_w8(a0 + 26, 0); c->mem_w8(a0 + 27, 0); c->mem_w8(a0 + 22, 1);
  c->mem_w16(a0 + 6, (uint16_t)arg5);
  uint32_t bound = (flag == 0) ? ((int32_t)arg5 < 257) : ((int32_t)arg5 < 289);
  c->mem_w8(a0 + 23, (uint8_t)bound);
  c->mem_w16(a0 + 8, (uint16_t)a1); c->mem_w16(a0 + 10, (uint16_t)a2);
  c->mem_w16(a0 + 20, 10); c->mem_w8(a0 + 24, 0);
}

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

// 0x800974FC — write into the u16 table at *0x800AC604, index a0: if a2==0 store a1, else store
// a1 >> (*0x800AC62C & 31).
static void ov_800974FC(Core* c) {
  uint32_t p = c->mem_r32(0x800AC604u) + (c->r[4] << 1);
  if (c->r[6] == 0) c->mem_w16(p, (uint16_t)c->r[5]);
  else c->mem_w16(p, (uint16_t)(c->r[5] >> (c->mem_r32(0x800AC62Cu) & 31)));
}

// 0x80097678 — read-modify-write the word at *0x800AC618: v = (v & 0xF0FFFFFF) | 0x02000000.
static void ov_80097678(Core* c) {
  uint32_t p = c->mem_r32(0x800AC618u);
  c->mem_w32(p, (c->mem_r32(p) & 0xF0FFFFFFu) | 0x02000000u);
}

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

// 0x800977C0 — allocate a `size`-byte run from the block table at G_base (0x800AC66C). Each block is
// 8 bytes: word0 = flags(bit30 used / bit31 free) | addr(low 28 bits), word1 = size. Globals:
//   active 0x800AC59C, a 0x800AC5A0, shift 0x800AC62C, mask 0x800AC634,
//   base 0x800AC66C, count 0x800AC664, nextfree 0x800AC668.
// Rounds `size` up to mask then aligns to 1<<shift; scans for a used block or a big-enough free block;
// splits it (used → new block at idx+1; free → remainder block at nextfree) and returns its addr, or
// -1 on no fit. 0x80097A90 is a lock/compaction sub-routine called around the table mutation.
static void ov_800977C0(Core* c) {
  const uint32_t GACT=0x800AC59Cu, GA=0x800AC5A0u, GSH=0x800AC62Cu, GMASK=0x800AC634u;
  const uint32_t GBASE=0x800AC66Cu, GCOUNT=0x800AC664u, GNEXT=0x800AC668u;

  uint32_t size = c->r[4];
  uint32_t active = c->mem_r32(GACT);
  int found = -1;                                   // r18

  uint32_t adj;                                     // r19
  if (active != 0) {
    uint32_t sh = c->mem_r32(GSH);
    adj = (0x10000u - c->mem_r32(GA)) << (sh & 31);
  } else adj = 0;

  // round up toward mask, then align down to 1<<shift (arithmetic shift)
  uint32_t mask = c->mem_r32(GMASK);
  if ((size & ~mask) != 0) size += mask;
  uint32_t sh = c->mem_r32(GSH);
  size = (uint32_t)(((int32_t)size >> (sh & 31)) << (sh & 31));

  uint32_t base = c->mem_r32(GBASE);
  if ((c->mem_r32(base) & 0x40000000u) != 0) {
    found = 0;                                      // *base already used → take index 0
  } else {
    call_fn(c, 0x80097A90u);
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (0 < count) {
      int i = 0;
      uint32_t bp = c->mem_r32(GBASE);              // &block[0]
      for (;;) {
        uint32_t blk = c->mem_r32(bp);
        bool hit = false;
        if (blk & 0x40000000u) hit = true;          // used → take it
        else if (blk & 0x80000000u) {               // free → take if big enough
          if (!(c->mem_r32(bp + 4) < size)) hit = true;
        }
        if (hit) { found = i; break; }
        i++;
        if (i < count) { bp += 8; continue; }
        break;
      }
    }
  }

  if (found == -1) { c->r[2] = (uint32_t)-1; return; }

  uint32_t basep = c->mem_r32(GBASE);
  uint32_t blkp = (uint32_t)(found << 3) + basep;
  uint32_t blk = c->mem_r32(blkp);

  if (blk & 0x40000000u) {
    // USED block → split a new used block in at idx+1
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (!((int32_t)found < count)) { c->r[2] = (uint32_t)-1; return; }
    if ((c->mem_r32(blkp + 4) - adj) < size) { c->r[2] = (uint32_t)-1; return; }
    uint32_t nidx = (uint32_t)found + 1;
    uint32_t np = (nidx << 3) + basep;
    uint32_t w0 = (c->mem_r32(blkp) & 0x0FFFFFFFu) + size;
    c->mem_w32(np + 0, w0 | 0x40000000u);
    c->mem_w32(np + 4, c->mem_r32(blkp + 4) - size);
    uint32_t addr = c->mem_r32(blkp);
    c->mem_w32(GNEXT, nidx);
    c->mem_w32(blkp + 4, size);
    c->mem_w32(blkp + 0, addr & 0x0FFFFFFFu);
    call_fn(c, 0x80097A90u);
    c->r[2] = c->mem_r32((uint32_t)(found << 3) + c->mem_r32(GBASE));
    return;
  }

  // FREE block → carve `size` off the front, push the remainder as a new free block at nextfree
  uint32_t blksize = c->mem_r32(blkp + 4);
  if (size < blksize) {
    int nf = (int32_t)c->mem_r32(GNEXT);
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (nf < count) {
      uint32_t np = ((uint32_t)nf << 3) + basep;
      uint32_t oldw0 = c->mem_r32(np + 0);
      uint32_t oldw1 = c->mem_r32(np + 4);
      c->mem_w32(np + 0, (blk + size) | 0x80000000u);
      c->mem_w32(np + 4, blksize - size);
      c->mem_w32(GNEXT, (uint32_t)nf + 1);
      c->mem_w32(np + 8, oldw0);
      c->mem_w32(np + 12, oldw1);
    }
  }
  uint32_t basep2 = c->mem_r32(GBASE);
  uint32_t blkp2 = (uint32_t)(found << 3) + basep2;
  uint32_t addr = c->mem_r32(blkp2) & 0x0FFFFFFFu;
  c->mem_w32(blkp2 + 4, size);
  c->mem_w32(blkp2 + 0, addr);
  call_fn(c, 0x80097A90u);
  c->r[2] = c->mem_r32((uint32_t)(found << 3) + c->mem_r32(GBASE));
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

// 0x80099370 — *0x800AC594 = a0; *0x800AC620 = (a0 == 1) ? 1 : 0.
static void ov_80099370(Core* c) {
  c->mem_w32(0x800AC594u, c->r[4]);
  c->mem_w32(0x800AC620u, (c->r[4] == 1u) ? 1u : 0u);
}

// 0x80099450 — *0x800AC638 = (a0 == 1) ? 0 : 1.
static void ov_80099450(Core* c) { c->mem_w32(0x800AC638u, (c->r[4] == 1u) ? 0u : 1u); }

// 0x80099478 — return (*0x800AC638 ^ 1) != 0  (i.e. *0x800AC638 != 1).
static void ov_80099478(Core* c) { c->r[2] = ((c->mem_r32(0x800AC638u) ^ 1u) != 0u) ? 1u : 0u; }

// 0x8009A1D0 — table read: entry = *0x800AC604 + (a0<<4); *(u16*)a1 = *(u16*)(entry+12).
static void ov_8009A1D0(Core* c) {
  uint32_t e = c->mem_r32(0x800AC604u) + (c->r[4] << 4);
  c->mem_w16(c->r[5], c->mem_r16(e + 12));
}

// 0x8009C9D0 — finalize a GPU command packet: call 0x8009CAEC(a0,a1); then poke five GPU/DMA
// register cells (ptrs held in the 0x800AD0xx table) with composed words. v16 = (a1>>5)<<16.
static void ov_8009C9D0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  call_fn(c, 0x8009CAECu);                 // (a0, a1) — args already in r4/r5
  uint32_t v16 = (a1 >> 5) << 16;
  uint32_t p;
  p = c->mem_r32(0x800AD09Cu); c->mem_w32(p, c->mem_r32(p) | 136u);
  p = c->mem_r32(0x800AD064u); c->mem_w32(p, a0 + 4u);
  p = c->mem_r32(0x800AD068u); c->mem_w32(p, v16 | 32u);
  p = c->mem_r32(0x800AD094u); c->mem_w32(p, c->mem_r32(a0));
  p = c->mem_r32(0x800AD06Cu); c->mem_w32(p, 0x01000000u | 513u);
}

