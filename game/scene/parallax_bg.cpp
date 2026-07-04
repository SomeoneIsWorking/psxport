// ParallaxBg::step — the per-frame body of guest FUN_8010BFFC. See parallax_bg.h for the SM
// layout + purpose. Ghidra decomp: scratch/decomp/sop_bg_chain.c.
//
// Faithful to the recomp; the state-1 wrap loops are the recomp's over-then-rollback pattern
// (`while (v < 0) v += mod;  v -= mod;`), and the underflow test on the SM+0x38 counter mirrors
// the MIPS `sll 24 ; blez` idiom as a signed-byte `<= 0`.

#include "parallax_bg.h"
#include "core.h"

namespace {

constexpr uint32_t BG_PARAMS_PTR   = 0x800ECF84u;   // *u16 — per-area BG parameter block ptr
constexpr uint32_t YAW_ADDR        = 0x1F8000F2u;   // s16
constexpr uint32_t PITCH_ADDR      = 0x1F8000F0u;   // s16

// SOP-overlay animation counters cleared on INIT.
constexpr uint32_t ANIM_COUNTER_0  = 0x8010D390u;
constexpr uint32_t ANIM_COUNTER_1  = 0x8010D391u;
constexpr uint32_t ANIM_COUNTER_2  = 0x8010D392u;

// Wrap `v` into [0, mod). Faithful to the recomp's over-then-rollback loops (a normal `v % mod`
// would produce the same bytes for the values we see; kept explicit for byte-exact fidelity).
inline int32_t wrapMod(int32_t v, int32_t mod) {
  if (v < 0) {
    v += mod;
    while (v < 0) v += mod;
    v -= mod;
  }
  if (mod <= v) {
    v -= mod;
    while (mod <= v) v -= mod;
    v += mod;
  }
  return v;
}

}  // namespace

void ParallaxBg::step() {
  Core* c = core;
  const uint32_t sm = SM_ADDR;
  const uint8_t state = c->mem_r8(sm + 0);

  if (state == 1) {
    // ---- RUNNING ---------------------------------------------------------------------------
    const int32_t yaw    = (int32_t)c->mem_r16s(YAW_ADDR);
    const int32_t pitch  = (int32_t)c->mem_r16s(PITCH_ADDR);
    const uint16_t sX    = c->mem_r16(sm + 0x2Cu);   // X scroll speed (×yaw   >>12)
    const uint16_t sY    = c->mem_r16(sm + 0x2Eu);   // Y scroll speed (×pitch >>12)
    const uint16_t modX  = c->mem_r16(sm + 0x30u);   // X wrap modulus (grid_w × 16)
    const uint16_t modY  = c->mem_r16(sm + 0x32u);   // Y wrap modulus ((grid_h×0x8e8)/0x90)
    const uint8_t  tileH = c->mem_r8 (sm + 0x11u);

    const int32_t tileW = (int32_t)(int16_t)c->mem_r16(sm + 0x2Cu);
    int32_t x = ((tileW + 0x140) >> 1) - ((yaw   * (int32_t)(uint32_t)sX) >> 12);
    int32_t y = ((pitch * (int32_t)(uint32_t)sY) >> 12) + (int32_t)(uint32_t)tileH * 8 - 0x20;

    x = wrapMod(x, (int32_t)(uint32_t)modX);
    y = wrapMod(y, (int32_t)(uint32_t)modY);

    // Counter tick — signed-byte underflow → SM[3] = 1 (frame settled).
    const uint8_t cnt1 = (uint8_t)(c->mem_r8(sm + 0x38u) - 1);
    c->mem_w8(sm + 0x38u, cnt1);
    if ((int8_t)cnt1 <= 0) c->mem_w8(sm + 3u, 1);

    c->mem_w16(sm + 0x28u, (uint16_t)(int16_t)x);
    c->mem_w16(sm + 0x2Au, (uint16_t)(int16_t)y);
    return;
  }

  if (state == 0) {
    // ---- INIT ------------------------------------------------------------------------------
    const uint32_t P = c->mem_r32(BG_PARAMS_PTR);
    c->mem_w8 (sm + 0u, 1);
    c->mem_w8 (sm + 3u, 0);

    // Header: 6 u16s copied verbatim from the per-area BG params block.
    for (int i = 0; i < 6; i++) {
      c->mem_w16(sm + 4u + i * 2u, c->mem_r16(P + i * 2u));
    }

    // Two u8s packed at P+12/+13 (grid_w / grid_h).
    const uint8_t grid_w = c->mem_r8(P + 12u);
    const uint8_t grid_h = c->mem_r8(P + 13u);
    c->mem_w8(sm + 0x10u, grid_w);
    c->mem_w8(sm + 0x11u, grid_h);

    // Two u16 sizes for the following data-run buffer pointers.
    const uint16_t size0 = c->mem_r16(P + 14u);
    const uint16_t size1 = c->mem_r16(P + 16u);

    // Scroll-speed u16 pairs (packed as two u8s each), fixed constants 0x0900 / 0x08E8.
    c->mem_w8(sm + 0x2Cu, 0x00);
    c->mem_w8(sm + 0x2Du, 0x09);
    c->mem_w8(sm + 0x2Eu, 0xE8);
    c->mem_w8(sm + 0x2Fu, 0x08);

    // Data-run buffer pointers: base = P + 20 (u16-array start after the 10-u16 header).
    const uint32_t base = P + 20u;
    const uint32_t buf1 = base + (uint32_t)size1;
    const uint32_t buf2 = buf1 + (uint32_t)size0;
    c->mem_w32(sm + 0x14u, buf1);
    c->mem_w32(sm + 0x18u, base);
    c->mem_w32(sm + 0x1Cu, buf2);
    c->mem_w32(sm + 0x34u, buf2);

    // Scroll wrap moduli.
    c->mem_w16(sm + 0x30u, (uint16_t)(((uint32_t)grid_w * 0x900u) / 0x90u));
    c->mem_w16(sm + 0x32u, (uint16_t)(((uint32_t)grid_h * 0x8E8u) / 0x90u));

    c->mem_w8(sm + 0x38u, 1);

    // Clear the three SOP-overlay animation counters. Order mirrors the recomp (high→low), though
    // the ending bytes are the same either way.
    c->mem_w8(ANIM_COUNTER_2, 0);
    c->mem_w8(ANIM_COUNTER_1, 0);
    c->mem_w8(ANIM_COUNTER_0, 0);
    return;
  }

  // state >= 2: no-op (recomp: falls through the compound test `bVar2 < 2 && bVar2 == 0` false).
}
