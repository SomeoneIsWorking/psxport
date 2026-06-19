// Hand-native boot→cutscene path — batch b2: more non-leaf init helpers (callees already native).
// Sub-calls via rec_dispatch(c, addr). RE'd from gen_func bodies; post-`jr ra` over-run dropped.
// A/B-verified (non-leaf: stack-region frame-slot diffs are benign; globals/pool/scratchpad must match).
#include "core.h"
#include <stdint.h>

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

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

// 0x8007982C — zero the 1524-byte control block at 0x800BF870 (via 0x8009A420), then seed its many
// default fields (and three scratchpad bytes/words). Values transcribed verbatim from the body.
static void ov_8007982C(Core* c) {
  uint32_t b = 0x800BF870u;
  c->r[4] = b; c->r[5] = 0; c->r[6] = 1524; call_fn(c, 0x8009A420u);
  c->mem_w8(b + 12, 8);  c->mem_w8(b + 13, 4);
  c->mem_w8(b + 28, 0);  c->mem_w8(b + 29, 0); c->mem_w8(b + 30, 0); c->mem_w8(b + 31, 0);
  c->mem_w8(b + 2, 255); c->mem_w8(b + 1520, 255); c->mem_w8(0x1F8001FFu, 255);
  c->mem_w16(0x1F800278u, 0);
  c->mem_w8(b + 15, 64); c->mem_w16(b + 352, 351);
  c->mem_w8(b + 580, 1); c->mem_w8(b + 590, 1); c->mem_w8(b + 846, 1); c->mem_w8(b + 69, 1);
  c->mem_w8(b + 354, 86); c->mem_w8(b + 836, 0); c->mem_w8(b + 50, 2); c->mem_w8(b + 350, 4);
  c->mem_w16(b + 56, 1); c->mem_w16(b + 390, 2280); c->mem_w16(b + 326, 0); c->mem_w16(b + 328, 0);
  c->mem_w8(b + 433, 1); c->mem_w8(b + 44, 2);
}

// 0x8007A810 — init a 4-entry, stride-264 array at 0x80100690: zero a 388-byte header, clear two
// globals, then for each of 4 entries memset 264 bytes + link (+36 = next entry ptr, +40 = 5); the
// last entry's +36 is reset to 0. Finally record the array base + a mode byte.
static void ov_8007A810(Core* c) {
  c->r[4] = 0x800E7E80u; c->r[5] = 0; c->r[6] = 388; call_fn(c, 0x8009A420u);
  c->mem_w32(0x800F2738u, 0); c->mem_w32(0x800F23A0u, 0);
  uint32_t base = 0x80100690u, link = 0x80100798u, p = base;
  for (int i = 0; i < 4; i++) {
    c->r[4] = p; c->r[5] = 0; c->r[6] = 264; call_fn(c, 0x8009A420u);
    c->mem_w32(p + 36, link); link += 264; c->mem_w8(p + 40, 5); p += 264;
  }
  c->mem_w32(base + 264 * 3 + 36, 0);
  c->mem_w32(0x800F273Cu, base);
  c->mem_w8(0x800F2410u, 4);
}

// 0x8007A8E0 — call 0x8007982C (block init) then clear the scratchpad u16 at 0x1F80017C.
static void ov_8007A8E0(Core* c) { call_fn(c, 0x8007982Cu); c->mem_w16(0x1F80017Cu, 0); }

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

// 0x80084250 — transform 3 vertices through a GTE matrix (5 ctrl regs from a0), three GTE ops with
// the packed SXY/SZ vertex data at a1, and pack the resulting screen coords back into a0 (+0/+4/+8/
// +12/+16). Result-read order preserved (each op's data regs read before the next op overwrites them).
static void ov_80084250(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  gte_write_ctrl(0, c->mem_r32(a0 + 0)); gte_write_ctrl(1, c->mem_r32(a0 + 4)); gte_write_ctrl(2, c->mem_r32(a0 + 8));
  gte_write_ctrl(3, c->mem_r32(a0 + 12)); gte_write_ctrl(4, c->mem_r32(a0 + 16));
  uint32_t r8, r9, r10;
  r8 = c->mem_r16(a1 + 0); r9 = c->mem_r32(a1 + 4) & 0xFFFF0000u; r10 = c->mem_r32(a1 + 12);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  r8 = c->mem_r16(a1 + 2); r9 = c->mem_r32(a1 + 8) << 16; r10 = (uint32_t)(int16_t)c->mem_r16(a1 + 14);
  uint32_t r11 = gte_read_data(9), r12 = gte_read_data(10), r13 = gte_read_data(11);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  r8 = c->mem_r16(a1 + 4); r9 = c->mem_r32(a1 + 8) & 0xFFFF0000u; r10 = c->mem_r32(a1 + 16);
  uint32_t r14 = gte_read_data(9), r15 = gte_read_data(10), r24 = gte_read_data(11);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  c->mem_w32(a0 + 0, (r14 << 16) | (r11 & 0xFFFFu));
  c->mem_w32(a0 + 12, (r24 << 16) | (r13 & 0xFFFFu));
  uint32_t d9 = gte_read_data(9), d10 = gte_read_data(10);
  c->mem_w32(a0 + 4, (r12 << 16) | (d9 & 0xFFFFu));
  c->mem_w32(a0 + 8, (d10 << 16) | (r15 & 0xFFFFu));
  c->mem_w32(a0 + 16, gte_read_data(11));
  c->r[2] = a0;
}

// 0x80098150 — channel state toggle on a0 (0 or 1). Updates the flag word at *(0x800AC604)+426 and the
// active-id at 0x800AC598. a0==0: clear (&0xFF7F), id=0. a0==1: if already active (*0x800AC59C==1) or
// 0x800982A0(*0x800AC5A0)==0 → set (|0x80), id=1; else clear. Other a0: no change. Returns *0x800AC598.
static void ov_80098150(Core* c) {
  uint32_t a0 = c->r[4];
  if (a0 == 0) {
    uint32_t t = c->mem_r32(0x800AC604u);
    uint32_t v = c->mem_r16(t + 426); c->mem_w32(0x800AC598u, 0);
    c->mem_w16(t + 426, (uint16_t)(v & 65407u));
  } else if (a0 == 1) {
    int matched = (c->mem_r32(0x800AC59Cu) == a0);
    if (!matched) { c->r[4] = c->mem_r32(0x800AC5A0u); call_fn(c, 0x800982A0u); matched = (c->r[2] == 0); }
    uint32_t t = c->mem_r32(0x800AC604u);
    uint32_t v = c->mem_r16(t + 426);
    if (matched) { c->mem_w32(0x800AC598u, a0); v |= 128u; }
    else { c->mem_w32(0x800AC598u, 0); v &= 65407u; }
    c->mem_w16(t + 426, (uint16_t)v);
  }
  c->r[2] = c->mem_r32(0x800AC598u);
}

void games_native_path_b2_init(void) {
  rec_set_override(0x8008BEACu, ov_8008BEAC);
  rec_set_override(0x8009440Cu, ov_8009440C);
  rec_set_override(0x8007982Cu, ov_8007982C);
  rec_set_override(0x8007A810u, ov_8007A810);
  rec_set_override(0x8007A8E0u, ov_8007A8E0);
  rec_set_override(0x80050738u, ov_80050738);
  rec_set_override(0x80084250u, ov_80084250);
  rec_set_override(0x80098150u, ov_80098150);
}
