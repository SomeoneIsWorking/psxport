// ⚠️ DRAFT — NOT WIRED INTO THE BUILD (not in build_port.sh / run.sh SRC lists, not registered in
// game_tomba2.cpp). Kept for its RE of FUN_8007e1b8 only. The repair strategy below (re-stamp inherit
// prims with the LAST-BOUND CLUT) reproduces the PSX GP0 CLUT latch — which the project directive
// forbids ("build the game as a PC game, do NOT replicate PSX"). The proper PC-native fix is to have the
// engine bind the UI font/atlas CLUT EXPLICITLY for inherit-descriptor rects from its own UI state, not
// to emulate the hardware latch. Issue #6 also does NOT currently reproduce (panel is transparent), so
// this is deferred until it reappears and the real font-atlas CLUT source is owned. See issue #6.
//
// PC-native ownership wrapper for the universal UI RECT emitter FUN_8007e1b8 (issue #6 — HUD/
// pause-menu opaque gray box). The engine OWNS its UI render; this override keeps the faithful
// PSX emit (super-call) but repairs the ONE field the PSX emitter intentionally drops for
// "inherit-texpage" descriptors, which the PSX hardware would have filled from its global GP0
// CLUT latch but our stateless PC submit path cannot.
//
// ===========================================================================================
//  RE of FUN_8007e1b8 (static; tools/disas.py 0x8007e1b8 [--mem]) — the universal UI rect emitter
// ===========================================================================================
//  ARGS (verified from the prologue):
//      a0 = t4  -> rect geometry record   (lh 4(t4) = width, lh 6(t4) = height)
//      a1       -> per-slice vertex/UV index (lh 0(a1) selects an entry in the a2 table)
//      a2       -> vertex/UV table base
//      a3 = t5  -> DESCRIPTOR pointer (this is the field of interest)
//
//  DESCRIPTOR layout (read via t5):
//      desc+0 (byte) : op-class 0..5 -> jump table @0x8001728c selects the prim builder case
//                       (and is masked to its low nibble, re-stored at desc+0).
//      desc+1 (byte) : OT bucket index, consumed at the emit/link tail (lbu v0,1(t5) @0x8007e62c).
//      desc+2 (u16)  : flags + CLUT.  Decoded as:
//                         t7 = desc+2 & 0x8000   -> SEMI/ABE: cmd byte 0x2C (off) vs 0x2E (on)
//                         t8 = desc+2 & 0x7fff   -> the CLUT id to store, OR 0 == "INHERIT"
//
//  THE BUG (0x8007e26c .. 0x8007e27c):
//         beq  t8, zero, 0x8007e280     ; if (desc+2 & 0x7fff) == 0  -> SKIP the store
//         lhu  v0, 2(t5)                ; v0 = desc+2
//         sh   v0, 14(t0)               ; *(u16*)(prim+0xE) = desc+2   (only when t8 != 0)
//      t0 = 0x1F800000 is the scratchpad PRIM TEMPLATE the case-builders fill; the emit tail
//      (0x8007e620..) copies template words +4..+0x24 into the OT pool (head @0x800BF544),
//      prepending a 1-word link tag, so each emitted pool prim is 11 words (0x2C) and the
//      template's +0xC word (CLUT in its high half) lands at pool-prim +0xE.
//
//  For a TEXTURED prim the word at template/pool +0xC = (CLUT << 16) | u0v0 and +0x14 =
//      (TPAGE << 16) | u1v1. The TPAGE (+0x16) is filled from the slice rec (lw 0(t3) / -11(t1));
//      ONLY the CLUT (+0xE) comes from desc+2. So for an INHERIT descriptor (desc+2 & 0x7fff == 0)
//      the prim is emitted with whatever CLUT halfword the template happened to already hold —
//      stale/zero. The pause-menu 9-slice panel (FUN_8007eae4, all 9 slices use a stack descriptor
//      with desc+2 = 0x8000: SEMI set, CLUT field 0 -> inherit) and the HUD gauge box take this
//      path. With a wrong/zero CLUT the textured-semi prim samples palette entries whose STP bit
//      is 0, and tritex.frag's faithful per-texel-STP gate (blend only where STP=1) leaves the
//      panel an opaque gray box. (Confirmed RE in issue #6: the semi flag IS preserved and the
//      shader is correct — must NOT relax the shader; the defect is the dropped CLUT.)
//
//      On real PSX hardware the "inherit" path is correct: the prim simply re-uses the GP0 CLUT
//      latched by the previously-drawn UI prim. Our PC submit path carries CLUT per-prim (there is
//      no global GP0 CLUT latch), so an inherit prim MUST be given an explicit CLUT.
//
// ===========================================================================================
//  THE FIX — back-fill the inherited CLUT, faithfully ("last bound"), NO magic constant
// ===========================================================================================
//  We reproduce the PSX "inherit = last-bound CLUT" semantics WITHIN this emitter family rather
//  than hardcoding an atlas CLUT id (which we cannot statically verify and which would be a magic
//  constant). The font/glyph emitters (FUN_80078ca8) and the various UI rects all flow CLUTs into
//  prim+0xE; we remember the most recent EXPLICIT (non-inherit) CLUT this emitter stored and reuse
//  it for inherit prims. That is exactly what the PSX GP0 latch would hold: the CLUT of the last
//  drawn UI prim from the same atlas. We capture it from the descriptor (cheap, exact) so we never
//  need to read it back out of the pool.
//
//  Mechanics (post-super-call, per the issue's candidate (b)):
//    1. Snapshot the pool head (*0x800BF544) BEFORE the faithful emit.
//    2. rec_super_call -> the PSX body emits its prim(s) into the pool.
//    3. If the descriptor is EXPLICIT (CLUT != 0): record it as the last-bound CLUT and return —
//       the faithful path already stored it; we touch nothing.
//    4. If the descriptor INHERITS (CLUT == 0) AND we have a remembered last-bound CLUT: rewrite
//       the +0xE halfword of every prim the body just emitted (pool range [before, after), stride
//       0x2C) with that CLUT. We ONLY enter this branch for the inherit descriptor, and ONLY when
//       a real CLUT has been seen, so the normal textured-sprite path is never altered and the
//       latent (currently non-reproducing) case is a safe no-op until a real CLUT is in flight.
//
//  This is a faithful repair of a stateless-submit gap, not a behavior change: explicit prims are
//  byte-identical to the super-call output.
//
//  Wiring (do in game_tomba2.cpp's override-install block, NOT here):
//      void ov_ui_rect_emit(Core*);                 // forward decl
//      rec_set_override(0x8007e1b8u, ov_ui_rect_emit);
//  and add engine/engine_ui_rect.cpp to the SRC lists in tools/build_port.sh AND run.sh.

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);

namespace {

constexpr uint32_t POOL_HEAD = 0x800BF544u;  // OT-pool write head (advanced by the emit tail)
constexpr uint32_t PRIM_STRIDE = 0x2Cu;      // 11 words/prim in the pool (1 link tag + 9 data + 1 pad)
constexpr uint32_t PRIM_CLUT_OFF = 0xEu;     // CLUT halfword within a pool prim (template +0xC high half)

// Last EXPLICIT CLUT this emitter stored, i.e. the "last-bound" CLUT a PSX inherit prim would reuse.
// File-static so it persists across the many per-frame calls (HUD digits, menu glyphs, panels, ...).
// Seeded 0 == "nothing bound yet" -> inherit prims are left untouched (safe no-op).
uint16_t s_last_clut = 0;

}  // namespace

// FUN_8007e1b8 override — see the RE block above.
void ov_ui_rect_emit(Core* c) {
  // Descriptor pointer is a3 (== c->r[7]); decode the CLUT field before the body runs.
  uint32_t desc = c->r[7];
  uint16_t desc2 = c->mem_r16(desc + 2);   // desc+2: bit15 = SEMI, bits0..14 = CLUT (0 == inherit)
  uint16_t clut_field = (uint16_t)(desc2 & 0x7FFFu);

  // Snapshot the pool head so we can find exactly the prim(s) the body emits.
  uint32_t pool_before = c->mem_r32(POOL_HEAD);

  // Faithful PSX emit (builds + links the prim(s) into the OT pool).
  rec_super_call(c, 0x8007e1b8u);

  if (clut_field != 0u) {
    // EXPLICIT descriptor: the body already stored this CLUT at prim+0xE. Remember it as the
    // last-bound CLUT for subsequent inherit prims; do NOT touch the emitted prim (byte-identical).
    s_last_clut = clut_field;
    return;
  }

  // INHERIT descriptor (CLUT == 0). The body skipped the +0xE store, so the emitted prim(s) carry a
  // stale CLUT. Re-stamp them with the last-bound CLUT — the value the PSX GP0 latch would have held.
  // No-op (and the latent #6 case stays harmless) until a real CLUT has actually been bound.
  if (s_last_clut == 0u) return;

  uint32_t pool_after = c->mem_r32(POOL_HEAD);
  if (pool_after <= pool_before) return;                 // nothing emitted (defensive)
  uint32_t span = pool_after - pool_before;
  // Guard: only rewrite if the span is a whole number of prim slots (it always is for this emitter);
  // bail otherwise rather than risk smearing the CLUT across unrelated pool words.
  if (span % PRIM_STRIDE != 0u) return;

  for (uint32_t p = pool_before; p + PRIM_CLUT_OFF + 1u <= pool_after; p += PRIM_STRIDE) {
    c->mem_w16(p + PRIM_CLUT_OFF, s_last_clut);
  }

  if (cfg_dbg("uirect")) {
    fprintf(stderr, "[uirect] inherit prim re-clut: range [%08x,%08x) n=%u clut=%04x\n",
            pool_before, pool_after, span / PRIM_STRIDE, s_last_clut);
  }
}
