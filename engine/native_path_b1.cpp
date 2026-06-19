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

void games_native_path_b1_init(void) {
  rec_set_override(0x8004FB20u, ov_8004FB20);
  rec_set_override(0x80089F68u, ov_80089F68);
  rec_set_override(0x8008BBC8u, ov_8008BBC8);
  rec_set_override(0x80097E10u, ov_80097E10);
  rec_set_override(0x80098DB0u, ov_80098DB0);
  rec_set_override(0x8007AC14u, ov_8007AC14);
  rec_set_override(0x8007AC40u, ov_8007AC40);
  rec_set_override(0x8007AC6Cu, ov_8007AC6C);
  rec_set_override(0x8007AC98u, ov_8007AC98);
  rec_set_override(0x8007AD14u, ov_8007AD14);
  rec_set_override(0x8007ACC4u, ov_8007ACC4);
  rec_set_override(0x8007AD40u, ov_8007AD40);
  rec_set_override(0x80098CE0u, ov_80098CE0);
  rec_set_override(0x8008CCE0u, ov_8008CCE0);
  rec_set_override(0x80084470u, ov_80084470);
}
