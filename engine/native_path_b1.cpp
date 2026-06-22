// Hand-native boot→cutscene path — batch b1: non-leaf init helpers whose callees are already native.
// These call sub-functions; the native body invokes them via rec_dispatch(c, addr) (same mechanism
// native_boot's rc0/rc1 use), which routes to the sub-function's native override (or interp if none).
// Each RE'd from its gen_func_<addr> body; trailing post-`jr ra` code in those bodies is recompiler
// over-run and is dropped. Args set in c->r[4..7] before the call, per o32. All A/B RAM-verified.
#include "core.h"
#include <stdint.h>

// Set up to 4 args and call a guest function through the dispatcher (routes to its native override).
static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x8004FB20 — memset(0x800BF548, 0, 700) via 0x8009A420.
static void ov_8004FB20(Core* c) { c->r[4] = 0x800BF548u; c->r[5] = 0; c->r[6] = 700; call_fn(c, 0x8009A420u); }

// 0x80089F68 — call 0x8008B040 (args passed through from caller) then return 1.
static void ov_80089F68(Core* c) { call_fn(c, 0x8008B040u); c->r[2] = 1; }

// 0x8008BBC8 — strncmp(a0, a1, 12) via 0x8009A640; return 1 if equal (result < 1 unsigned == result 0), else 0.
static void ov_8008BBC8(Core* c) { c->r[6] = 12; call_fn(c, 0x8009A640u); c->r[2] = (c->r[2] < 1u) ? 1u : 0u; }

// 0x80097E10 / 0x80098DB0 — call 0x80097E40 (a0,a1 passed through) with fixed (a2,a3) id pairs.
static void ov_80097E10(Core* c) { c->r[6] = 202; c->r[7] = 203; call_fn(c, 0x80097E40u); }
static void ov_80098DB0(Core* c) { c->r[6] = 204; c->r[7] = 205; call_fn(c, 0x80097E40u); }

// 0x8007AC14/AC40/AC6C/AC98/AD14 — memset(<global>, 0, <n>) via 0x8009A420. Bases from N<<16+signed-off:
//   AC14: 0x800F0000-32760 = 0x800E8008, n=144   AC40: 0x800F0000+9240 = 0x800F2418, n=524
//   AC6C: 0x80100000+1632  = 0x80100660, n=48    AC98: 0x800F0000-12264 = 0x800ED018, n=60
//   AD14: 0x800F0000-12200 = 0x800ED058, n=64
static void ov_8007AC14(Core* c) { c->r[4] = 0x800E8008u; c->r[5] = 0; c->r[6] = 144; call_fn(c, 0x8009A420u); }
static void ov_8007AC40(Core* c) { c->r[4] = 0x800F2418u; c->r[5] = 0; c->r[6] = 524; call_fn(c, 0x8009A420u); }
static void ov_8007AC6C(Core* c) { c->r[4] = 0x80100660u; c->r[5] = 0; c->r[6] = 48;  call_fn(c, 0x8009A420u); }
static void ov_8007AC98(Core* c) { c->r[4] = 0x800ED018u; c->r[5] = 0; c->r[6] = 60;  call_fn(c, 0x8009A420u); }
static void ov_8007AD14(Core* c) { c->r[4] = 0x800ED058u; c->r[5] = 0; c->r[6] = 64;  call_fn(c, 0x8009A420u); }

// 0x8007ACC4 — 8× memset(0x80100400 + i*76, 0, 76) via 0x8009A420 (eight stride-76 records).
static void ov_8007ACC4(Core* c) {
  for (uint32_t i = 0; i < 8; i++) { c->r[4] = 0x80100400u + i * 76; c->r[5] = 0; c->r[6] = 76; call_fn(c, 0x8009A420u); }
}

// 0x8007AD40 — memset(0x800EC188, 0, 2560) via 0x8009A420, then for 40 stride-64 records set
// byte[+7] = (i & 7) and u16[+12] = 4096.
static void ov_8007AD40(Core* c) {
  uint32_t base = 0x800EC188u;
  c->r[4] = base; c->r[5] = 0; c->r[6] = 2560; call_fn(c, 0x8009A420u);
  for (uint32_t i = 0; i < 40; i++) { uint32_t p = base + i * 64; c->mem_w8(p + 7, (uint8_t)(i & 7)); c->mem_w16(p + 12, 4096); }
}

// 0x80098CE0 — return (a0 != 0 && 0x800982A0(*0x800AC5A0) == 0) ? 1 : 0; also store that to 0x800AC59C.
static void ov_80098CE0(Core* c) {
  if (c->r[4] == 0) { c->mem_w32(0x800AC59Cu, 0); c->r[2] = 0; return; }
  c->r[4] = c->mem_r32(0x800AC5A0u); call_fn(c, 0x800982A0u);
  uint32_t v = (c->r[2] == 0) ? 1u : 0u;
  c->mem_w32(0x800AC59Cu, v); c->r[2] = v;
}

// 0x8008CCE0 — clear a cluster of globals at 0x801027xx, then 0x8008CFF0(0, *0x8010272C), then clear three more.
static void ov_8008CCE0(Core* c) {
  uint32_t cnt = c->mem_r32(0x8010272Cu);
  c->mem_w32(0x80102714u, 0); c->mem_w32(0x80102710u, 0); c->mem_w32(0x8010270Cu, 0); c->mem_w32(0x80102704u, 0);
  c->r[4] = 0; c->r[5] = cnt; call_fn(c, 0x8008CFF0u);
  c->mem_w32(0x801026F4u, 0); c->mem_w16(0x801026ECu, 0); c->mem_w32(0x801026E8u, 0);
}

// 0x80084470 — GTE matrix×vector: load 5 ctrl regs from the matrix at a0, 2 data regs from the vector
// at a1, run GTE op 0x4A486012, store IR-pack results (data regs 25/26/27) to a2; return a2.
static void ov_80084470(Core* c) {
  uint32_t m = c->r[4], v = c->r[5], o = c->r[6];
  gte_write_ctrl(0, c->mem_r32(m + 0));  gte_write_ctrl(1, c->mem_r32(m + 4));  gte_write_ctrl(2, c->mem_r32(m + 8));
  gte_write_ctrl(3, c->mem_r32(m + 12)); gte_write_ctrl(4, c->mem_r32(m + 16));
  gte_write_data(0, c->mem_r32(v + 0));  gte_write_data(1, c->mem_r32(v + 4));
  gte_op(c, 0x4A486012u);
  c->mem_w32(o + 0, gte_read_data(25)); c->mem_w32(o + 4, gte_read_data(26)); c->mem_w32(o + 8, gte_read_data(27));
  c->r[2] = o;
}

// 0x80079528 — strlen(a0). (gen_func_80079528's body past the first `jr ra` — the 8009A640 call — is
// the next function's recompiler over-run; the real body is just the count loop.)
static void ov_80079528(Core* c) {
  uint32_t p = c->r[4], n = 0;
  if (c->mem_r8(p) != 0) { for (;;) { p++; n++; if (c->mem_r8(p) == 0) break; } }
  c->r[2] = n;
}

// 0x8007B328 — zero an 8-byte descriptor at 0x800FB160 (via 0x8009A420), seed bytes
// [1]=1,[4]=7,[5]=9 (rest 0), then call 0x8007B2C0(0) to load the ascending weight ramp.
static void ov_8007B328(Core* c) {
  uint32_t b = 0x800FB160u;
  c->r[4] = b; c->r[5] = 0; c->r[6] = 8; call_fn(c, 0x8009A420u);
  c->mem_w8(b + 1, 1); c->mem_w8(b + 2, 0); c->mem_w8(b + 3, 0);
  c->mem_w8(b + 4, 7); c->mem_w8(b + 5, 9); c->mem_w8(b + 6, 0); c->mem_w8(b + 7, 0);
  c->r[4] = 0; call_fn(c, 0x8007B2C0u);
}

// 0x8007B38C — scatter the 0x800FB160 descriptor's bytes [1..7] into the HW-shadow block at 0x800BF870
// (offsets 51/1502/1503/26/27/1500/1501), then call 0x8007B2C0(desc[6] & 0xff).
static void ov_8007B38C(Core* c) {
  uint32_t s = 0x800FB160u, d = 0x800BF870u;
  uint32_t b6 = c->mem_r8(s + 6);
  c->mem_w8(d + 1500, (uint8_t)b6);
  c->mem_w8(d + 51,   (uint8_t)c->mem_r8(s + 1));
  c->mem_w8(d + 1502, (uint8_t)c->mem_r8(s + 2));
  c->mem_w8(d + 1503, (uint8_t)c->mem_r8(s + 3));
  c->mem_w8(d + 26,   (uint8_t)c->mem_r8(s + 4));
  c->mem_w8(d + 27,   (uint8_t)c->mem_r8(s + 5));
  c->mem_w8(d + 1501, (uint8_t)c->mem_r8(s + 7));
  c->r[4] = b6 & 0xff; call_fn(c, 0x8007B2C0u);
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

void games_native_path_b1_init(void) {
}
