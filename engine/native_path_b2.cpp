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

void games_native_path_b2_init(void) {
  rec_set_override(0x8008BEACu, ov_8008BEAC);
  rec_set_override(0x8009440Cu, ov_8009440C);
  rec_set_override(0x8007982Cu, ov_8007982C);
  rec_set_override(0x8007A810u, ov_8007A810);
}
