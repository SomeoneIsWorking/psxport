// game/world/pool.cpp — PC-native object-pool / control-block init (the boot-time zero/seed cluster)
// and scheduler-frame helpers.
//
// All method bodies below reimplement per-guest-address inits (0x800796DC, 0x8007B18C, 0x800263E8,
// 0x80075240, 0x800783DC, 0x80078610, 0x80074F24) 1:1 from their disas. Sub-leaves each dispatches
// stay PSX via rec_dispatch. Incidental v0 mirrors the recomp epilogue where noted so downstream
// content-interface reads see the same value.

#include "core.h"
#include "pool.h"
#include "mtx.h"          // class Mtx — libgte helpers (identity)
#include "placement.h"    // Placement::spawnWithParent (FUN_80072DDC)
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x800796DC — zero the 104-byte control block at 0x800BF808, seed two bytes, clear ~30 scratchpad
// fields, call 0x800782F0(b[168],b[169]) and 0x800508A8(), then select a uniform arg for 0x8005082C.
void Pool::resetControlBlock() {
  Core* c = core;
  uint32_t b = 0x800BF808u;
  c->r[4]=b; c->r[5]=0; c->r[6]=104; call_fn(c, 0x8009A420u);   // memset(b, 0, 104)
  uint32_t a4 = c->mem_r8(0x800BF8B0u);
  uint32_t a5 = c->mem_r8(0x800BF8B1u);
  c->mem_w8(b + 41, 255);
  c->mem_w8(b + 40, 255);
  const uint32_t S = 0x1F800000u;
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
  c->r[2] = 0x800C0000u;   // incidental v0
}

// 0x8007B18C — top-level object-pool init. Zeroes 520 68-byte slots at 0x800F2740; builds a
// downward-growing free-list of slot pointers at 0x800E7E74; runs eight further sub-inits.
void Pool::init() {
  Core* c = core;
  call_fn(c, 0x8004FB20u);
  call_fn(c, 0x800798F8u);

  for (int i = 0; i < 520; i++) {
    c->r[4] = 0x800F2740u + (uint32_t)i * 68u; c->r[5] = 0; c->r[6] = 68;
    call_fn(c, 0x8009A420u);                              // memset(slot, 0, 68)
  }

  c->mem_w32(0x800E7E74u, 0x800ED8C0u);                   // free-list head
  uint32_t payload = 0x800FB11Cu;                         // last slot
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

// 0x800263E8 — area object-record seeding. Selects a per-area byte sequence (table 0x8009D414);
// for each byte up to a 0xFF terminator, allocate a record via 0x8007AD98 and stamp record[0]=1,
// record[2]=byte.
void Pool::seedAreaObjects() {
  Core* c = core;
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
  c->r[2] = 0xFFu;   // incidental v0
}

// 0x80075240 — reset the control block at 0x800BE1F8: call 0x80075D58 leaf, seed clamp limits, run
// 0x80075824 and 0x80099490 on it, zero word +0 again and bytes +50/+51.
void Pool::reset75240() {
  Core* c = core;
  const uint32_t b = 0x800BE1F8u;
  call_fn(c, 0x80075D58u);
  c->mem_w16(b + 42, 0x7FFF); c->mem_w16(b + 44, 0x7FFF);
  c->mem_w16(b + 46, 0x1FFF); c->mem_w16(b + 48, 0x1FFF);
  c->mem_w32(b + 0, 768);
  c->mem_w32(b + 24, 0); c->mem_w32(b + 20, 0);
  c->r[4] = b; call_fn(c, 0x80075824u);
  c->r[4] = b; call_fn(c, 0x80099490u);
  c->mem_w32(b + 0, 0);
  c->mem_w8(b + 50, 0); c->mem_w8(b + 51, 0);
}

// 0x800783DC — per-area VIEW/SCROLL setup. Calls a leaf (0x80048D3C), builds the view control block
// at 0x800E7E80 from the area control block at 0x800BF870, publishes four fields into the scratchpad
// camera (0x1F800207/0160/0162/0164).
void Pool::setupViewScroll() {
  Core* c = core;
  const uint32_t S0 = 0x800E7E80u;
  const uint32_t A  = 0x800BF870u;

  call_fn(c, 0x80048D3Cu);

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
      int16_t  h0  = c->mem_r16s(a0 + 0);
      int16_t  s17e = c->mem_r16s(S0 + 382);
      c->mem_w32(S0 + 44, (uint32_t)((int32_t)h0 << 16));
      c->mem_w32(S0 + 48, (uint32_t)(c->mem_r16s(a0 + 2) << 16));
      if (s17e < 0) c->mem_w16(S0 + 50, (uint16_t)(c->mem_r16(S0 + 50) + 70));
      c->mem_w32(S0 + 52, (uint32_t)(c->mem_r16s(a0 + 4) << 16));
      uint32_t a0h = c->mem_r16(a0 + 6);
      c->mem_w8(S0 + 42,  (uint8_t)(a0h & 0x7f));
      c->mem_w8(S0 + 348, (uint8_t)((a0h >> 7) & 1));
      c->mem_w8(S0 + 327, (uint8_t)((a0h >> 8) & 1));
      if (a0h & 0x800) {
        c->mem_w8(0x800BF816u, 1);
        c->mem_w8(0x800BF817u, (uint8_t)(((uint32_t)((int32_t)(int16_t)a0h & 0xf000)) >> 12));
      }
    }
    uint8_t s = c->mem_r8(0x1F800236u);
    if ((uint32_t)(s - 5) < 2u)
      c->mem_w16(S0 + 50, (uint16_t)(c->mem_r16(S0 + 50) - 1000));
  }

  c->mem_w8 (0x1F800207u, (uint8_t) c->mem_r8 (S0 + 42));
  c->mem_w16(0x1F800160u, (uint16_t)c->mem_r16(S0 + 46));
  c->mem_w16(0x1F800162u, (uint16_t)c->mem_r16(S0 + 50));
  c->mem_w16(0x1F800164u, (uint16_t)c->mem_r16(S0 + 54));
  c->r[2] = 0x1F800000u;   // incidental v0
}

// 0x80078610 — final per-area view init: zero two control blocks, seed fixed view params, copy the
// three view vectors from 0x800E7E80 into both blocks, run camera orient/look-at 0x8006D02C, set the
// area's draw-range 0x801003F8 and run 0x800846F0 with it.
void Pool::finalViewInit() {
  Core* c = core;
  const uint32_t E = 0x800E8008u;
  const uint32_t V = 0x800E7E80u;
  const uint32_t P = 0x1F8000D0u;

  c->mtx.identity(0x1F8000F8u);   // MR_init(s2)
  c->mtx.identity(0x1F800118u);   // MR_init(s2+32)

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
  c->r[2] = 0x80100000u;   // incidental v0
  c->r[4] = (uint32_t)c->mem_r16s(0x801003F8u);
  call_fn(c, 0x800846F0u);
}

// 0x80074F24 — per-area STATE-INDEX select + apply. Early-out if scratchpad 0x1F800137==1 or area
// 0x800BF870==21. Otherwise pick an index s0, call 0x800750D8(s0,1), publish s0 to 0x1F80023B.
void Pool::selectStateIndex(uint8_t area) {
  Core* c = core;
  c->r[4] = area;
  if (c->mem_r8(0x1F800137u) == 1) { c->r[2] = 1; return; }
  uint32_t m = c->mem_r8(0x800BF870u);
  if (m == 21) { c->r[2] = 21; return; }
  uint32_t q = c->r[4] & 0xFFu;

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
