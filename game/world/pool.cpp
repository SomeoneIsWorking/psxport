// game/world/pool.cpp — PC-native object-pool / control-block init (the boot-time zero/seed cluster)
// and scheduler-frame helpers.
//
// All method bodies below reimplement per-guest-address inits (0x800796DC, 0x8007B18C, 0x800263E8,
// 0x80075240, 0x800783DC, 0x80078610, 0x80074F24) 1:1 from their disas. Sub-leaves each dispatches
// stay PSX via rec_dispatch. Incidental v0 mirrors the recomp epilogue where noted so downstream
// content-interface reads see the same value.

#include "core.h"
#include "game_ctx.h"
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
  eng(c).sceneTransition.areaMaskTrigger((uint8_t)a4, (uint8_t)a5);   // was rec_dispatch 0x800782F0
  eng(c).modeStateArm.armFromAreaTable();                                  // was rec_dispatch 0x800508A8
  uint32_t v = c->mem_r8(S + 566);
  uint8_t arg = (v == 0u || (uint32_t)(v - 7u) < 2u) ? 0u : 0xFFu;
  eng(c).modeStateArm.arm(arg, arg, arg);                                  // was rec_dispatch 0x8005082C
  c->mem_w8(0x800BF9D4u, 0);
  c->r[2] = 0x800C0000u;   // incidental v0
}

// FUN_8004FB20 — zero 700 bytes at 0x800BF548. Trivial memset wrapper. Every field of this
// region is a per-area scene control block that later inits fill in.
void Pool::clearBf548Region() {
  Core* c = core;
  for (uint32_t off = 0; off < 700; off++) c->mem_w8(0x800BF548u + off, 0);
}

// FUN_800798F8 — the 5 typed object pools + list-head init. See pool.h for the pool table + free-
// list head/count addresses. Each pool is built as a singly-linked free-list via slot[+0x24] =
// next-slot (last is 0), with slot[+0x28] = pool class byte. All slot bodies are memset to 0.
// Faithful to the recomp; the redundant slot[+0x0C] writes (memset already zeroed) are preserved
// for byte-exact fidelity.
void Pool::initTypedPools() {
  Core* c = core;
  // Active list heads (zeroed once — will be built up as objects spawn).
  c->mem_w32(0x800FB168u, 0);   // T2_OBJLIST_HEAD_1 (list-1 walkAll first loop)
  c->mem_w32(0x800F23A8u, 0);
  c->mem_w32(0x800F2624u, 0);   // T2_OBJLIST_HEAD_2 (walkAll second loop + walkList2)
  c->mem_w32(0x800F239Cu, 0);

  struct PoolSpec {
    uint32_t base;     // guest base addr
    uint32_t stride;   // slot size in bytes
    uint32_t count;    // slot count
    uint8_t  klass;    // slot[+0x28] class byte
    uint8_t  meta;     // slot[+0x0C] byte (all zero, matches recomp)
    uint32_t headVar;  // free-list head ptr addr
    uint32_t countVar; // free-count byte addr
  };
  static const PoolSpec pools[5] = {
    { 0x800ED8D8u, 0x88u,  0x34u, 0, 0, 0x800E8098u, 0x800E7E7Cu },
    { 0x800EF478u, 0xC4u,  0x3Au, 1, 0, 0x800E80A0u, 0x800E7E7Du },
    { 0x800FE198u, 0xD0u,  0x2Au, 2, 0, 0x800F2398u, 0x800ED8CCu },
    { 0x800FB858u, 0x108u, 0x28u, 3, 0, 0x800ED8D4u, 0x800ED8C5u },
    { 0x800FB218u, 0x140u, 0x05u, 4, 0, 0x800ED8D0u, 0x800ED8C4u },
  };
  for (const PoolSpec& p : pools) {
    for (uint32_t i = 0; i < p.count; i++) {
      const uint32_t slot = p.base + i * p.stride;
      for (uint32_t off = 0; off < p.stride; off++) c->mem_w8(slot + off, 0);
      const uint32_t nextSlot = (i + 1 < p.count) ? (slot + p.stride) : 0u;   // last-slot next = 0
      c->mem_w32(slot + 0x24u, nextSlot);
      c->mem_w8 (slot + 0x28u, p.klass);
      c->mem_w8 (slot + 0x0Cu, p.meta);              // matches the recomp's redundant write
    }
    c->mem_w32(p.headVar,  p.base);
    c->mem_w8 (p.countVar, (uint8_t)p.count);
  }

  // Aux render-list heads/tails (scratchpad). Each pair is (head, tail) both pointing at the
  // same sentinel node — the empty-list bootstrap the walkers expect.
  c->mem_w32(0x1F80013Cu, 0x800F2410u); c->mem_w32(0x1F800140u, 0x800F2410u);
  c->mem_w16(0x1F800146u, 0);           c->mem_w16(0x1F800144u, 0);
  c->mem_w32(0x1F800148u, 0x800F26C8u); c->mem_w32(0x1F80014Cu, 0x800F26C8u);
  c->mem_w16(0x1F800152u, 0);           c->mem_w16(0x1F800150u, 0);
  c->mem_w32(0x1F800154u, 0x800F2738u); c->mem_w32(0x1F800158u, 0x800F2738u);
  c->mem_w16(0x1F80015Eu, 0);           c->mem_w16(0x1F80015Cu, 0);
}

// 0x8007B18C — top-level object-pool init. Zeroes 520 68-byte slots at 0x800F2740; builds a
// downward-growing free-list of slot pointers at 0x800E7E74; runs eight further sub-inits.
void Pool::init() {
  Core* c = core;
  clearBf548Region();       // native — was call_fn(c, 0x8004FB20)
  initTypedPools();         // native — was call_fn(c, 0x800798F8)

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
    eng(c).placement.spawnWithParent();
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

  mtxOf(c).identity(0x1F8000F8u);   // MR_init(s2)
  mtxOf(c).identity(0x1F800118u);   // MR_init(s2+32)

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

  eng(c).audioDispatch.dispatch3Way(s0, 1);   // native — was rec_dispatch 0x800750D8
  c->mem_w8(0x1F80023Bu, (uint8_t)s0);
  c->mem_w8(0x800BE22Bu, 0);
  c->r[2] = 0x800C0000u;   // incidental v0
}
