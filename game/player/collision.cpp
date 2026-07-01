// engine/collision.cpp — PC-native COLLISION-GRID subsystem.
// The collision-grid family that resolves an object's position against the level's spatial grid:
// the list-tail resolver (FUN_80031780), the grid row-pointer setup (FUN_80049968), the cell query /
// neighbor-walk (FUN_80047CBC), the resolve loop (FUN_800498C8), and the per-step origin/index setup
// (FUN_8004798C). Pure control flow over scratchpad + object/grid memory — NO GTE, NO render packets.
// Extracted verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for
// PC-game code structure. Diagnostic A/B gates (listscan/gridsetup/gridquery/gridresolve/gridstep)
// are REPL channels, unchanged. The dispatched grid callees stay reachable by address (rec_dispatch).
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// FUN_80031780 — list-tail resolver / reset. Walks the 8-byte-stride linked list rooted at
// a0[52] (off 0x34), reading the tag word at entry+4 each step, until a tag has bit30|bit31
// (0xC0000000) set. If that terminator tag has bit30 (0x40000000) set -> clear the list
// (a0[52]=a0[56]=0); else set the tail pointer a0[56] (off 0x38)=found entry. If a0[52]==0 at
// entry it is a no-op. Pure guest-pointer/integer walk, no GP0/OT. `listscan` (lazy gate) A/B's
// the two written words.
void ov_list_scan_31780(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("listscan") ? 1 : 0;
  uint32_t a0 = c->r[4];
  uint32_t o52 = c->mem_r32(a0 + 52), o56 = c->mem_r32(a0 + 56);
  uint32_t n52 = o52, n56 = o56, v0 = c->r[2];
  if (o52 != 0) {
    uint32_t v1 = o52, a1;
    for (;;) { a1 = c->mem_r32(v1 + 4); bool brk = (a1 & 0xC0000000u) != 0; v1 += 8; if (brk) break; }  // +8 is the loop's delay slot — runs even on exit
    v0 = a1 & 0x40000000u;
    if (v0) { n56 = 0; n52 = 0; } else { n56 = v1; }
  }
  if (s_v) {
    rec_super_call(c, 0x80031780u);                  // memory untouched above -> oracle writes
    uint32_t r52 = c->mem_r32(a0 + 52), r56 = c->mem_r32(a0 + 56);
    static long ng = 0, nb = 0;
    if (r52 != n52 || r56 != n56) { if (nb++ < 20) fprintf(stderr, "[listscan] MISMATCH a0=%x 52 mine=%x oracle=%x  56 mine=%x oracle=%x\n", a0, n52, r52, n56, r56); }
    else if (++ng % 5000 == 0) fprintf(stderr, "[listscan] %ld matches\n", ng);
    return;                                          // keep oracle result
  }
  c->mem_w32(a0 + 52, n52); c->mem_w32(a0 + 56, n56); c->r[2] = v0;
}

// FUN_80049968 — collision-grid ROW-POINTER setup. a0 = grid/layer index (&0xff). Reads the table
// base ptr @0x1F8001C8, indexes table[a0] (halfword offset) to a per-grid record, then writes 5
// scratchpad row pointers from the record's halfword fields:
//   0x1F8001CC = rec+0x14;  0x1F8001D0/D4/D8/DC = rec + rec[12/14/16/18]*2
// Pure pointer arithmetic over scratchpad + guest record data. `gridsetup` A/B's the 5 written words.
void ov_grid_setup_49968(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridsetup") ? 1 : 0;
  uint32_t a0   = c->r[4] & 0xffu;
  uint32_t base = c->mem_r32(0x1F8001C8u);
  uint32_t rec  = base + (uint32_t)c->mem_r16(base + a0 * 2) * 2;
  uint32_t cc = rec + 20;
  uint32_t d0 = rec + (uint32_t)c->mem_r16(rec + 12) * 2;
  uint32_t d4 = rec + (uint32_t)c->mem_r16(rec + 14) * 2;
  uint32_t d8 = rec + (uint32_t)c->mem_r16(rec + 16) * 2;
  uint32_t dc = rec + (uint32_t)c->mem_r16(rec + 18) * 2;
  if (s_v) {
    rec_super_call(c, 0x80049968u);
    static long ng = 0, nb = 0;
    uint32_t o_cc = c->mem_r32(0x1F8001CCu), o_d0 = c->mem_r32(0x1F8001D0u), o_d4 = c->mem_r32(0x1F8001D4u),
             o_d8 = c->mem_r32(0x1F8001D8u), o_dc = c->mem_r32(0x1F8001DCu);
    if (o_cc != cc || o_d0 != d0 || o_d4 != d4 || o_d8 != d8 || o_dc != dc) {
      if (nb++ < 20) fprintf(stderr, "[gridsetup] MISMATCH a0=%x cc=%x/%x d0=%x/%x d4=%x/%x d8=%x/%x dc=%x/%x\n",
                             a0, cc, o_cc, d0, o_d0, d4, o_d4, d8, o_d8, dc, o_dc);
    } else if (++ng % 5000 == 0) fprintf(stderr, "[gridsetup] %ld matches\n", ng);
    return;
  }
  c->mem_w32(0x1F8001CCu, cc); c->mem_w32(0x1F8001D0u, d0); c->mem_w32(0x1F8001D4u, d4);
  c->mem_w32(0x1F8001D8u, d8); c->mem_w32(0x1F8001DCu, dc);
}

// FUN_80047CBC — collision-grid CELL QUERY / neighbor-walk. Converts the probe position
// (sh[0x1BC],sh[0x1C0]) relative to grid origin (sh[0x1AA],sh[0x1AC]) into grid indices (>>6),
// bounds-checks against the row table (w[0x1CC]), looks up the cell record (w[0x1D0] + idx*8) and
// reads its tag. Then loops following the tag bits: 0x8000=keep walking, 0x4000=follow the cell's
// link/child list (inner sub-scan against u16[0x1BE]-32), else step ONE cell in +/-X (sh[0x1C0]) or
// +/-Z (sh[0x1BC]) per the low 3 tag bits, recompute the cell, repeat. Returns 0 (off-grid/blocked)
// or 1 (resolved). Writes scratchpad ONLY (0x08C idx, 0x1A8 tag, 0x1BC/0x1C0 stepped coords,
// 0x1E0/E4 cursor ptrs). t6=w[0x1D4], t7=u16[0x1BE], MASK=~63 (the -64 grid-snap mask).
static uint32_t grid_query_47cbc(Core* c) {
  const uint32_t SP = 0x1F800000u, MASK = 0xFFFFFFC0u;
  // ---- phase A: initial cell from probe vs origin ----
  int32_t t1 = ((int32_t)(int16_t)c->mem_r16(SP+0x1BC) - (int32_t)(int16_t)c->mem_r16(SP+0x1AA)) >> 6;  // grid Z idx (a3/t1)
  int32_t a3 = t1;
  uint32_t row0 = c->mem_r32(SP+0x1CC);
  uint32_t a1   = row0 + (uint32_t)(t1 << 2);                    // &row0[t1] (4-byte stride)
  int32_t t0   = ((int32_t)(int16_t)c->mem_r16(SP+0x1C0) - (int32_t)(int16_t)c->mem_r16(SP+0x1AC)) >> 6;  // grid X idx (t0)
  uint32_t A1_0 = c->mem_r16(a1+0);
  if (t0 < (int32_t)A1_0) return 0;
  uint32_t a2 = (c->mem_r16(a1+2) + (uint32_t)t0) - A1_0;
  int32_t limit = (int32_t)((uint32_t)c->mem_r16(SP+0x1AE) >> 6) - 2;
  if (a3 < limit) { if (!((a2 & 0xffff) < (uint32_t)c->mem_r16(a1+6))) return 0; }
  // ---- L_d64: latch the cell record + tag ----
  uint32_t idx = a2 & 0xffff;
  c->mem_w32(SP+0x08C, idx);
  uint32_t ptr = c->mem_r32(SP+0x1D0) + (idx << 3);
  a2 = c->mem_r16(ptr+0);
  c->mem_w32(SP+0x1E4, ptr);
  c->mem_w32(SP+0x1E0, ptr);
  c->mem_w16(SP+0x1A8, (uint16_t)a2);
  if ((a2 & 0xc000u) != 0xc000u) c->mem_w16(SP+0x1A8, 0);
  if ((a2 & 0x8000u) == 0) return 1;
  uint32_t t6 = c->mem_r32(SP+0x1D4);
  uint32_t t7 = c->mem_r16(SP+0x1BE);
  // ---- walk ----
  for (;;) {
    if (a2 & 0x4000u) {
      // ARM A: follow link / child list
      uint32_t rec = c->mem_r32(SP+0x1E0);                       // original record (a1)
      c->mem_w32(SP+0x1E0, t6 + ((uint32_t)c->mem_r16(rec+2) << 3));
      if (a2 & 0x0001u) {
        int32_t a0 = 1;
        uint32_t cnt = c->mem_r16(rec+4);
        if (1 < (int32_t)cnt) {
          int32_t a3p = (int32_t)t7 - 32;
          for (;;) {
            uint32_t cur = c->mem_r32(SP+0x1E0) + 8;
            uint32_t iv = c->mem_r16(cur+4);
            c->mem_w32(SP+0x1E0, cur);
            uint32_t iw = c->mem_r16(cur+6);
            if (((iv - (uint32_t)a3p) & 0xffff) < iw) break;
            a0 += 1;
            if (!(a0 < (int32_t)cnt)) break;
          }
        }
        uint32_t t = c->mem_r16(rec+6);
        bool hit = ((uint32_t)a0 == (t & 0xff)) || ((uint32_t)a0 == (t >> 8));
        if (!hit && (uint32_t)a0 == (uint32_t)c->mem_r16(rec+4)) hit = true;
        if (hit) c->mem_w32(SP+0x1E0, t6 + ((uint32_t)c->mem_r16(rec+2) << 3));
      }
      a2 = c->mem_r16(c->mem_r32(SP+0x1E0) + 0);
    } else {
      // ARM B: step one grid cell, recompute
      if (a2 & 0x0004u) {
        switch (a2 & 3u) {
          case 1: c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) + 64) & MASK)); t1++; break;
          case 0: c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) & MASK) - 1));  t1--; break;
          case 2: c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) & MASK) - 1));  t0--; break;
          case 3: c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) + 64) & MASK)); t0++; break;
        }
      } else if ((uint32_t)c->mem_r16(SP+0x1AE) < (uint32_t)c->mem_r16(SP+0x1B0)) {
        if (a2 & 0x0002u) { c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) + 64) & MASK)); t1++; }
        else              { c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) & MASK) - 1));  t1--; }
      } else {
        if (a2 & 0x0001u) { c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) + 64) & MASK)); t0++; }
        else              { c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) & MASK) - 1));  t0--; }
      }
      // L_f9c: recompute cell from stepped indices
      uint32_t a1b = c->mem_r32(SP+0x1CC) + (uint32_t)(((int32_t)(int16_t)t1) * 4);
      uint32_t A1b0 = c->mem_r16(a1b+0);
      if (A1b0 == 0xffff) return 0;
      if ((int32_t)(int16_t)t0 < (int32_t)A1b0) return 0;
      uint32_t a2v = (c->mem_r16(a1b+2) + (uint32_t)t0) - A1b0;
      uint32_t a0b = a2v & 0xffff;
      if (!(a0b < (uint32_t)c->mem_r16(a1b+6))) return 0;
      uint32_t ptrB = c->mem_r32(SP+0x1D0) + (a0b << 3);
      a2 = c->mem_r16(ptrB+0);
      c->mem_w32(SP+0x08C, a0b);
      c->mem_w32(SP+0x1E4, ptrB);
      c->mem_w32(SP+0x1E0, ptrB);
      c->mem_w16(SP+0x1A8, (uint16_t)a2);
    }
    if ((a2 & 0x8000u) == 0) return 1;
  }
}

void ov_grid_query_47cbc(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridquery") ? 1 : 0;
  if (!s_v) { c->r[2] = grid_query_47cbc(c); return; }
  const uint32_t LO = 0x1F800080u, HI = 0x1F8001F0u, N = HI - LO;
  uint8_t snap[0x170], after[0x170];
  for (uint32_t a = LO; a < HI; a++) snap[a-LO] = c->mem_r8(a);
  uint32_t mine = grid_query_47cbc(c);
  for (uint32_t a = LO; a < HI; a++) { after[a-LO] = c->mem_r8(a); c->mem_w8(a, snap[a-LO]); }  // capture+restore
  rec_super_call(c, 0x80047CBCu);
  uint32_t oracle = c->r[2];
  int firstoff = -1;
  for (uint32_t a = LO; a < HI; a++) if (c->mem_r8(a) != after[a-LO]) { firstoff = (int)(a-LO); break; }
  static long ng = 0, nb = 0;
  if (firstoff >= 0 || mine != oracle) {
    if (nb++ < 30) fprintf(stderr, "[gridquery] MISMATCH ret mine=%x oracle=%x scratchdiff@+%x\n", mine, oracle, firstoff);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridquery] %ld matches\n", ng);
  (void)N;
  c->r[2] = oracle;                                            // keep oracle scratchpad state
}

// FUN_800498C8 — collision-grid RESOLVE LOOP (top of the grid family; pairs with the owned
// FUN_80049968 setup + FUN_80047CBC query). a0 = probe object. Iterates:
//   jal 0x8004798C(obj)                    -- per-step grid-origin/index setup (kept dispatched; non-trivial)
//   jal 0x80049968(u8 @0x1F8001FE)         -- row-pointer setup (owned ov_grid_setup_49968)
//   v0 = jal 0x80047CBC()                  -- cell query/neighbor-walk (owned ov_grid_query_47cbc)
//   if v0 == 0 -> return 0                  (query found nothing / off-grid -> done)
//   v1 = w[0x1F8001E0] (the cell record ptr the query latched)
//   if (h[v1] & 0x4000) == 0 -> return 1   (resolved cell is terminal -> done, keep)
//   obj[42] = b[v1]                         (record the resolved cell's tag byte onto the probe object)
//   reload v1' = w[0x1F8001E0]; if (h[v1'] & 0x4000) != 0 -> LOOP (descend further)
//   else -> return 1
// Pure control flow over scratchpad + object memory; ONE object write (obj+42); NO GTE, NO render
// packets. The three callees stay PSX via rec_dispatch (the two grid leaves honor their own owned
// override identically in the dispatched path). Return: 0 only when the query returns 0; otherwise 1.
static uint32_t grid_resolve_498c8(Core* c) {
  const uint32_t obj = c->r[4];
  for (;;) {
    c->r[4] = obj; rec_dispatch(c, 0x8004798Cu);                 // setup (dispatched)
    c->r[4] = (uint32_t)c->mem_r8(0x1F8001FEu); rec_dispatch(c, 0x80049968u);  // row-ptr setup (owned)
    rec_dispatch(c, 0x80047CBCu);                                // cell query (owned)
    if (c->r[2] == 0) return 0;
    uint32_t v1 = c->mem_r32(0x1F8001E0u);
    if ((c->mem_r16(v1) & 0x4000u) == 0) return 1;
    c->mem_w8(obj + 42, c->mem_r8(v1));                          // record tag byte onto the object
    uint32_t v1b = c->mem_r32(0x1F8001E0u);
    if ((c->mem_r16(v1b) & 0x4000u) != 0) continue;             // bne v0,zero,0x800498e8 -> loop
    return 1;
  }
}

void ov_grid_resolve_498c8(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridresolve") ? 1 : 0;
  if (!s_v) { c->r[2] = grid_resolve_498c8(c); return; }
  // Full RAM+scratchpad A/B vs rec_super_call. The native path runs first, its writes are snapshotted
  // and rolled back, then the recomp body runs and we diff. The dispatched callees (incl. the deep
  // FUN_8004798C tree) run in BOTH passes; FUN_800498C8's own 32-byte stack frame [sp-32, sp) is dead
  // below sp on return (gen saves regs there; native never touches the guest stack) -> excluded.
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = grid_resolve_498c8(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x800498C8u);
  uint32_t v0_o = c->r[2];
  // Exclude pure stack scratch below entry sp: the dispatched callee tree (FUN_8004798C -> 0x80048ecc/
  // 0x80048fc4 + the grid leaves) runs in BOTH passes and leaves transient values in its OWN frames below
  // sp; because FUN_800498C8's native frame is absent, the residual bytes there differ harmlessly (same as
  // scriptvm/player). The exclusion window is the top-of-RAM stack (sp-0x800, sp) — far above ALL game
  // data (sp ~0x1FE9xx, RAM end 0x200000); a real behavioral divergence would alter persistent state below.
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[gridresolve] MISMATCH obj=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           obj, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridresolve] %ld matches\n", ng);
}

// FUN_8004798C — collision-grid PER-STEP ORIGIN/INDEX SETUP (the remaining dispatched callee inside
// the owned FUN_800498C8 resolve loop; completes the grid family with FUN_80049968 setup / FUN_80047CBC
// query / FUN_800498C8 resolve). a0 = probe object. Pure scratchpad halfword arithmetic + two dispatched
// callees; NO GTE, NO render packets. Scratchpad fields (base 0x1F800000):
//   0x1AA,0x1AC   = grid origin (X,Z)
//   0x1AE,0x1B0   = grid extents (X,Z)    [used unsigned in the select/clamp tests]
//   0x1B2,0x1B4   = grid cell base (X,Z)
//   0x1BA         = grid cell pitch       [signed; the >>14 fixed-point recompute multiplier]
//   0x1BC,0x1C0   = working probe coords (X,Z)
//   0x1FE (byte)  = current grid id
// Control flow:
//   if (obj[42] != byte[0x1FE]) jal 0x80048ecc(a0 = obj[42])    -- reload grid for this id (dispatched)
//   SELECT/RANGE TEST: if (h[0x1AE] u< h[0x1B0]) use the Z range else the X range; if probe is past the
//     selected range, jal 0x80048fc4(a0 = obj, a1 = 1)          -- re-resolve (dispatched)
//   CLAMP + RECOMPUTE: on (h[0x1AE] u< h[0x1B0]) -> Z branch (clamp 0x1C0 into [0x1AC, 0x1AC+0x1B0],
//     recompute 0x1BC) else X branch (clamp 0x1BC into [0x1AA, 0x1AA+0x1AE], recompute 0x1C0).
//   recompute writes the OTHER coord = cellbase + (((clamped - cellbase2) * pitch) >> 14) (signed mult,
//   low word). NB the >>14 is an arithmetic shift of the 32-bit low product (sra).
// `gridstep` gate = full RAM+scratchpad A/B vs rec_super_call (the two dispatched callees run in BOTH
// passes; this fn's own [sp-24, sp) stack frame + the callees' frames below sp differ harmlessly, so the
// gate excludes [sp-0x800, sp) — same family rationale as gridresolve/scriptvm).
static void grid_step_4798c(Core* c) {
  const uint32_t SP = 0x1F800000u, obj = c->r[4];
  // ---- block 1: reload grid if the object's recorded id differs ----
  uint32_t v1 = c->mem_r8(obj + 42);
  uint32_t gid = c->mem_r8(SP + 0x1FE);
  if (v1 != gid) { c->r[4] = v1; rec_dispatch(c, 0x80048eccu); }
  // ---- block 2: select range (Z if h[0x1AE] u< h[0x1B0], else X), test, maybe re-resolve ----
  uint32_t aE = c->mem_r16(SP + 0x1AE);   // h[0x1AE] (a1)
  uint32_t b0 = c->mem_r16(SP + 0x1B0);   // h[0x1B0] (a0)
  uint32_t test;
  if (aE < b0) {                          // sltu(a1,a0) != 0 -> Z range
    uint32_t d = (c->mem_r16(SP + 0x1C0) - c->mem_r16(SP + 0x1AC)) & 0xffffu;
    test = (b0 < d) ? 1u : 0u;            // sltu(a0, d)
  } else {                                // X range
    uint32_t d = (c->mem_r16(SP + 0x1BC) - c->mem_r16(SP + 0x1AA)) & 0xffffu;
    test = (aE < d) ? 1u : 0u;            // sltu(a1, d)
  }
  if (test != 0) { c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x80048fc4u); }
  // ---- block 3: clamp the in-range coord, then recompute the other from it ----
  uint32_t lo = c->mem_r16(SP + 0x1AE), hi = c->mem_r16(SP + 0x1B0);
  if (lo < hi) {
    // Z branch: clamp 0x1C0 into [0x1AC, 0x1AC + 0x1B0], then recompute 0x1BC
    int32_t  a2 = (int16_t)c->mem_r16(SP + 0x1C0);
    int32_t  a1 = (int16_t)c->mem_r16(SP + 0x1AC);
    uint32_t v1u = c->mem_r16(SP + 0x1AC);
    if (a2 < a1) {
      c->mem_w16(SP + 0x1C0, (uint16_t)v1u);
    } else {
      uint32_t a0u = c->mem_r16(SP + 0x1B0);
      if ((int32_t)((uint32_t)a1 + a0u) < a2) c->mem_w16(SP + 0x1C0, (uint16_t)(v1u + a0u));
    }
    int32_t  cv  = (int16_t)c->mem_r16(SP + 0x1C0);
    uint32_t cb  = c->mem_r16(SP + 0x1B4);
    int32_t  pit = (int16_t)c->mem_r16(SP + 0x1BA);
    int32_t  prod = (int32_t)((uint32_t)((uint32_t)cv - cb) * (uint32_t)pit);  // lo(mult)
    int32_t  v = prod >> 14;
    c->mem_w16(SP + 0x1BC, (uint16_t)(c->mem_r16(SP + 0x1B2) + (uint32_t)v));
  } else {
    // X branch: clamp 0x1BC into [0x1AA, 0x1AA + 0x1AE], then recompute 0x1C0
    int32_t  a2 = (int16_t)c->mem_r16(SP + 0x1BC);
    int32_t  a1 = (int16_t)c->mem_r16(SP + 0x1AA);
    uint32_t v1u = c->mem_r16(SP + 0x1AA);
    if (a2 < a1) {
      c->mem_w16(SP + 0x1BC, (uint16_t)v1u);
    } else {
      uint32_t a0u = c->mem_r16(SP + 0x1AE);
      if ((int32_t)((uint32_t)a1 + a0u) < a2) c->mem_w16(SP + 0x1BC, (uint16_t)(v1u + a0u));
    }
    int32_t  cv  = (int16_t)c->mem_r16(SP + 0x1BC);
    uint32_t cb  = c->mem_r16(SP + 0x1B2);
    int32_t  pit = (int16_t)c->mem_r16(SP + 0x1BA);
    int32_t  prod = (int32_t)((uint32_t)((uint32_t)cv - cb) * (uint32_t)pit);  // lo(mult)
    int32_t  v = prod >> 14;
    c->mem_w16(SP + 0x1C0, (uint16_t)(c->mem_r16(SP + 0x1B4) + (uint32_t)v));
  }
}

void ov_grid_step_4798c(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridstep") ? 1 : 0;
  if (!s_v) { grid_step_4798c(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  grid_step_4798c(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8004798Cu);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[gridstep] MISMATCH obj=%08x ram@%x spad@%x sp=%x\n", obj, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridstep] %ld matches\n", ng);
}
