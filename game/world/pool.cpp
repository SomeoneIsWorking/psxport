// game/world/pool.cpp — PC-native object-pool / control-block init (the boot-time zero/seed cluster) and scheduler-frame helpers.

#include "core.h"
#include "cfg.h"
#include "pool.h"
#include "verify_gate.h"
#include "mtx.h"          // class Mtx — libgte helpers (MR_init, ...)
#include "placement.h"    // ov_spawn_with_parent (FUN_80072DDC)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x800796DC — zero the 104-byte control block at 0x800BF808 (via 0x8009A420), seed two bytes,
// clear ~30 scratchpad fields, call 0x800782F0(b[168],b[169]) and 0x800508A8(), then select a
// uniform arg (0 if state byte == 0 or in {7,8}, else 255) for 0x8005082C(arg,arg,arg).
static void ov_800796DC(Core* c) {
  uint32_t b = 0x800BF808u;
  c->r[4]=b; c->r[5]=0; c->r[6]=104; call_fn(c, 0x8009A420u);   // memset(b, 0, 104)
  uint32_t a4 = c->mem_r8(0x800BF8B0u);
  uint32_t a5 = c->mem_r8(0x800BF8B1u);
  c->mem_w8(b + 41, 255);
  c->mem_w8(b + 40, 255);
  const uint32_t S = 0x1F800000u;            // scratchpad
  c->mem_w8 (S + 638, 0); c->mem_w32(S + 584, 0); c->mem_w8 (S + 582, 0);
  c->mem_w32(S + 588, 0); c->mem_w8 (S + 592, 0); c->mem_w8 (S + 601, 0);
  c->mem_w8 (S + 602, 0); c->mem_w8 (S + 603, 0); c->mem_w8 (S + 634, 0);
  c->mem_w8 (S + 560, 0);
  c->mem_w8 (b + 7, 1);
  c->mem_w8 (S + 310, 0); c->mem_w8 (S + 311, 0);
  c->mem_w32(S + 388, 0); c->mem_w32(S + 532, 0); c->mem_w32(S + 520, 0); c->mem_w32(S + 640, 0);
  c->mem_w8 (S + 561, 0); c->mem_w8 (S + 593, 0); c->mem_w8 (S + 562, 0);
  c->mem_w8 (S + 595, 0); c->mem_w8 (S + 563, 0); c->mem_w8 (S + 571, 0);
  c->engine.sceneTransition.areaMaskTrigger((uint8_t)a4, (uint8_t)a5);   // was rec_dispatch 0x800782F0
  call_fn(c, 0x800508A8u);
  uint32_t v = c->mem_r8(S + 566);
  uint32_t arg = (v == 0u || (uint32_t)(v - 7u) < 2u) ? 0u : 255u;
  c->r[4]=arg; c->r[5]=arg; c->r[6]=arg; call_fn(c, 0x8005082Cu);
  c->mem_w8(0x800BF9D4u, 0);
  c->r[2] = 0x800C0000u;   // incidental v0: recomp epilogue `lui v0,0x800c; sb zero,-1580(v0)` leaves the store base
}

// 0x8007B18C — top-level object-pool init. Calls 0x8004FB20 then 0x800798F8; zeroes 520 contiguous
// 68-byte slots at 0x800F2740; builds a downward-growing free-list of slot pointers at 0x800E7E74
// (head init 0x800ED8C0, pushing the 520 slot payloads last→first, payload base 0x800FB11C step -68);
// records the free count (520) at 0x800ED098; then runs eight further sub-inits.
static void ov_8007B18C(Core* c) {
  call_fn(c, 0x8004FB20u);
  call_fn(c, 0x800798F8u);

  for (int i = 0; i < 520; i++) {
    c->r[4] = 0x800F2740u + (uint32_t)i * 68u; c->r[5] = 0; c->r[6] = 68;
    call_fn(c, 0x8009A420u);                              // memset(slot, 0, 68)
  }

  c->mem_w32(0x800E7E74u, 0x800ED8C0u);                   // free-list head
  uint32_t payload = 0x800FB11Cu;                         // last slot (0x800FB160 - 68)
  for (int i = 0; i < 520; i++) {
    uint32_t head = c->mem_r32(0x800E7E74u);
    c->mem_w32(0x800E7E74u, head - 4);
    c->mem_w32(head - 4, payload);
    payload -= 68u;
  }
  c->mem_w16(0x800ED098u, 520);                           // free count

  call_fn(c, 0x8007ACC4u);
  call_fn(c, 0x8007A810u);
  call_fn(c, 0x8007AC14u);
  call_fn(c, 0x8007AC40u);
  call_fn(c, 0x8007AC6Cu);
  call_fn(c, 0x8007AC98u);
  call_fn(c, 0x8007AD14u);
  call_fn(c, 0x8007AD40u);
}

// 0x800263E8 — area object-record seeding. Selects a per-area byte sequence (table 0x8009D414 indexed
// by the area byte 0x800BF870); for each byte up to a 0xFF terminator, allocate a record via
// 0x8007AD98 and stamp record[0]=1, record[2]=byte. RE'd 1:1 from disas 0x800263E8 (the FUN_8007AD98
// allocator stays a PSX leaf via rec_dispatch). Empty (first byte 0xFF) → no-op.
static void ov_800263E8(Core* c) {
  uint32_t area = c->mem_r8(0x800BF870u);
  uint32_t s0   = c->mem_r32(0x8009D414u + area * 4u);
  if (c->mem_r8(s0) != 0xFFu) {
    do {
      call_fn(c, 0x8007AD98u);                 // v0 = newly-allocated record ptr
      uint32_t rec = c->r[2];
      c->mem_w8(rec + 0, 1);
      uint8_t b = c->mem_r8(s0); s0 += 1;
      c->mem_w8(rec + 2, b);
    } while (c->mem_r8(s0) != 0xFFu);
  }
  c->r[2] = 0xFFu;   // incidental v0: both the skip and loop-exit paths leave the 0xFF terminator in v0
}

// 0x80075240 — reset the control block at 0x800BE1F8: call 0x80075D58 (leaf, entry a0 unchanged), seed
// clamp limits (s16 +42/+44 = 0x7FFF, +46/+48 = 0x1FFF), word +0 = 768, zero +24/+20, then call
// 0x80075824(blk) and 0x80099490(blk), finally zero word +0 again and bytes +50/+51. RE'd 1:1 from
// disas 0x80075240. The three callees stay PSX leaves via rec_dispatch. Incidental v0 = the last
// call's (0x80099490) return — left in c->r[2] automatically, no manual mirror needed.
static void ov_80075240(Core* c) {
  const uint32_t b = 0x800BE1F8u;
  call_fn(c, 0x80075D58u);                  // a0 = entry a0 (matches recomp: jal precedes a0=blk)
  c->mem_w16(b + 42, 0x7FFF); c->mem_w16(b + 44, 0x7FFF);
  c->mem_w16(b + 46, 0x1FFF); c->mem_w16(b + 48, 0x1FFF);
  c->mem_w32(b + 0, 768);
  c->mem_w32(b + 24, 0); c->mem_w32(b + 20, 0);
  c->r[4] = b; call_fn(c, 0x80075824u);
  c->r[4] = b; call_fn(c, 0x80099490u);
  c->mem_w32(b + 0, 0);
  c->mem_w8(b + 50, 0); c->mem_w8(b + 51, 0);
}

// 0x800783DC — per-area VIEW/SCROLL setup. Calls a leaf (0x80048D3C) with the entry a0, builds the view
// control block at 0x800E7E80 from the area control block at 0x800BF870, then publishes four fields into the
// scratchpad camera (0x1F800207/0160/0162/0164). RE'd 1:1 from disas 0x800783DC. The two callees (0x80048D3C,
// and 0x80072DDC on the mode==3 path) stay PSX leaves via rec_dispatch. Incidental v0 = 0x1F800000 (the
// epilogue's lui-loaded scratchpad base, left in v0 on every return path).
static void ov_800783DC(Core* c) {
  const uint32_t S0 = 0x800E7E80u;   // view control block
  const uint32_t A  = 0x800BF870u;   // area control block

  call_fn(c, 0x80048D3Cu);           // a0 = entry a0 (matches recomp: jal precedes any arg setup)

  c->mem_w8 (S0, 3);
  c->mem_w16(S0 + 370, 60);
  if (c->mem_r8(A + 16) == 0) {
    uint32_t v = c->mem_r8(A + 13);
    c->mem_w16(S0 + 366, (uint16_t)v); c->mem_w16(S0 + 368, (uint16_t)v);
  } else {
    uint32_t v = c->mem_r16(0x1F800194u);
    c->mem_w8(A + 16, 0);
    c->mem_w16(S0 + 366, (uint16_t)v); c->mem_w16(S0 + 368, (uint16_t)v);
  }

  c->mem_w8 (S0 + 108, (uint8_t) c->mem_r8 (A + 28));
  c->mem_w8 (S0 + 109, (uint8_t) c->mem_r8 (A + 29));
  c->mem_w16(S0 + 382, (uint16_t)c->mem_r16(A + 46));
  c->mem_w8 (S0 + 372, (uint8_t) c->mem_r8 (A + 17));

  uint32_t mode = c->mem_r8(A);
  if (mode == 3) {
    c->r[4] = 0; c->r[5] = 3; c->r[6] = 4; c->r[7] = 27;
    c->engine.placement.spawnWithParent();
    uint32_t v0 = c->r[2];
    c->mem_w32(v0 + 28, 0x8010B37Cu);
    c->mem_w32(S0 + 16, v0);
  } else {
    if (c->mem_r8(0x1F800134u) != 0) {
      uint8_t vb = c->mem_r8(A + 1480);
      c->mem_w32(S0 + 44, c->mem_r32(A + 32));
      c->mem_w32(S0 + 48, c->mem_r32(A + 36));
      c->mem_w32(S0 + 52, c->mem_r32(A + 40));
      c->mem_w8 (0x1F800134u, 0);
      c->mem_w8 (S0 + 348, 0);
      c->mem_w8 (S0 + 42, vb);
    } else {
      uint32_t tbl = c->mem_r32(0x800A54A8u + mode * 4u);
      uint32_t a0  = tbl + (uint32_t)c->mem_r8(A + 1) * 8u;
      int16_t  h0  = (int16_t)c->mem_r16(a0 + 0);
      int16_t  s17e = (int16_t)c->mem_r16(S0 + 382);
      c->mem_w32(S0 + 44, (uint32_t)((int32_t)h0 << 16));
      c->mem_w32(S0 + 48, (uint32_t)((int32_t)(int16_t)c->mem_r16(a0 + 2) << 16));
      if (s17e < 0) c->mem_w16(S0 + 50, (uint16_t)(c->mem_r16(S0 + 50) + 70));
      c->mem_w32(S0 + 52, (uint32_t)((int32_t)(int16_t)c->mem_r16(a0 + 4) << 16));
      uint32_t a0h = c->mem_r16(a0 + 6);              // lhu (zero-extended)
      c->mem_w8(S0 + 42,  (uint8_t)(a0h & 0x7f));
      c->mem_w8(S0 + 348, (uint8_t)((a0h >> 7) & 1));
      c->mem_w8(S0 + 327, (uint8_t)((a0h >> 8) & 1));
      if (a0h & 0x800) {
        c->mem_w8(0x800BF816u, 1);
        c->mem_w8(0x800BF817u, (uint8_t)(((uint32_t)((int32_t)(int16_t)a0h & 0xf000)) >> 12));
      }
    }
    uint8_t s = c->mem_r8(0x1F800236u);
    if ((uint32_t)(s - 5) < 2u)                       // s in {5,6}
      c->mem_w16(S0 + 50, (uint16_t)(c->mem_r16(S0 + 50) - 1000));
  }

  // common epilogue: publish into the scratchpad camera fields
  c->mem_w8 (0x1F800207u, (uint8_t) c->mem_r8 (S0 + 42));
  c->mem_w16(0x1F800160u, (uint16_t)c->mem_r16(S0 + 46));
  c->mem_w16(0x1F800162u, (uint16_t)c->mem_r16(S0 + 50));
  c->mem_w16(0x1F800164u, (uint16_t)c->mem_r16(S0 + 54));
  c->r[2] = 0x1F800000u;   // incidental v0
}

// 0x80078610 — final per-area view init: zero two control blocks (scratchpad 0x1F8000D0, main 0x800E8008),
// seed fixed view params, copy the three view vectors from the 0x800E7E80 block (built by 0x800783DC) into
// both blocks (the scratchpad copy biased by 0xFEC00000 / 0xF92A0000), run the camera orient/look-at matrix
// builder 0x8006D02C, then set the area's draw-range half-word 0x801003F8 (233 if area mode==3 else 350) and
// run 0x800846F0 with it. RE'd 1:1 from disas 0x80078610. Callees (0x80051794×2, 0x8006D02C, 0x800846F0)
// stay PSX leaves via rec_dispatch. Incidental v0 = 0x800846F0's return (left in c->r[2] automatically).
static void ov_80078610(Core* c) {
  const uint32_t E = 0x800E8008u;   // s0: main control block
  const uint32_t V = 0x800E7E80u;   // s1: view control block (built by ov_800783DC)
  const uint32_t P = 0x1F8000D0u;   // v1 = s2-40: scratchpad control block

  Mtx::identity(c, 0x1F8000F8u);   // MR_init(s2)     — native class Mtx (was 0x80051794)
  Mtx::identity(c, 0x1F800118u);   // MR_init(s2+32)  — native class Mtx (was 0x80051794)

  c->mem_w16(P + 30, (uint16_t)-1750);
  c->mem_w16(P + 28, 4096);
  c->mem_w16(P + 24, 0); c->mem_w16(P + 26, 0);
  c->mem_w16(P + 32, 0); c->mem_w16(P + 34, 0); c->mem_w16(P + 36, 0);
  c->mem_w16(E + 112, 1024);
  c->mem_w32(E + 88, 0);
  c->mem_w32(E + 32, 0); c->mem_w32(E + 36, 0); c->mem_w32(E + 40, 0);
  c->mem_w32(E + 20, 0); c->mem_w32(E + 24, 0);
  c->mem_w16(E + 110, 256);

  uint32_t a1 = c->mem_r32(V + 48);
  uint32_t a2 = c->mem_r32(V + 44);
  uint32_t a3 = c->mem_r32(V + 52);
  c->mem_w16(E + 108, 1750);
  c->mem_w32(P + 16, a1); c->mem_w32(P + 12, a2); c->mem_w32(P + 20, a3);
  c->mem_w32(E + 8, a1);
  a1 += 0xFEC00000u;
  c->mem_w32(E + 16, a3);
  a3 += 0xF92A0000u;
  c->mem_w32(E + 12, a2);
  c->mem_w32(P + 0, a2);
  c->mem_w32(P + 4, a1);
  c->mem_w32(P + 8, a3);
  c->r[4] = E; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
  call_fn(c, 0x8006D02Cu);

  uint32_t v0 = (c->mem_r8(0x800BF870u) == 3) ? 233u : 350u;
  c->mem_w16(0x801003F8u, (uint16_t)v0);
  c->r[2] = 0x80100000u;   // lui v0,0x8010 — survives to return (0x800846F0 leaves v0); incidental v0
  c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(0x801003F8u);
  call_fn(c, 0x800846F0u);
}

// 0x80074F24 — per-area STATE-INDEX select + apply. Early-out if scratchpad 0x1F800137==1 or area
// 0x800BF870==21 (both leave a fixed v0). Otherwise pick an index s0: 42 if (area!=15 && 0x800BF873!=0);
// 10 if (a0==6 && 9<=0x800BF871<15); else a bit of the area mask 0x800BFE56 (shifted by a0) selects table
// 0x800A4F68 / 0x800A4F50, indexed by a0. Then call 0x800750D8(s0,1), publish s0 to scratchpad 0x1F80023B,
// clear 0x800BE22B. RE'd 1:1 from disas 0x80074F24. a0 is the area byte (caller passes 0x800BF870). The
// callee 0x800750D8 stays a PSX leaf. Incidental v0: early-outs leave 1 / 21; the work path leaves the
// last lui value 0x800C0000.
static void ov_80074F24(Core* c) {
  if (c->mem_r8(0x1F800137u) == 1) { c->r[2] = 1; return; }
  uint32_t m = c->mem_r8(0x800BF870u);
  if (m == 21) { c->r[2] = 21; return; }
  uint32_t q = c->r[4] & 0xFFu;                       // a0 (area byte)

  uint32_t s0 = 0; bool have = false;
  if (m != 15 && c->mem_r8(0x800BF873u) != 0) { s0 = 42; have = true; }
  if (!have && q == 6) {
    uint32_t p = c->mem_r8(0x800BF871u);
    if (p >= 9 && p < 15) { s0 = 10; have = true; }
  }
  if (!have) {
    uint32_t w = c->mem_r16(0x800BFE56u);
    uint32_t tbl = ((w >> (q & 31)) & 1u) ? 0x800A4F68u : 0x800A4F50u;
    s0 = c->mem_r8(tbl + q);
  }

  c->r[4] = s0; c->r[5] = 1; call_fn(c, 0x800750D8u);
  c->mem_w8(0x1F80023Bu, (uint8_t)s0);
  c->mem_w8(0x800BE22Bu, 0);
  c->r[2] = 0x800C0000u;   // incidental v0
}

// Shared inline A/B gate for the once-per-field-load WORLD inits wired into engine_stage case-0.
// Runs the native body, snapshots+rolls back, super-calls the recomp body, and diffs full
// main-RAM (excl. the dead stack window) + scratchpad + v0. Prints EVERY match (record_gate is
// silent on single matches), giving positive confirmation the once-per-load fn actually ran —
// matching placement.cpp's gate style. Counters are per-call-site (passed in).
static void world_init_gate(Core* c, void (*fn)(Core*), uint32_t super, const char* tag,
                            long* ng, long* nb) {
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  fn(c); uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, super); uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  if (ro >= 0 || so >= 0 || v0_n != v0_o)
    { if ((*nb)++ < 40) fprintf(stderr, "[%s] MISMATCH v0 n=%x o=%x ram@%x spad@%x sp=%x\n", tag, v0_n, v0_o, ro, so, sp); }
  else fprintf(stderr, "[%s] match #%ld\n", tag, ++(*ng));
}

// Public GATED entries — the native field case-0 prefix (engine_stage.cpp ov_field_run) calls these
// directly (PC calls PC) instead of rec_dispatch. Native by default; A/B-vs-recomp when the channel
// is on. The leaves each fn dispatches stay PSX via rec_dispatch.
void Pool::init() {                                       // 0x8007B18C — object-pool init
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("poolinitverify") ? 1 : 0;
  if (!s_v) { ov_8007B18C(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_8007B18C, 0x8007B18Cu, "poolinitverify", &ng, &nb);
}
void Pool::resetControlBlock() {                          // 0x800796DC — control-block reset + sub-inits
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init796dcverify") ? 1 : 0;
  if (!s_v) { ov_800796DC(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800796DC, 0x800796DCu, "init796dcverify", &ng, &nb);
}
void Pool::seedAreaObjects() {                            // 0x800263E8 — area object-record seeding
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init263e8verify") ? 1 : 0;
  if (!s_v) { ov_800263E8(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800263E8, 0x800263E8u, "init263e8verify", &ng, &nb);
}
void Pool::reset75240() {                                 // 0x80075240 — clamp/control-block reset
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init75240verify") ? 1 : 0;
  if (!s_v) { ov_80075240(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_80075240, 0x80075240u, "init75240verify", &ng, &nb);
}
void Pool::setupViewScroll() {                            // 0x800783DC — per-area view/scroll setup
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init783dcverify") ? 1 : 0;
  if (!s_v) { ov_800783DC(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800783DC, 0x800783DCu, "init783dcverify", &ng, &nb);
}
void Pool::finalViewInit() {                              // 0x80078610 — final per-area view init
  Core* c = core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init78610verify") ? 1 : 0;
  if (!s_v) { ov_80078610(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_80078610, 0x80078610u, "init78610verify", &ng, &nb);
}
void Pool::selectStateIndex(uint8_t area) {               // 0x80074F24 — per-area state-index select (a0=area)
  Core* c = core;
  c->r[4] = area;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init74f24verify") ? 1 : 0;
  if (!s_v) { ov_80074F24(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_80074F24, 0x80074F24u, "init74f24verify", &ng, &nb);
}
