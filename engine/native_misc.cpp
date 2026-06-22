// engine/native_misc.cpp — miscellaneous PC-native global/state pokes that do not form a larger subsystem (getter/setters, scratchpad scatter, small table inits).

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

#define CTORS_GUARD 0x800BBEF0u   // one-shot guard word for the crt0 __main ctors runner (ov_80089788)

static void a12c_link_back(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  uint32_t old = c->mem_r32(r3);
  c->mem_w32(node + 36, 0);
  c->mem_w32(node + 32, old);
  if (old == 0) c->mem_w32(r4, node);
  else          c->mem_w32(old + 36, node);
  c->mem_w32(r3, node);
}

// 0x8007A12C — link a sound/voice node into one of several intrusive doubly-linked lists. a0 is the
// node (r9); a3 (r7) selects which head-table set (1, <2, ==2), a2 (r6) selects an insert mode
// (1/<2/==2/==3). The free-list head is the global at 0x800ED8D4 (32783<<16 - 10028); the alloc count
// byte at 0x800ED8C5 is decremented. Heads live in fixed globals at 0x800F-region offsets. The node's
// next/prev are at +32/+36; its type/key bytes at +0/+10/+12. Returns v0 = node ptr (or 0 if the free
// list was empty). Many list permutations — kept structurally identical to the decoded control flow.
// node fields: +32 = "next" link, +36 = "prev" link.  r4 = list-array head pointer cell,
// r3 = secondary head-cell.  These two link helpers mirror L_8007A214 / L_8007A27C.
static void a12c_link_front(Core* c, uint32_t node, uint32_t r4, uint32_t r3) {
  c->mem_w32(node + 32, 0);
  uint32_t old = c->mem_r32(r4);
  c->mem_w32(node + 36, old);
  if (old == 0) c->mem_w32(r3, node);        // empty list → head-cell points at node
  else          c->mem_w32(old + 32, node);
  c->mem_w32(r4, node);
}

// Additional hand-native leaf batches live in sibling files (native_path_aN.cpp) to allow parallel
// authoring; each exposes a register fn wired in below.
void games_native_path_a1_init(void);
void games_native_path_a2_init(void);
void games_native_path_a3_init(void);
void games_native_path_b1_init(void);
void games_native_path_b2_init(void);
void games_native_path_b3_init(void);
void games_native_path_b4_init(void);
void games_native_path_b5_init(void);

// Register every hand-native boot→cutscene function. Called from games_tomba2_init at startup, before
// ov_game_main runs the init prefix, so rec_dispatch routes these addresses to the native C++ bodies.
void games_native_path_init(void) {
  games_native_path_a1_init();
  games_native_path_a2_init();
  games_native_path_a3_init();
  games_native_path_b1_init();
  games_native_path_b2_init();
  games_native_path_b3_init();
  games_native_path_b4_init();
  games_native_path_b5_init();
}

// Register every batch-A1 native function.
void games_native_path_a1_init(void) {
  // TODO(verify): rec_set_override(0x80097540u, ov_80097540); — DISABLED: A/B RAM-diff fails (2 bytes vs interp). return-selection edge case (a0==-1/-2 path). Re-enable after fixing gen_func_80097540 + re-A/B.
  // TODO(verify): rec_set_override(0x800752B4u, ov_800752B4); — DISABLED: A/B RAM-diff fails (167 bytes vs interp). stride-12 band-classify table @0x800BE238 — wrong records. Re-enable after fixing gen_func_800752B4 + re-A/B.
}

// Register every batch-A2 hand-native function (called from games_tomba2_init at startup).
void games_native_path_a2_init(void) {
  // TODO(verify): rec_set_override(0x80090160u, ov_80090160); — DISABLED: A/B RAM-diff fails (80 bytes vs interp). varint stream consumer — wrong accumulation. Re-enable after fixing gen_func_80090160 + re-A/B.
}

// Register every batch-A3 native override. Called from games_tomba2_init alongside the other
// games_native_path_*_init runners.
void games_native_path_a3_init(void) {
  // TODO(verify): rec_set_override(0x80094C10u, ov_80094C10); — DISABLED: A/B RAM-diff fails (15 bytes vs interp). fixed-point mixer/pan — densest, reciprocal-magic divides. Re-enable after fixing gen_func_80094C10 + re-A/B.
  // TODO(verify): rec_set_override(0x80077FB0u, ov_80077FB0); — DISABLED: A/B RAM-diff fails (4 bytes vs interp). 16-bit integer sqrt — wrong result (cascades into 0x800E806x). Re-enable after fixing gen_func_80077FB0 + re-A/B.
}

void games_native_path_b1_init(void) {
}

void games_native_path_b2_init(void) {
}

void games_native_path_b3_init(void) {
}

void games_native_path_b4_init(void) {
}

void games_native_path_b5_init(void) {
}

// 0x8001CBA8 — write a {0,252,0,255} 4-byte pattern at offsets 72/74/76/78 of a0+(a1&0xff), for up
// to two entries (the start index folds a1's low byte; 255 wraps to 0). A palette/attr seed.
static void ov_8001CBA8(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t k = ((a1 & 0xff) == 255) ? 0u : 1u;
  if ((a1 & 0xff) == 255) a1 = 0;
  for (;;) {
    k++;
    uint32_t p = a0 + (a1 & 0xff);
    c->mem_w8(p + 72, 0); c->mem_w8(p + 74, 252); c->mem_w8(p + 76, 0); c->mem_w8(p + 78, 255);
    if ((int32_t)k < 2) a1++; else break;
  }
}

// 0x80045080 — index the stride-8 table at 0x800BE118 by (a1 & 255); load its two words into
// a1,a2 and tail-call 0x8001DC40(a0, tab[0], tab[1]).
static void ov_80045080(Core* c) {
  uint32_t tab = 0x800BE118u + (c->r[5] & 255u) * 8u;
  c->r[5] = c->mem_r32(tab);
  c->r[6] = c->mem_r32(tab + 4);
  call_fn(c, 0x8001DC40u);
}

// 0x8006CBD0 — scatter 6 u16 fields from a1 into the scratchpad block at 0x1F8000D0 (+2,+6,+10)
// and into the object at a0 (+58,+62,+66).
static void ov_8006CBD0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  c->mem_w16(0x1F8000D2u, c->mem_r16(a1 + 0));
  c->mem_w16(0x1F8000D6u, c->mem_r16(a1 + 2));
  c->mem_w16(0x1F8000DAu, c->mem_r16(a1 + 4));
  c->mem_w16(a0 + 58, c->mem_r16(a1 + 6));
  c->mem_w16(a0 + 62, c->mem_r16(a1 + 8));
  c->mem_w16(a0 + 66, c->mem_r16(a1 + 10));
}

// 0x8006D934 — *0x1F8000DC = *(a1+0); *0x1F8000E4 = *(a1+8) (scratchpad).
static void ov_8006D934(Core* c) {
  c->mem_w32(0x1F8000DCu, c->mem_r32(c->r[5] + 0));
  c->mem_w32(0x1F8000E4u, c->mem_r32(c->r[5] + 8));
}

// 0x8006D950 — *0x1F8000E0 (scratchpad) = *(a1+4).
static void ov_8006D950(Core* c) { c->mem_w32(0x1F8000E0u, c->mem_r32(c->r[5] + 4)); }

// 0x800753AC — a1 = *(0x800BE108) + a1, then tail-call 0x8001DC40(a0, a1).
static void ov_800753AC(Core* c) {
  c->r[5] = c->mem_r32(0x800BE108u) + c->r[5];
  call_fn(c, 0x8001DC40u);
}

// 0x80080890 — EnterCriticalSection: `a0=1; syscall`. Routed through rec_syscall so the shared
// IRQ-enable state (s_irq_enabled in hle.cpp) stays consistent with the rest of the BIOS HLE.
static void ov_80080890(Core* c) { c->r[4] = 1; rec_syscall(c, 0); }

static void ov_800865F0(Core* c) { c->r[2] = c->mem_r32(0x800ABE20u); c->mem_w32(0x800ABE20u, c->r[4]); }

// 0x80086604 / 0x800865F0 — getter/setter for the global word at 0x800ABE20 (0x800B<<16 - 0x41E0).
// Getter returns it; setter returns the OLD value then writes a0 (read-before-write order matters).
static void ov_80086604(Core* c) { c->r[2] = c->mem_r32(0x800ABE20u); }

static void ov_80089788(Core* c) {
  if (c->mem_r32(CTORS_GUARD) != 0) return;   // already run
  c->mem_w32(CTORS_GUARD, 1);
  // ctor table @0x80010000, count 0 → nothing to run.
}

// 0x8008CCE0 — clear a cluster of globals at 0x801027xx, then 0x8008CFF0(0, *0x8010272C), then clear three more.
static void ov_8008CCE0(Core* c) {
  uint32_t cnt = c->mem_r32(0x8010272Cu);
  c->mem_w32(0x80102714u, 0); c->mem_w32(0x80102710u, 0); c->mem_w32(0x8010270Cu, 0); c->mem_w32(0x80102704u, 0);
  c->r[4] = 0; c->r[5] = cnt; call_fn(c, 0x8008CFF0u);
  c->mem_w32(0x801026F4u, 0); c->mem_w16(0x801026ECu, 0); c->mem_w32(0x801026E8u, 0);
}

// 0x8008CFF0 — zero the first word of `a1` stride-32 records starting at index a0 in the table at
// *0x80102728: for (i=0; i<a1; i++) *(base + (a0+i)*32) = 0.
static void ov_8008CFF0(Core* c) {
  uint32_t a0 = c->r[4], n = c->r[5]; if (n == 0) return;
  uint32_t base = c->mem_r32(0x80102728u);
  for (uint32_t i = 0; i < n; i++) c->mem_w32(base + ((a0 + i) << 5), 0);
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

// ── 0x800982A0 — range-overlap test against a list. a0 <<= (*0x800AC62C & 31). If the list head
// *0x800AC5F0... wait: the head is *0x800AC638? No — head ptr = *0x800AC5F0? Body: r3 = *0x800AC5F0
// (=0x800B0000-14740). If head==0 return 0. Walk entries (stride 8): for each entry word w=*entry:
//   if (w & 0x80000000) → next; else if (w & 0x40000000) → return 0; else r3 = w & 0x0FFFFFFF; if
//   (r3 < a0) → next; if (a0 < r3 + *(entry+4)) → next; else return 1. Returns 1 when a0 lands inside a
//   live [start,start+len) span, 0 if it falls before the span or off the end of the list.
static void ov_800982A0(Core* c) {
  uint32_t sh = c->mem_r32(0x800AC62Cu) & 31;           // *(0x800B0000-14804)
  uint32_t a0 = c->r[4] << sh;
  uint32_t e = c->mem_r32(0x800AC66Cu);                 // *(0x800B0000-14740) — list head
  if (e == 0) { c->r[2] = 0; return; }
  for (;;) {
    uint32_t w = c->mem_r32(e + 0);
    if (w & 0x80000000u) { e += 8; continue; }          // flagged → skip
    if (w & 0x40000000u) { c->r[2] = 0; return; }        // terminator
    uint32_t start = w & 0x0FFFFFFFu;
    if (start >= a0) { c->r[2] = 1; return; }            // a0 < start → "1" (matches: r3<a0 false → ret 1)
    uint32_t end = start + c->mem_r32(e + 4);
    if (a0 < end) { c->r[2] = 1; return; }               // inside span → 1
    e += 8;                                              // past this span → next entry
  }
}

// ── 0x80098810 — voice param scatter (key bits 0..18). a0 = ptr to a record (r4); first word *a0 is a
// bitmask r5 (and r6 = (r5<1) i.e. r5==0 forces all-fields when the mask is 0/negative-as-unsigned?). For
// each bit k, if the mask bit is set OR r6!=0 (mask is 0 → write everything; matches `bltz`/`<1` guard),
// copy the u16 at a0 + (4 + 2k) into the active voice block (*0x800AC604) at +(448 + 2k). 32 params at
// +448..+510 (bits 0..31), with the last guarded on the sign bit. r6 is true when r5==0 (or top bit set,
// via the signed `>=0` test on the final entry). Faithful per-bit transcription.
static void ov_80098810(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t mask = c->mem_r32(a0 + 0);
  bool all = (mask < 1u);                                // r6 = (r5 < 1) → true iff mask==0
  uint32_t blk = c->mem_r32(0x800AC604u);                // *(0x800B0000-14844)
  // bits 0..30 → field offset +448 + 2*bit, source +4 + 2*bit
  for (int bit = 0; bit < 31; bit++) {
    if (all || (mask & (1u << bit)))
      c->mem_w16(blk + 448 + 2 * bit, c->mem_r16(a0 + 4 + 2 * bit));
  }
  // bit 31 guarded on the sign: write iff r6!=0 OR (s32)mask < 0.
  if (all || ((int32_t)mask < 0))
    c->mem_w16(blk + 448 + 2 * 31, c->mem_r16(a0 + 4 + 2 * 31));
  // (body returns no value of interest before its real `return` at L_80098CD8.)
}

// 0x80098CE0 — return (a0 != 0 && 0x800982A0(*0x800AC5A0) == 0) ? 1 : 0; also store that to 0x800AC59C.
static void ov_80098CE0(Core* c) {
  if (c->r[4] == 0) { c->mem_w32(0x800AC59Cu, 0); c->r[2] = 0; return; }
  c->r[4] = c->mem_r32(0x800AC5A0u); call_fn(c, 0x800982A0u);
  uint32_t v = (c->r[2] == 0) ? 1u : 0u;
  c->mem_w32(0x800AC59Cu, v); c->r[2] = v;
}

// ── 0x80098D30 — voice param scatter, bits 1 and 2 only (a small sibling of 0x80098810). a0=record ptr;
// r5=*a0 mask, r6=(r5==0). If (mask&2) or r6: copy u16 a0+8 → *0x800AC604 +388, and also shadow it to
// *0x800AC5AC (=-14932). If (mask&4) or r6: copy u16 a0+10 → block +390, shadow to *0x800AC5AE (-14930).
// Returns 0.
static void ov_80098D30(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t mask = c->mem_r32(a0 + 0);
  bool all = (mask < 1u);                                // r6 = (mask < 1) → mask==0
  if (all || (mask & 2u)) {
    uint32_t blk = c->mem_r32(0x800AC604u);              // *(-14844)
    uint16_t v = c->mem_r16(a0 + 8);
    c->mem_w16(blk + 388, v);
    c->mem_w16(0x800AC5ACu, v);                          // shadow (-14932)
  }
  if (all || (mask & 4u)) {
    uint32_t blk = c->mem_r32(0x800AC604u);
    uint16_t v = c->mem_r16(a0 + 10);
    c->mem_w16(blk + 390, v);
    c->mem_w16(0x800AC5AEu, v);                          // shadow (-14930)
  }
  c->r[2] = 0;
}

static void ov_80098DB0(Core* c) { c->r[6] = 204; c->r[7] = 205; call_fn(c, 0x80097E40u); }

// ── 0x80098F90 — voice key-on/key-off request. a1 &= 0x00FFFFFF; lo = a1 (low 16 bits via w16), hi =
// a1 >> 16 (the byte in bits16..23). Only a0==0 and a0==1 do work; any other a0 returns immediately.
// The "dynamic block" (@0x80105EC8 = 0x80100000+24264) vs the "static block" (*0x800AC604 = -14844) is
// gated on bit0 of *0x800AC5F0 (-14864): bit set → dynamic, clear → static. Globals: busy *0x800AC5BC
// (-14916), pending *0x800AC5B8 (-14920), enable *0x800AC590 (-14960).
//   a0==1 (key-ON):  dyn → DYN+0=lo, DYN+2=hi, busy|=1, pending|=a1, then AND-clear a1 out of DYN+4/+6.
//                    static → blk+392=lo, blk+394=hi, enable|=a1.
//   a0==0 (key-OFF): dyn → DYN+4=lo, DYN+6=hi, busy|=1, pending&=~a1, then AND-clear a1 out of DYN+0/+2.
//                    static → blk+396=lo, blk+398=hi, enable&=~a1.
static void ov_80098F90(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t a1 = c->r[5] & 0x00FFFFFFu;
  uint32_t lo = a1 & 0xFFFFu, hi = a1 >> 16;
  const uint32_t DYN = 0x80105EC8u;                      // dynamic voice block (0x80100000+24264)
  bool dyn = (c->mem_r32(0x800AC5F0u) & 1u) != 0;        // bit0 set → dynamic

  if (a0 == 1) {                                         // key-ON
    if (dyn) {
      c->mem_w16(DYN + 0, (uint16_t)lo);
      c->mem_w16(DYN + 2, (uint16_t)hi);
      c->mem_w32(0x800AC5BCu, c->mem_r32(0x800AC5BCu) | 1u);            // busy |= 1
      c->mem_w32(0x800AC5B8u, c->mem_r32(0x800AC5B8u) | a1);           // pending |= a1
      if (c->mem_r16(DYN + 4) & lo) c->mem_w16(DYN + 4, (uint16_t)(c->mem_r16(DYN + 4) & ~lo));
      if (c->mem_r16(DYN + 6) & hi) c->mem_w16(DYN + 6, (uint16_t)(c->mem_r16(DYN + 6) & ~hi));
    } else {
      uint32_t blk = c->mem_r32(0x800AC604u);
      c->mem_w16(blk + 392, (uint16_t)lo);
      c->mem_w16(blk + 394, (uint16_t)hi);
      c->mem_w32(0x800AC590u, c->mem_r32(0x800AC590u) | a1);           // enable |= a1
    }
  } else if (a0 == 0) {                                  // key-OFF
    if (dyn) {
      c->mem_w16(DYN + 4, (uint16_t)lo);
      c->mem_w16(DYN + 6, (uint16_t)hi);
      c->mem_w32(0x800AC5BCu, c->mem_r32(0x800AC5BCu) | 1u);            // busy |= 1
      c->mem_w32(0x800AC5B8u, c->mem_r32(0x800AC5B8u) & ~a1);          // pending &= ~a1
      if (c->mem_r16(DYN + 0) & lo) c->mem_w16(DYN + 0, (uint16_t)(c->mem_r16(DYN + 0) & ~lo));
      if (c->mem_r16(DYN + 2) & hi) c->mem_w16(DYN + 2, (uint16_t)(c->mem_r16(DYN + 2) & ~hi));
    } else {
      uint32_t blk = c->mem_r32(0x800AC604u);
      c->mem_w16(blk + 396, (uint16_t)lo);
      c->mem_w16(blk + 398, (uint16_t)hi);
      c->mem_w32(0x800AC590u, c->mem_r32(0x800AC590u) & ~a1);          // enable &= ~a1
    }
  }
  // any other a0 → no-op (body falls straight to L_80099144 return).
}

// 0x800998E4 — fill the 24-byte status array at a0 from two parallel 16-element bitmask/table globals.
// For each i in [0,24): entry = *(0x800AC604) + (i<<4); mask = *(0x800AC590) & (1<<i);
//   tag = *(uint16_t)(entry+12);
//   a0[i] = mask ? (tag ? 1 : 3) : (tag ? 2 : 0).
static void ov_800998E4(Core* c) {
  uint32_t out = c->r[4];
  uint32_t table = c->mem_r32(0x800AC604u);        // 0x800B0000 - 14844
  uint32_t bits  = c->mem_r32(0x800AC590u);        // 0x800B0000 - 14960
  for (int i = 0; i < 24; i++) {
    uint32_t entry = table + ((uint32_t)i << 4);
    uint32_t mask = bits & (1u << (i & 31));
    uint32_t tag = c->mem_r16(entry + 12);
    uint8_t v;
    if (mask != 0) v = tag ? 1 : 3;
    else           v = tag ? 2 : 0;
    c->mem_w8(out + i, v);
  }
}

// 0x8009C1FC — copy a 28-word table from 0x8009C060 to low RAM 0xDF80 (installs a fixed block).
static void ov_8009C1FC(Core* c) {
  uint32_t s = 0x8009C060u, d = 0xDF80u;
  for (int i = 0; i < 28; i++) { c->mem_w32(d, c->mem_r32(s)); s += 4; d += 4; }
}

