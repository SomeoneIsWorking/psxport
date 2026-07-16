// game/render/overlay_ground_gt3gt4.cpp — native mirror of the A00-overlay GROUND/SCENE
// GT3/GT4 packet-emitter pair (FUN_8013FB88/8013FE58) + their entity-loop caller (FUN_801401B8).
//
// WIRED + SBS-gated — see overlay_ground_gt3gt4.h banner. Two things fixed in this session
// against generated/ov_a00_shard_0.c ov_a00_gen_801401B8 (ground truth):
//   (1) BUG: entityLoop's ground-record lookup did an extra pointer dereference
//       (`rec = mem_r32(table+idx*4); counts = mem_r32(rec+0); recBase = rec+4`). Ground truth
//       has NO second dereference — `table+idx*4` IS the counts word's own address, and
//       `table+idx*4+4` IS recBase (the table holds inline per-slot data: header word then the
//       GT3-then-GT4 record array, not a pointer to a separate struct). Fixed below.
//   (2) GAP: entityLoop's own real 40-byte guest-stack frame (`addiu sp,-40`, spills at
//       +16(r16)/+20(r17)/+24(r18)/+28(r19)/+32(r20)/+36(ra)) was not mirrored at all (the
//       file's own prior banner flagged this honestly). Mirrored below per CLAUDE.md's "MIRROR
//       THE GUEST STACK" directive, same wrapFrame idiom as game/render/cull.cpp: spill the
//       CALLER's live incoming values, run the body (host locals — nothing else touches these
//       registers mid-call), restore, ascend.
//
// RE: read directly off the recompiler's own register-accurate translation (the project
// convention for GTE-heavy leaves — generated/ov_a00_shard_0.c ov_a00_gen_8013FB88/801401B8,
// generated/ov_a00_shard_1.c ov_a00_gen_8013FE58 — Ghidra's COP2 decompilation garbles GTE
// register indices into placeholder immediates, so the recompiler's literal per-instruction C is
// the more precise source here, same as overlay_gt3gt4.cpp's own RE note).
//
// SIBLING of the field-object pair (game/render/overlay_gt3gt4.cpp, FUN_801465EC/801467BC,
// ALREADY OWNED + SBS-gated). docs/engine_re.md's "GAME-STAGE OBJECT PIPELINE" step 7 names this
// pair explicitly as the OTHER (ground/scene) copy: "...ground/scene entities go
// 0x8003D0BC -> 0x801401B8 (entity loop) -> 0x8013FE58/0x8013FB88 (overlay GT4/GT3 submitters)".
// Same overall shape (GTE RTPT/RTPS + NCLIP + AVSZ-or-custom-blend -> packet-pool bump allocator
// + OT linked list) but genuinely DIFFERENT in several concrete ways from the field pair —
// preserved literally below, NOT "fixed" to match:
//   - GT3's rec+4 colour word (rgb1/rgb2) masks with 0x00F0F0F0 (drops the top byte entirely),
//     not the field pair's COL_MASK 0xFFF0F0F0.
//   - GT4's rec+0 colour word (rgb0/rgb1) masks with the STANDARD 0xFFF0F0F0, but its rec+4
//     word (rgb2/rgb3) masks with 0x00F0F0F0 — a per-slot mix, not a uniform choice.
//   - GT3's rgb0|code word is written UNMASKED (raw) — this asymmetry IS shared with the field
//     GT3 leaf (verified: same pattern), so it's a genuine cross-cutting PSX packet-format trait,
//     not a copy-paste bug.
//   - The near-plane Z-select for a nonzero flag byte is a TRUE 3-way (GT3) / 4-way (GT4)
//     min-or-max of the raw SZ FIFO values, selected by flag&3 (1=>max/far, 2=>min/near, 0=>the
//     ordinary AVSZ3/4 average) — verified by fully unrolling both stack-spill branches by hand
//     (see sz3_minmax/sz4_minmax below). This differs from the field pair's flag&2 pairwise-clamp
//     encoding (mathematically equivalent to min/max of 3, but a DIFFERENT flag-bit convention —
//     do not conflate the two encodings when a future session wires this).
//   - GT3 also stages a 16-bit uv2-hi word at packet offset +36 (mem_w16, from the upper half of
//     rec+32) that the field GT3 leaf's OWN port does not currently write (its comment documents
//     the field position but the already-landed body never stores it) — reproduced here because
//     it IS a real guest write the recomp body performs; left as an open question for whoever
//     next audits the field leaf's byte-exactness, not "fixed" on either side here.
//
// Guest-stack frames: MIRRORED per CLAUDE.md ("guest-stack frames: MIRROR, never revert/
// exclude") — gt3's real frame is `addiu sp,-24` (6 words: two 3-slot scratch groups for the
// unrolled min/max, no saved registers); gt4's is `addiu sp,-32` (8 words: two 4-slot groups);
// entityLoop's is `addiu sp,-40` with 6 real register spills (s0..s4/ra equivalents + return
// address) around its two call sites. All descend/spill/ascend on the REAL c->r[29], matching
// game/render/cull.cpp's Cull::wrapFrame idiom.
#include "core.h"
#include "game.h"
#include "overlay_ground_gt3gt4.h"
#include "cfg.h"
#include <cstdint>

// -- packet-pool bump allocator (SAME process-global pointer the field pair uses — one shared
//    pool across every render-underneath submitter in the frame; see overlay_gt3gt4.cpp).
#define PKT_POOL_PTR      0x800BF544u

// -- the two fixed SCRATCHPAD words this leaf pair uses as spilled locals (0x1F800000/0x1F800004
//    — literal PSX addresses baked into the recompiled MIPS, not a tuning constant): the recomp
//    body spills its "GTE FLAG / MAC0 sign" temp to +0 and its "OTZ pick" temp to +4. Scratchpad
//    is part of the SBS-compared state, so these MUST be real c->mem_w32 writes, not C++ locals.
#define SCRATCH_FLAG_TMP  0x1F800000u
#define SCRATCH_OTZ_TMP   0x1F800004u

// -- colour masks: the STANDARD field-pair mask (rgb0 slot of GT4 only) vs the ground-specific
//    "drop the whole top byte" mask (every other colour slot in both ground leaves).
#define COL_MASK_STD      0xFFF0F0F0u
#define COL_MASK_GROUND   0x00F0F0F0u

// -- OT tag length words (matches the field pair's convention: len<<24 packed into the recycled
//    OT-bucket head pointer).
#define TAG_LEN_GT3       (9u  << 24)   // 36-byte GT3 packet body (9 words)
#define TAG_LEN_GT4       (12u << 24)   // 52-byte GT4 packet body (13 words incl. tag)

// -- OT-index compute (byte-identical to overlay_gt3gt4.cpp's overlay_gt_otz_index — same
//    bit-recombination + [4, 0x7ff) range gate). Kept as a private static twin rather than
//    sharing the symbol across two still-independent, still-unwired classes.
static int32_t ground_otz_index(int32_t z) {
  int32_t shift = z >> 10;
  int32_t idx = (z >> (shift & 31)) + shift * 0x200;
  // gen_func_8013FB88 L24069-24075: the ONLY range gate is `(idx - 4) < 2044` -> keep (i.e. drop
  // when idx >= 2048), then `if (idx < 0) skip`. NO lower bound. A prior draft mis-split gen's
  // single upper-bound expression into a two-sided range [4..2047] — the spurious `idx <= 4`
  // rejection dropped records gen emits (pool offset shift, the f118 divergence) and the upper
  // bound was off-by-one (2047 vs 2048). Match gen's one check exactly.
  if ((uint32_t)(idx - 4) >= 2044u) return -1;
  return idx;
}

// Fully-unrolled 3-way min/max (GT3's flag&3==1 => max, ==2 => min branch, hand-verified by
// walking BOTH stack-spill code paths of FUN_8013FB88 to their common convergence point). The
// convergence labels (L_8013FD38/L_8013FD98/L_8013FDA4) apply a final ARITHMETIC `>> 2` to the
// selected value before storing it to the OTZ scratch (gen L24053/24056: `r2 = r3 >> 2`). The
// prior draft returned the raw min/max — 4x too large, so too many records failed the
// `idx >= 2048` depth gate and got dropped (pool offset shift, the f118 divergence). The AVSZ3
// path (below, in gt3/gt4) correctly has NO >>2 — only this manual min/max path does.
static int32_t sz3_minmax(bool want_max, int32_t a, int32_t b, int32_t c3) {
  int32_t hi_ab = want_max ? (a > b ? a : b) : (a < b ? a : b);
  int32_t r     = want_max ? (hi_ab > c3 ? hi_ab : c3) : (hi_ab < c3 ? hi_ab : c3);
  return r >> 2;
}
// Same, 4-way (GT4's flag&3==1/2 branch: pairs (a,b) and (e,f), then combine). Same trailing
// `>> 2` as sz3_minmax (gen gt4 L_80140100: `r2 = r3 >> 2` at the convergence label, stored to OTZ).
static int32_t sz4_minmax(bool want_max, int32_t a, int32_t b, int32_t e, int32_t f) {
  int32_t hi_ab = want_max ? (a > b ? a : b) : (a < b ? a : b);
  int32_t hi_ef = want_max ? (e > f ? e : f) : (e < f ? e : f);
  return (want_max ? (hi_ab > hi_ef ? hi_ab : hi_ef) : (hi_ab < hi_ef ? hi_ab : hi_ef)) >> 2;
}

// FUN_8013FB88 — ground/scene POLY_GT3 emit. Record = 36 bytes, SAME field layout as the
// field-object GT3 leaf: {+0 rgb0|code, +4 rgb1(rgb2=rgb1<<4)|flag@[31:24], +8 uv0|clut,
// +12 uv1|tpage, +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2hi(hi)}.
// Output packet = 40 bytes: {+0 tag(len=9<<24|next), +4 rgb0|code RAW (unmasked, matches the
// field leaf's own asymmetry), +8 SXY0, +12 uv0|clut, +16 rgb1&COL_MASK_GROUND, +20 SXY1,
// +24 uv1|tpage, +28 rgb2&COL_MASK_GROUND, +32 SXY2, +36 uv2hi (16-bit)}.
//
// WRITE-ORDER FIX (2026-07-10, convergence-agent): a prior draft grouped rgb2/uv0/uv1 into one
// block placed AFTER the on-screen tests. Ground truth (`ov_a00_gen_8013FB88`, re-read
// instruction-by-instruction this session) writes uv0 (pool+12) and uv1 (pool+24) UNCONDITIONALLY
// right after RTPT — BEFORE the GTE-FLAG check, BEFORE NCLIP, BEFORE the backface/on-screen gates
// — while rgb2 (pool+28) stays where it was, genuinely AFTER the on-screen tests. This is NOT
// cosmetic: for a REJECTED record (one that fails a LATER gate — GTE FLAG, backface, on-screen,
// or the OTZ range check), gen has ALREADY written pool+12/pool+24 as dead-but-real bump-allocator
// scratch before bailing, while the prior draft's native body — gating uv0/uv1 behind the SAME
// late block as rgb2 — never wrote them for that rejected record at all. When a LATER record from
// a different emitter (or a later loop iteration) reuses that exact pool address, the two engines'
// "dead" leftover bytes differ — the f158 packet-pool `sbs-div` residual (docs/findings/render.md).
// Fix: uv0/uv1 moved to fire exactly where gen fires them (right after RTPT, unconditional).
// ORACLE: ov_a00_gen_8013FB88 (tools/port_check.py equivalence-gate marker; see docs/port-framework.md)
void OverlayGroundGt3Gt4::gt3(Core* c) {
  uint32_t rec = c->r[4], ot_base = c->r[5], count = c->r[6];
  if (cfg_dbg("ovgt")) { static long n=0; if (n++%512==0) cfg_logf("ovgt", "[ovgtgnd] gt3 call#%ld count=%u", n, count); }
  if (count == 0) { c->r[2] = rec; return; }

  uint32_t pool = c->mem_r32(PKT_POOL_PTR);
  uint32_t sp = c->r[29]; c->r[29] -= 24;              // real frame: 6 scratch words, no spills

  for (; count != 0; count--, rec += 36) {
    gte_write_data(0, c->mem_r32(rec + 16));            // VXY0
    uint32_t vz01 = c->mem_r32(rec + 20);
    gte_write_data(2, c->mem_r32(rec + 24));            // VXY1
    gte_write_data(1, vz01);                            // VZ0
    gte_write_data(4, c->mem_r32(rec + 28));            // VXY2
    gte_write_data(3, vz01 >> 16);                       // VZ1
    gte_write_data(5, c->mem_r32(rec + 32));             // VZ2(lo)|uv2hi(hi)
    uint32_t rgb0_code = c->mem_r32(rec + 0);
    c->mem_w32(pool + 4, rgb0_code);                     // rgb0 -- unconditional
    gte_op(c, 0x4A280030u);                              // RTPT

    // uv0/uv1 -- UNCONDITIONAL right after RTPT (gen writes these before ANY gate, including the
    // GTE FLAG check below); see banner.
    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12);
    c->mem_w32(pool + 12, uv0);
    c->mem_w32(pool + 24, uv1);

    uint32_t rgb1_src = c->mem_r32(rec + 4);
    uint32_t flagreg  = gte_read_ctrl(31);
    c->mem_w32(SCRATCH_FLAG_TMP, flagreg);
    if ((int32_t)c->mem_r32(SCRATCH_FLAG_TMP) < 0) continue;   // GTE FLAG error -> drop record

    gte_op(c, 0x4B400006u);                              // NCLIP (backface / MAC0)
    uint32_t rgb1 = rgb1_src & COL_MASK_GROUND;
    c->mem_w32(pool + 16, rgb1);
    c->mem_w32(SCRATCH_FLAG_TMP, gte_read_data(24));      // MAC0
    if ((int32_t)c->mem_r32(SCRATCH_FLAG_TMP) <= 0) continue;  // backface cull

    c->mem_w32(pool + 8,  gte_read_data(12));             // SXY0
    c->mem_w32(pool + 20, gte_read_data(13));             // SXY1
    c->mem_w32(pool + 32, gte_read_data(14));             // SXY2

    if (!((c->mem_r16(pool+8) < 320) || (c->mem_r16(pool+20) < 320) || (c->mem_r16(pool+32) < 320))) continue;
    if (!((c->mem_r16(pool+10) < 240) || (c->mem_r16(pool+22) < 240) || (c->mem_r16(pool+34) < 240))) continue;

    uint32_t rgb2 = (rgb1_src << 4) & COL_MASK_GROUND;
    c->mem_w32(pool + 28, rgb2);                          // rgb2 -- LATE, after the on-screen tests (unchanged)

    uint32_t flagbyte = rgb1_src >> 24;
    int32_t z;
    uint32_t mode = flagbyte & 3u;
    if (mode == 1u || mode == 2u) {
      int32_t sz1 = (int32_t)gte_read_data(17), sz2 = (int32_t)gte_read_data(18), sz3 = (int32_t)gte_read_data(19);
      // Guest-stack mirror (RE: ov_a00_gen_8013FB88 L_8013FD00 vs L_8013FD54) — the ORIGINAL
      // compiler inlined the same sz-minmax computation TWICE at two DIFFERENT dead-scratch stack
      // offsets depending on mode: mode==1 writes to (new sp)+0/4/8, mode==2 writes to (new
      // sp)+12/16/20. A prior draft always used the mode==1 offsets, so mode==2 records never
      // touched (new sp)+12..+23 — the exact f179 SBS residual at 0x801FE924 (task-0 stack, this
      // function's own frame at offset +20). Mirror the REAL offset per CLAUDE.md ("MIRROR THE
      // GUEST STACK... never exclude a slot because it looks like dead scratch").
      const uint32_t base = (mode == 1u) ? (sp - 24 + 0) : (sp - 24 + 12);
      c->mem_w32(base + 0, sz1); c->mem_w32(base + 4, sz2); c->mem_w32(base + 8, sz3); // real stack mirror
      z = sz3_minmax(mode == 1u, sz1, sz2, sz3);
      c->mem_w32(SCRATCH_OTZ_TMP, z);
    } else {
      gte_op(c, 0x4B58002Du);                             // AVSZ3
      z = (int32_t)gte_read_data(7);
      c->mem_w32(SCRATCH_OTZ_TMP, z);
    }

    int32_t idx = ground_otz_index(z);
    c->mem_w32(SCRATCH_OTZ_TMP, (uint32_t)idx);
    if (idx < 0) continue;

    // uv2hi (16-bit, high half of rec+32) — a real guest write the recomp body performs at this
    // packet slot; see file banner re: the field leaf not currently reproducing it.
    c->mem_w16(pool + 36, (uint16_t)(c->mem_r32(rec + 32) >> 16));

    uint32_t slot_addr = ot_base + (uint32_t)idx * 4;
    uint32_t old_head = c->mem_r32(slot_addr);
    c->mem_w32(slot_addr, pool);
    c->mem_w32(pool + 0, old_head | TAG_LEN_GT3);
    pool += 40;
  }

  c->r[29] = sp;                                          // ascend
  c->mem_w32(PKT_POOL_PTR, pool);
  c->r[2] = rec;
}

// FUN_8013FE58 — ground/scene POLY_GT4 emit. Record = 44 bytes: {+0 rgb0(rgb1=rgb0<<4)|code,
// +4 rgb2(rgb3=rgb2<<4)|flag@[31:24], +8 uv0, +12 uv1, +16 uv2(lo)|uv3(hi), +20 VXY0,
// +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi), +40 VXY3}. Output packet = 52
// bytes: {+0 tag(len=12<<24|next), +4 rgb0&COL_MASK_STD, +8 SXY0, +12 uv0, +16 rgb1&
// COL_MASK_GROUND, +20 SXY1, +24 uv1, +28 rgb2&COL_MASK_GROUND, +32 SXY2, +36 uv2, +40
// rgb3&COL_MASK_GROUND, +44 SXY3, +48 uv3}. Note the PER-SLOT mask mix (rgb0 standard, rgb1-3
// ground) — verified against the recomp body, not simplified to one constant.
//
// WRITE-ORDER FIX (2026-07-10, convergence-agent): same class of bug as gt3 above, found by fully
// re-reading `ov_a00_gen_8013FE58` instruction-by-instruction (a prior draft's block-grouped
// uv0/rgb2/rgb3/uv1/uv2/uv3 writes right after the backface gate did NOT match gen's real,
// interleaved gate/write order). Ground truth's actual order per record:
//   rgb0(pool+4) -> RTPT -> rgb1(pool+16) -> [load rec4] -> GTE-FLAG#1 gate -> NCLIP ->
//   uv0(pool+12) -> MAC0/backface gate -> SXY0/1/2 -> [GTE-write VXY3/VZ3] -> rgb2(pool+28) ->
//   RTPS -> rgb3(pool+40) -> uv1(pool+24) -> GTE-FLAG#2 gate -> SXY3(pool+44) -> on-screen X/Y
//   gates -> OTZ range gate -> uv2(pool+36)/uv3(pool+48) [LATEST — only once fully accepted] ->
//   OT link. Two consequences the prior draft's block grouping got wrong for a REJECTED record:
//   uv0 is written UNCONDITIONALLY right after NCLIP (before the backface gate — gen has already
//   written it even for a backface-culled record), and uv2/uv3 are written ONLY once the record
//   clears every gate including the OTZ range check (the prior draft wrote them right after the
//   backface gate, i.e. for records that get rejected LATER by the on-screen or OTZ gates, when
//   gen never touches pool+36/+48 at all for that same rejected record). This exact mismatch —
//   native leaving real uv2/uv3 content in a "dead" pool slot that gen leaves untouched — is the
//   f158 packet-pool `sbs-div` residual (docs/findings/render.md): a LATER record (from this same
//   emitter or an adjacent one) that reuses that exact pool address inherits two different
//   "leftover" byte patterns on the two engines. Fix: reordered to match gen's write timing
//   exactly, gate for gate.
void OverlayGroundGt3Gt4::gt4(Core* c) {
  uint32_t rec = c->r[4], ot_base = c->r[5], count = c->r[6];
  if (count == 0) { c->r[2] = rec; return; }

  uint32_t pool = c->mem_r32(PKT_POOL_PTR);
  uint32_t sp = c->r[29]; c->r[29] -= 32;                 // real frame: 8 scratch words, no spills

  for (; count != 0; count--, rec += 44) {
    gte_write_data(0, c->mem_r32(rec + 20));              // VXY0
    uint32_t vz01 = c->mem_r32(rec + 24);
    gte_write_data(2, c->mem_r32(rec + 28));              // VXY1
    gte_write_data(1, vz01);                              // VZ0
    gte_write_data(4, c->mem_r32(rec + 32));              // VXY2
    gte_write_data(3, vz01 >> 16);                        // VZ1
    uint32_t vz23 = c->mem_r32(rec + 36);
    gte_write_data(5, vz23);                              // VZ2

    uint32_t hdr0 = c->mem_r32(rec + 0);
    c->mem_w32(pool + 4, hdr0 & COL_MASK_STD);            // rgb0, STANDARD mask (differs from GT3's raw + from rgb1-3 below)
    gte_op(c, 0x4A280030u);                                // RTPT (verts 0..2)
    c->mem_w32(pool + 16, (hdr0 << 4) & COL_MASK_GROUND);  // rgb1

    uint32_t rec4 = c->mem_r32(rec + 4);                   // rgb2|flag -- loaded EARLY, before any gate

    uint32_t flagreg = gte_read_ctrl(31);
    c->mem_w32(SCRATCH_FLAG_TMP, flagreg);
    if ((int32_t)c->mem_r32(SCRATCH_FLAG_TMP) < 0) continue;
    gte_op(c, 0x4B400006u);                                // NCLIP
    uint32_t uv0 = c->mem_r32(rec + 8);
    c->mem_w32(pool + 12, uv0);                            // uv0 -- UNCONDITIONAL right after NCLIP, before the backface gate

    c->mem_w32(SCRATCH_FLAG_TMP, gte_read_data(24));       // MAC0
    if ((int32_t)c->mem_r32(SCRATCH_FLAG_TMP) <= 0) continue;  // backface cull

    c->mem_w32(pool + 8,  gte_read_data(12));              // SXY0
    c->mem_w32(pool + 20, gte_read_data(13));              // SXY1
    c->mem_w32(pool + 32, gte_read_data(14));              // SXY2

    gte_write_data(0, c->mem_r32(rec + 40));               // VXY3
    gte_write_data(1, vz23 >> 16);                          // VZ3

    c->mem_w32(pool + 28, rec4 & COL_MASK_GROUND);         // rgb2
    gte_op(c, 0x4A180001u);                                // RTPS (4th point)
    c->mem_w32(pool + 40, (rec4 << 4) & COL_MASK_GROUND);  // rgb3
    uint32_t uv1 = c->mem_r32(rec + 12);
    c->mem_w32(pool + 24, uv1);                            // uv1

    uint32_t flagreg2 = gte_read_ctrl(31);
    c->mem_w32(SCRATCH_FLAG_TMP, flagreg2);
    if ((int32_t)c->mem_r32(SCRATCH_FLAG_TMP) < 0) continue;
    c->mem_w32(pool + 44, gte_read_data(14));              // SXY3

    if (!((c->mem_r16(pool+8) < 320) || (c->mem_r16(pool+20) < 320) || (c->mem_r16(pool+32) < 320) || (c->mem_r16(pool+44) < 320))) continue;
    if (!((c->mem_r16(pool+10) < 240) || (c->mem_r16(pool+22) < 240) || (c->mem_r16(pool+34) < 240) || (c->mem_r16(pool+46) < 240))) continue;

    uint32_t flagbyte = rec4 >> 24;
    int32_t z;
    uint32_t mode = flagbyte & 3u;
    if (mode == 1u || mode == 2u) {
      int32_t sz1 = (int32_t)gte_read_data(16), sz2 = (int32_t)gte_read_data(17),
              sz3 = (int32_t)gte_read_data(18), sz4 = (int32_t)gte_read_data(19);
      // Guest-stack mirror (RE: ov_a00_gen_8013FE58, same duplicated-inline shape as gt3 above) —
      // mode==1 writes to (new sp)+0/4/8/12, mode==2 writes to (new sp)+16/20/24/28. A prior draft
      // always used the mode==1 offsets; mirror the real per-mode offset (CLAUDE.md "MIRROR THE
      // GUEST STACK").
      const uint32_t base = (mode == 1u) ? (sp - 32 + 0) : (sp - 32 + 16);
      c->mem_w32(base + 0, sz1); c->mem_w32(base + 4, sz2);
      c->mem_w32(base + 8, sz3); c->mem_w32(base + 12, sz4);     // real stack mirror
      z = sz4_minmax(mode == 1u, sz1, sz2, sz3, sz4);
      c->mem_w32(SCRATCH_OTZ_TMP, z);
    } else {
      gte_op(c, 0x4B68002Eu);                              // AVSZ4
      z = (int32_t)gte_read_data(7);
      c->mem_w32(SCRATCH_OTZ_TMP, z);
    }

    int32_t idx = ground_otz_index(z);
    c->mem_w32(SCRATCH_OTZ_TMP, (uint32_t)idx);
    if (idx < 0) continue;

    // uv2/uv3 -- LATEST: only once the record has cleared every gate including this OTZ range
    // check (matches gen's placement immediately before the OT-link below).
    uint32_t uv23 = c->mem_r32(rec + 16);
    c->mem_w32(pool + 36, uv23);                           // uv2 (lo half)
    c->mem_w32(pool + 48, uv23 >> 16);                     // uv3 (hi half)

    uint32_t slot_addr = ot_base + (uint32_t)idx * 4;
    uint32_t old_head = c->mem_r32(slot_addr);
    c->mem_w32(slot_addr, pool);
    c->mem_w32(pool + 0, old_head | TAG_LEN_GT4);
    pool += 52;
  }

  c->r[29] = sp;
  c->mem_w32(PKT_POOL_PTR, pool);
  c->r[2] = rec;
}

#define CAMERA_GTE_CTRL 0x1F8000F8u
#define OT_BASE_GLOBAL  0x800ED8C8u

// FUN_801401B8 — the ground-entity render list walker. list=a0: +6 (u8) entry count, +12 (u32)
// pointer table base, +16.. (u16 index array, one per entry). Loads the SHARED camera GTE
// control-register block once (CR0..7 from scratchpad 0x1F8000F8, the SAME "camera" block
// docs/engine_re.md's render-command-flush note names as feeding the mode dispatcher), then for
// each list entry: table[idx] -> a per-object record whose +0 word packs {lo byte: GT3 count,
// (word>>16)&0xff: GT4 count} and whose +4 points at the actual GT3-record-then-GT4-record
// array gt3()/gt4() walk. OT base = *0x800ED8C8 (the SAME global the render dispatcher's
// "*0x800ED8C8 OTbase" note documents for queued commands).
void OverlayGroundGt3Gt4::entityLoop(Core* c) {
  uint32_t list = c->r[4];
  if (cfg_dbg("ovgtgnd")) { static long n = 0; if (n++ % 128 == 0) cfg_logf("ovgtgnd", "entityLoop call#%ld", n); }

  // Real 40-byte guest stack frame (RE: generated/ov_a00_shard_0.c ov_a00_gen_801401B8:
  // `addiu sp,-40; sw ra,36(sp); sw r20,32(sp); sw r19,28(sp); sw r18,24(sp); sw r17,20(sp);
  // sw r16,16(sp)`) — six LIVE incoming register spills. Mirrored per CLAUDE.md ("MIRROR THE
  // GUEST STACK... never revert/exclude a leaf because it pushes a frame"): this function's own
  // body never needs r16..r20/ra as WORKING registers mid-call (its locals below are host C++
  // values; nothing else runs mid-call to observe a different value), so the mirror is exactly
  // the simple wrapFrame idiom (cull.cpp): spill the caller's live values, run, restore, ascend.
  uint32_t save16 = c->r[16], save17 = c->r[17], save18 = c->r[18];
  uint32_t save19 = c->r[19], save20 = c->r[20], saveRa = c->r[31];
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 36, saveRa);
  c->mem_w32(c->r[29] + 32, save20);
  c->mem_w32(c->r[29] + 28, save19);
  c->mem_w32(c->r[29] + 24, save18);
  c->mem_w32(c->r[29] + 20, save17);
  c->mem_w32(c->r[29] + 16, save16);

  uint8_t count = c->mem_r8(list + 6);
  uint32_t idxCursor = list + 16;
  uint32_t idxEnd = idxCursor + (uint32_t)count * 2;

  uint32_t otBase = c->mem_r32(OT_BASE_GLOBAL);
  uint32_t table = c->mem_r32(list + 12);

  // camera GTE control block (8 words, CR0..CR7 — matches the recomp body's own 8-word load)
  for (int i = 0; i < 8; i++) gte_write_ctrl((uint32_t)i, c->mem_r32(CAMERA_GTE_CTRL + (uint32_t)i * 4));

  while (idxCursor < idxEnd) {
    uint16_t idx = c->mem_r16(idxCursor);
    idxCursor += 2;
    // FIX (2026-07-08): a prior draft added a spurious extra pointer dereference here
    // (`rec = mem_r32(table+idx*4); counts = mem_r32(rec+0); recBase = rec+4`). Ground truth
    // (ov_a00_gen_801401B8) has only ONE dereference: `table+idx*4` IS the counts word's own
    // address (r16 = mem_r32(table+idx*4) used directly as the packed count word, never
    // re-dereferenced), and `table+idx*4+4` IS recBase — the table holds INLINE per-slot data
    // (header word then the GT3-then-GT4 record array), not a pointer to a separate struct.
    uint32_t tableSlot = table + (uint32_t)idx * 4;
    uint32_t counts = c->mem_r32(tableSlot);
    uint32_t recBase = tableSlot + 4;

    c->r[4] = recBase; c->r[5] = otBase; c->r[6] = counts & 0xFFu;
    gt3(c);
    recBase = c->r[2];

    c->r[4] = recBase; c->r[5] = otBase; c->r[6] = (counts >> 16) & 0xFFu;
    gt4(c);
  }

  c->r[31] = c->mem_r32(c->r[29] + 36);
  c->r[20] = c->mem_r32(c->r[29] + 32);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 40;
}

// Wiring (frontier, 2026-07-08): all three leaves are reached only by a direct C call the
// recompiler generates inside the ov_a00 shard (never rec_dispatch), so wired via the overlay's
// own process-global g_ov_a00_override[] table — same discipline as OverlayGt3Gt4's twin cluster.
// engine_set_override_a00 (runtime/recomp/override_registry.h) installs into the ONE process-global
// override registry, which runs ov_a00_gen_* on the oracle leg (core B) and the native handler
// everywhere else — NOT a raw ov_a00_set_override.
void OverlayGroundGt3Gt4::registerOverrides(Game*) {
  extern void ov_a00_gen_8013FB88(Core*);
  extern void ov_a00_gen_8013FE58(Core*);
  extern void ov_a00_gen_801401B8(Core*);
  extern void engine_set_override_a00(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_a00(0x8013FB88u, &OverlayGroundGt3Gt4::gt3,        ov_a00_gen_8013FB88);
  engine_set_override_a00(0x8013FE58u, &OverlayGroundGt3Gt4::gt4,        ov_a00_gen_8013FE58);
  engine_set_override_a00(0x801401B8u, &OverlayGroundGt3Gt4::entityLoop, ov_a00_gen_801401B8);
}
