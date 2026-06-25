// game/world/pool.cpp — PC-native object-pool / control-block init (the boot-time zero/seed cluster) and scheduler-frame helpers.

#include "core.h"
#include "cfg.h"
#include "pool.h"
#include "verify_gate.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// Intrusive doubly-linked-list insert helpers used by ov_8007A12C (node+32=next, +36=prev;
// r4=tail-cell, r3=head-cell). Front = push to head, back = push to tail.
static void a12c_link_front(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  c->mem_w32(node + 32, 0);
  uint32_t old = c->mem_r32(r4);
  c->mem_w32(node + 36, old);
  if (old == 0) c->mem_w32(r3, node);        // empty list → head-cell points at node
  else          c->mem_w32(old + 32, node);
  c->mem_w32(r4, node);
}
static void a12c_link_back(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  uint32_t old = c->mem_r32(r3);
  c->mem_w32(node + 36, 0);
  c->mem_w32(node + 32, old);
  if (old == 0) c->mem_w32(r4, node);
  else          c->mem_w32(old + 36, node);
  c->mem_w32(r3, node);
}

static void seed_array(Core* c, uint32_t base, uint32_t stride, int count, uint32_t tag) {
  for (int k = 0; k < count; k++) {
    uint32_t e = base + (uint32_t)k * stride;
    c->r[4]=e; c->r[5]=0; c->r[6]=stride; call_fn(c, 0x8009A420u);   // memset(e, 0, stride)
    c->mem_w32(e + 36, base + (uint32_t)(k + 1) * stride);          // next ptr
    c->mem_w8 (e + 40, (uint8_t)tag);
    c->mem_w8 (e + 12, 0);
  }
  c->mem_w32(base + (uint32_t)(count - 1) * stride + 36, 0);        // last entry next = 0
}

// 0x8004FB20 — memset(0x800BF548, 0, 700) via 0x8009A420.
static void ov_8004FB20(Core* c) { c->r[4] = 0x800BF548u; c->r[5] = 0; c->r[6] = 700; call_fn(c, 0x8009A420u); }

// 0x80051794 — init an 8-word block at a0 to {0x1000,0,0x1000,0,0x1000,0,0,0} (three 0x1000 stride
// fields with zeroed counters — a scratch/free-list header).
static void ov_80051794(Core* c) {
  uint32_t p = c->r[4];
  c->mem_w32(p + 0, 0x1000); c->mem_w32(p + 4, 0);  c->mem_w32(p + 8, 0x1000); c->mem_w32(p + 12, 0);
  c->mem_w32(p + 16, 0x1000); c->mem_w32(p + 20, 0); c->mem_w32(p + 24, 0);    c->mem_w32(p + 28, 0);
}

// 0x80051F80 — fetch the descriptor ptr at scratch+312, write a0 to +2 and 1 to +0, then
// call 0x80080880(0xFF000000).
static void ov_80051F80(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t p = c->mem_r32(0x1F800138u);   // scratchpad + 312
  c->mem_w16(p + 2, (uint16_t)a0);
  c->mem_w16(p + 0, 1);
  c->r[4] = 0xFF000000u;
  call_fn(c, 0x80080880u);
}

// ── 0x800752B4 — classify 24 entries into a priority byte at +8 of each stride-12 record in the table
// at 0x800C0000-7624 (=0x800B8C58... no: 0x800C0000-7624). For i in 0..23: pick a band by comparing the
// loop index i against thresholds derived from a0 (24-a0, 16-a0, 12-a0, 8-a0): i<24-a0 → 4; i<16-a0 → 1;
// i<12-a0 → 3; i<8-a0 → 2; else → 0. Store that byte to record[i].byte8. (Record addr = i*12 + table.)
static void ov_800752B4(Core* c) {
  int32_t a0 = (int32_t)c->r[4];
  uint32_t table = 0x800BE238u;                        // 0x800C0000 - 7624
  int32_t t24 = 24 - a0, t16 = 16 - a0, t12 = 12 - a0, t8 = 8 - a0;
  for (int32_t i = 0; i < 24; i++) {
    uint32_t rec = table + (uint32_t)(i * 12);
    uint8_t band;
    if (i < t24) band = 4;
    else if (i < t16) band = 1;
    else if (i < t12) band = 3;
    else if (i < t8)  band = 2;
    else              band = 0;
    c->mem_w8(rec + 8, band);
  }
}

// 0x80075E04 — register an entry in a 24-slot, stride-12 priority table and (if a slot was chosen)
// fill its 8 bytes. The table base is 0x800BE238; the live start index is the global at 0x800BED78
// (32780<<16 - 4744). For each index i in [start,24): the candidate row is base+i*12, and a parallel
// row base+i*12+1 is examined at +7 (key byte). The search keeps the best-priority slot (r11) using
// a0=key match plus the (a1-low / 254) comparisons on the row's byte[0]. When a slot is found it is
// populated from a1..a3 and four stack args (orig caller frame), and v0 = (slotword & ~0xFF) | savedR10.
// Uses an 8-byte stack scratch (sp-8): word[0] holds the captured r10 written back into v0's low byte.
static void ov_80075E04(Core* c) {
  uint32_t sp = c->r[29] - 8;                       // local frame (no real prologue; just scratch)
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
  int32_t r12 = (int32_t)a1;                          // running "best byte[0]" lower bound
  int32_t r13 = 254;                                  // running "best byte[0]" upper bound
  int32_t i = (int32_t)c->mem_r32(0x800BED78u);       // start index
  uint32_t row = 0x800BE238u + (uint32_t)i * 12u;     // &table[i]
  uint32_t chosen = 0;                                // r11 (0 = none yet)
  uint32_t sp0 = 0;                                   // sp+0 scratch (captured r10 for v0 low byte)

  if (i < 24) {
    uint32_t key = row + 1;                            // r9 = row + 1 (key bytes live at +7 of row+1)
    for (; i < 24; i++, key += 12, row += 12) {
      if (a0 != c->mem_r8(key + 7)) continue;          // key mismatch → skip
      uint32_t b = c->mem_r8(key + 0);                 // candidate byte[0]
      if (!((int32_t)b < r12)) {                       // b >= r12: tighten only if it also passes
        if (b != (uint32_t)r12) continue;              // strictly greater (and != lower) → skip
        uint32_t rb = c->mem_r8(row + 0);
        if (r13 < (int32_t)rb) continue;               // row byte[0] exceeds current upper → skip
      }
      // accept this slot as the new best
      chosen = row;
      sp0 = (uint32_t)i;                                // save current loop index r10 to scratch
      r12 = (int32_t)c->mem_r8(key + 0);
      r13 = (int32_t)c->mem_r8(row + 0);
    }
  }

  if (chosen == 0) { c->r[2] = 0; c->r[29] = sp + 8; return; }
  // populate the chosen slot
  c->mem_w8(chosen + 2, (uint8_t)a2);
  c->mem_w8(chosen + 3, (uint8_t)a3);
  c->mem_w8(chosen + 4, (uint8_t)c->mem_r32(sp + 24));   // stack arg @ caller sp+16
  c->mem_w8(chosen + 5, (uint8_t)c->mem_r32(sp + 28));   //                    +20
  c->mem_w8(chosen + 6, (uint8_t)c->mem_r32(sp + 32));   //                    +24
  uint32_t arg = c->mem_r32(sp + 36);                    //                    +28
  c->mem_w8(chosen + 1, (uint8_t)a1);
  c->mem_w8(chosen + 7, (uint8_t)arg);
  c->mem_w8(chosen + 0, (uint8_t)255);
  uint32_t word = c->mem_r32(chosen + 0);
  c->r[2] = (word & 0xFFFFFF00u) | sp0;                  // r4 = -256 = ~0xFF; OR the saved index (r3)
  c->r[29] = sp + 8;
}

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
  c->r[4]=a4; c->r[5]=a5; call_fn(c, 0x800782F0u);
  call_fn(c, 0x800508A8u);
  uint32_t v = c->mem_r8(S + 566);
  uint32_t arg = (v == 0u || (uint32_t)(v - 7u) < 2u) ? 0u : 255u;
  c->r[4]=arg; c->r[5]=arg; c->r[6]=arg; call_fn(c, 0x8005082Cu);
  c->mem_w8(0x800BF9D4u, 0);
  c->r[2] = 0x800C0000u;   // incidental v0: recomp epilogue `lui v0,0x800c; sb zero,-1580(v0)` leaves the store base
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

static void ov_800798F8(Core* c) {
  c->mem_w32(0x800FB168u, 0);
  c->mem_w32(0x800F23A8u, 0);
  c->mem_w32(0x800F2624u, 0);
  c->mem_w32(0x800F239Cu, 0);

  seed_array(c, 0x800ED8D8u, 136, 52, 0);
  c->mem_w32(0x800E8098u, 0x800ED8D8u); c->mem_w8(0x800E7E7Cu, 52);
  seed_array(c, 0x800EF478u, 196, 58, 1);
  c->mem_w32(0x800E80A0u, 0x800EF478u); c->mem_w8(0x800E7E7Du, 58);
  seed_array(c, 0x800FE198u, 208, 42, 2);
  c->mem_w32(0x800F2398u, 0x800FE198u); c->mem_w8(0x800ED8CCu, 42);
  seed_array(c, 0x800FB858u, 264, 40, 3);
  c->mem_w32(0x800ED8D4u, 0x800FB858u); c->mem_w8(0x800ED8C5u, 40);
  seed_array(c, 0x800FB218u, 320,  5, 4);
  c->mem_w32(0x800ED8D0u, 0x800FB218u); c->mem_w8(0x800ED8C4u, 5);

  const uint32_t S = 0x1F800000u;
  c->mem_w32(S + 316, 0x800F2410u); c->mem_w32(S + 320, 0x800F2410u); c->mem_w16(S + 326, 0);
  c->mem_w32(S + 328, 0x800F26C8u); c->mem_w32(S + 332, 0x800F26C8u);
  c->mem_w16(S + 338, 0); c->mem_w16(S + 336, 0);
  c->mem_w32(S + 340, 0x800F2738u); c->mem_w32(S + 344, 0x800F2738u);
  c->mem_w16(S + 350, 0); c->mem_w16(S + 348, 0);
}

static void ov_8007A12C(Core* c) {
  uint32_t r9 = c->r[4];                     // a0: source/reference node
  uint32_t a2 = c->r[6], a3 = c->r[7];       // r6 = insert mode, r7 = list-select
  uint32_t r8 = c->mem_r32(0x800ED8D4u);     // free-list head (32783<<16 - 10028)
  if (r8 == 0) { c->r[2] = 0; return; }      // nothing free → return 0

  // pop the free node: free-head = freenode->prev(+36), dec the count byte at -10043
  uint8_t cnt = (uint8_t)c->mem_r8(0x800ED8C5u);
  uint32_t newHead = c->mem_r32(r8 + 36);
  c->mem_w8(0x800ED8C5u, (uint8_t)(cnt - 1));
  c->mem_w32(0x800ED8D4u, newHead);

  // choose the head-pointer pair (r4 = list array, r3 = secondary head-cell) by a3.
  // a3==1 → set1; a3==2 → set2; everything else (0 or >2) → set0.
  uint32_t r4, r3;
  if (a3 == 1u) {
    r4 = 0x800F2624u;                        // 0x800F0000 + 9764
    r3 = 0x800F239Cu;                        // 0x800F0000 + 9116
  } else if (a3 == 2u) {
    r4 = 0x800F2738u;                        // 0x800F0000 + 10040
    r3 = 0x800F23A0u;                        // 0x800F0000 + 9120
  } else {                                   // a3 == 0 (and any a3 > 2)
    r4 = 0x800FB168u;                        // 0x80100000 - 20120
    r3 = 0x800F23A8u;                        // 0x800F0000 + 9128
  }

  // insert mode by a2 (decoded: a2==0 → front-link; a2==1 → after r9->next; a2==2 → back-link;
  // a2==3 → after r9->prev; any other a2 → no linking, just stamp the bytes).
  if (a2 == 0u) {
    a12c_link_front(c, r8, r4, r3);
  } else if (a2 == 1u) {
    uint32_t nxt = c->mem_r32(r9 + 32);
    if (nxt == 0) {                           // no successor → fall back to front-link (L_8007A214)
      a12c_link_front(c, r8, r4, r3);
    } else {                                  // splice r8 between r9 and r9->next
      c->mem_w32(r8 + 32, nxt);
      c->mem_w32(r8 + 36, r9);
      c->mem_w32(c->mem_r32(r9 + 32) + 36, r8);
      c->mem_w32(r9 + 32, r8);
    }
  } else if (a2 == 2u) {
    a12c_link_back(c, r8, r4, r3);
  } else if (a2 == 3u) {
    uint32_t prv = c->mem_r32(r9 + 36);
    if (prv == 0) {                           // no predecessor → fall back to back-link (L_8007A27C)
      a12c_link_back(c, r8, r4, r3);
    } else {                                  // splice r8 between r9->prev and r9
      c->mem_w32(r8 + 32, r9);
      c->mem_w32(r8 + 36, prv);
      c->mem_w32(c->mem_r32(r9 + 36) + 32, r8);
      c->mem_w32(r9 + 36, r8);
    }
  }
  // else: a2 not in {0,1,2,3} → no list mutation (matches the L_8007A2B0 fall-through).

  // stamp the node header bytes and return the node pointer
  c->mem_w8(r8 + 10, (uint8_t)a3);
  c->mem_w8(r8 + 0,  (uint8_t)2u);            // type byte = 2 on every path that reaches the stamp
  c->mem_w8(r8 + 12, (uint8_t)c->r[5]);       // a1 → key byte
  c->r[2] = r8;
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

// 0x8007AC14/AC40/AC6C/AC98/AD14 — memset(<global>, 0, <n>) via 0x8009A420. Bases from N<<16+signed-off:
//   AC14: 0x800F0000-32760 = 0x800E8008, n=144   AC40: 0x800F0000+9240 = 0x800F2418, n=524
//   AC6C: 0x80100000+1632  = 0x80100660, n=48    AC98: 0x800F0000-12264 = 0x800ED018, n=60
//   AD14: 0x800F0000-12200 = 0x800ED058, n=64
static void ov_8007AC14(Core* c) { c->r[4] = 0x800E8008u; c->r[5] = 0; c->r[6] = 144; call_fn(c, 0x8009A420u); }

static void ov_8007AC40(Core* c) { c->r[4] = 0x800F2418u; c->r[5] = 0; c->r[6] = 524; call_fn(c, 0x8009A420u); }

static void ov_8007AC6C(Core* c) { c->r[4] = 0x80100660u; c->r[5] = 0; c->r[6] = 48;  call_fn(c, 0x8009A420u); }

static void ov_8007AC98(Core* c) { c->r[4] = 0x800ED018u; c->r[5] = 0; c->r[6] = 60;  call_fn(c, 0x8009A420u); }

// 0x8007ACC4 — 8× memset(0x80100400 + i*76, 0, 76) via 0x8009A420 (eight stride-76 records).
static void ov_8007ACC4(Core* c) {
  for (uint32_t i = 0; i < 8; i++) { c->r[4] = 0x80100400u + i * 76; c->r[5] = 0; c->r[6] = 76; call_fn(c, 0x8009A420u); }
}

static void ov_8007AD14(Core* c) { c->r[4] = 0x800ED058u; c->r[5] = 0; c->r[6] = 64;  call_fn(c, 0x8009A420u); }

// 0x8007AD40 — memset(0x800EC188, 0, 2560) via 0x8009A420, then for 40 stride-64 records set
// byte[+7] = (i & 7) and u16[+12] = 4096.
static void ov_8007AD40(Core* c) {
  uint32_t base = 0x800EC188u;
  c->r[4] = base; c->r[5] = 0; c->r[6] = 2560; call_fn(c, 0x8009A420u);
  for (uint32_t i = 0; i < 40; i++) { uint32_t p = base + i * 64; c->mem_w8(p + 7, (uint8_t)(i & 7)); c->mem_w16(p + 12, 4096); }
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
    call_fn(c, 0x80072DDCu);
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
void ov_pool_init_run(Core* c) {                          // 0x8007B18C — object-pool init
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("poolinitverify") ? 1 : 0;
  if (!s_v) { ov_8007B18C(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_8007B18C, 0x8007B18Cu, "poolinitverify", &ng, &nb);
}
void ov_796dc_run(Core* c) {                              // 0x800796DC — control-block reset + sub-inits
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init796dcverify") ? 1 : 0;
  if (!s_v) { ov_800796DC(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800796DC, 0x800796DCu, "init796dcverify", &ng, &nb);
}
void ov_263e8_run(Core* c) {                              // 0x800263E8 — area object-record seeding
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init263e8verify") ? 1 : 0;
  if (!s_v) { ov_800263E8(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800263E8, 0x800263E8u, "init263e8verify", &ng, &nb);
}
void ov_75240_run(Core* c) {                              // 0x80075240 — clamp/control-block reset
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init75240verify") ? 1 : 0;
  if (!s_v) { ov_80075240(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_80075240, 0x80075240u, "init75240verify", &ng, &nb);
}
void ov_783dc_run(Core* c) {                              // 0x800783DC — per-area view/scroll setup
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("init783dcverify") ? 1 : 0;
  if (!s_v) { ov_800783DC(c); return; }
  static long ng = 0, nb = 0; world_init_gate(c, ov_800783DC, 0x800783DCu, "init783dcverify", &ng, &nb);
}

// 0x8007B2C0 — load a 4-entry u16 weight ramp into the scratchpad at 0x1F800170: a0==0 → descending
// {0x8000,0x4000,0x2000,0x1000}; a0!=0 → ascending {0x1000,0x2000,0x4000,0x8000}.
static void ov_8007B2C0(Core* c) {
  uint32_t b = 0x1F800170u;
  if (c->r[4] == 0) { c->mem_w16(b+0,0x8000); c->mem_w16(b+2,0x4000); c->mem_w16(b+4,0x2000); c->mem_w16(b+6,0x1000); }
  else              { c->mem_w16(b+0,0x1000); c->mem_w16(b+2,0x2000); c->mem_w16(b+4,0x4000); c->mem_w16(b+6,0x8000); }
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

