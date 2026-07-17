// game/render/quad_rtpt_submit.cpp — see quad_rtpt_submit.h. Faithful substrate-mirror bodies of
// FUN_8003B054 and FUN_8003B320, RE'd instruction-by-instruction from the recompiler's own
// translation (generated/shard_3.c gen_func_8003B054, generated/shard_6.c gen_func_8003B320 —
// ground truth per CLAUDE.md for GTE-bearing code; Ghidra's COP2 decompile of FUN_8003B320 renders
// the GTE data-register writes as synthetic setCopReg/getCopReg/copFunction "bus" pseudo-calls that
// do not resolve to plain register indices, so it was cross-checked against, not relied on, for the
// GTE portion — FUN_8003B054 has no GTE so Ghidra's decompile of it was already reliable and is
// reproduced 1:1 below).
//
// WIRED + SBS-gated 2026-07-08. Two bugs found by re-diffing the draft against gen_func_8003B320
// and fixed here:
//   (1) the on-screen test was `&&` (ALL 4 corners in range) — ground truth is `||` (ANY corner in
//       range; it jumps to "keep" the instant one corner passes, only drops if all 4 fail), same
//       convention as OverlayGt3Gt4/OverlayGroundGt3Gt4's "any1"/"any2" gates.
//   (2) the real `addiu sp,-16` guest stack frame (pure scratch: FLAG/z0/otz working values) was
//       not mirrored at all — fixed per CLAUDE.md's "MIRROR THE GUEST STACK" directive.
// Also added the NCLIP call gen_func_8003B320 performs between RTPT and the 4th-corner RTPS —
// its only output (MAC0) is provably clobbered by the RTPS flag store before ever being read, so
// it has zero effect on any surviving register/RAM byte, but it's a real executed op and this
// leaf's contract is op-exact transcription, not "rebuild for observable result" (that pc_render
// rule does not apply to this render-UNDERNEATH substrate mirror).
#include "quad_rtpt_submit.h"
#include "game_ctx.h"
#include "core.h"
#include "game.h"
#include "cfg.h"
#include "render_internal.h"   // cur_render_node — WqRec identity
#include "render_queue.h"      // RenderQueue::emitOrQueue + RQ_WORLD/RQ_OM_DEPTH
#include <cstdint>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8003B054 — rotate the 4 corner fields of a quad record from `src` into `dst`'s reserved
// vertex-index/extent slots (+0xC/+0x14/+0x1C/+0x24, each a u16; a shared 2nd word at +0xE/+0x16
// filled from src+2/src+6 in the SAME order for every non-zero index). `idx` selects which
// physical corner of `src` becomes dst's "first" corner (a cyclic rotation) and, for idx 1..3,
// applies a small "-1" shrink to specific BYTES of the 4 written u16s (idx1: low byte of all 4;
// idx2: high byte of all 4; idx3: both bytes of all 4) — idx 0 does no byte adjustment and writes
// all 4 fields as FULL 32-bit words (src's next word, not just its low u16) with an early return
// that SKIPS the shared +0xE/+0x16 tail (traced exactly from gen_func_8003B054's control flow —
// this asymmetry is real, not an RE artifact: idx0 is qualitatively different from 1/2/3).
void QuadRtptSubmit::rotateQuadCorners(Core* c) {
  const uint32_t dst = c->r[4];   // a0
  const uint32_t src = c->r[5];   // a1
  const int32_t  idx = (int32_t)c->r[6];   // a2: corner/orientation selector

  if (idx == 1) {
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 4));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 0));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 8));
    c->mem_w8(dst + 0x0C, (uint8_t)(c->mem_r8(dst + 0x0C) - 1));   // low byte of each corner -1
    c->mem_w8(dst + 0x14, (uint8_t)(c->mem_r8(dst + 0x14) - 1));
    c->mem_w8(dst + 0x1C, (uint8_t)(c->mem_r8(dst + 0x1C) - 1));
    c->mem_w8(dst + 0x24, (uint8_t)(c->mem_r8(dst + 0x24) - 1));
  } else if (idx < 2) {
    if (idx != 0) return;
    c->mem_w32(dst + 0x0C, c->mem_r32(src + 0));
    c->mem_w32(dst + 0x14, c->mem_r32(src + 4));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 12));
    return;   // idx==0 skips the shared tail below — faithful to gen_func_8003B054
  } else if (idx == 2) {
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 0));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 4));
    c->mem_w8(dst + 0x0D, (uint8_t)(c->mem_r8(dst + 0x0D) - 1));   // high byte of each corner -1
    c->mem_w8(dst + 0x15, (uint8_t)(c->mem_r8(dst + 0x15) - 1));
    c->mem_w8(dst + 0x1D, (uint8_t)(c->mem_r8(dst + 0x1D) - 1));
    c->mem_w8(dst + 0x25, (uint8_t)(c->mem_r8(dst + 0x25) - 1));
  } else {
    if (idx != 3) return;
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 4));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 0));
    // both bytes of each of the 4 corners -1 (two independent byte-decrements each, NOT a u16 -=1 —
    // matches gen_func_8003B054's per-byte store order exactly, borrow behaviour included).
    c->mem_w8(dst + 0x0C, (uint8_t)(c->mem_r8(dst + 0x0C) - 1));
    c->mem_w8(dst + 0x0D, (uint8_t)(c->mem_r8(dst + 0x0D) - 1));
    c->mem_w8(dst + 0x14, (uint8_t)(c->mem_r8(dst + 0x14) - 1));
    c->mem_w8(dst + 0x15, (uint8_t)(c->mem_r8(dst + 0x15) - 1));
    c->mem_w8(dst + 0x1C, (uint8_t)(c->mem_r8(dst + 0x1C) - 1));
    c->mem_w8(dst + 0x1D, (uint8_t)(c->mem_r8(dst + 0x1D) - 1));
    c->mem_w8(dst + 0x24, (uint8_t)(c->mem_r8(dst + 0x24) - 1));
    c->mem_w8(dst + 0x25, (uint8_t)(c->mem_r8(dst + 0x25) - 1));
  }

  // shared tail (idx 1/2/3 only): second word's two halves, UNROTATED (always src+2/src+6).
  c->mem_w16(dst + 0x16, c->mem_r16(src + 6));
  c->mem_w16(dst + 0x0E, c->mem_r16(src + 2));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8003B320 — project a quad through an already-composed GTE transform (RTPT the first 3
// corners, RTPS the 4th) and, if it survives the on-screen + OT-range gates, bump-copy the
// pre-built 10-word packet record into the packet pool and link it into the OT bucket for its
// depth. Traced from gen_func_8003B320 (generated/shard_6.c) — same gte_op idiom as the already-
// owned OverlayGt3Gt4::gt3/gt4 (game/render/overlay_gt3gt4.cpp), which this mirrors closely: RTPT
// via 0x4A280030, an intervening NCLIP via 0x4B400006 (see below), RTPS via 0x4A180001, AVSZ4 via
// 0x4B68002E, OTZ-bucket compute identical to overlay_gt_otz_index (z>>10 exponent-shift index,
// valid range [4,0x7ff]).
//
// NCLIP: gen_func_8003B320 calls it between the RTPT and the VXY3/RTPS setup, storing its MAC0
// result (data reg 24) to the same 4-byte FLAG scratch slot the RTPT/RTPS flag checks use — but
// that slot is unconditionally overwritten by the POST-RTPS flag store before anything ever reads
// it back (traced instruction-by-instruction: no branch, no other read, in between). So the call
// is REAL (an actually-executed GTE op) but its only output is provably dead on every path. It is
// reproduced anyway (not "rebuilt away") because this leaf's contract is op-exact transcription of
// the substrate (see quad_rtpt_submit.h) — the pc_render "rebuild for observable result" rule does
// not apply to a render-underneath substrate mirror. A prior draft of this file both omitted the
// call and mis-documented it as absent ("no NCLIP/backface test here"); fixed 2026-07-08.
//
// Real 16-byte guest stack frame (RE: `addiu sp,-16`, no saved registers — pure scratch: +0 = the
// FLAG scratch shared by all three flag checks, +4 = the raw AVSZ4 z0, +8 = the OTZ working value
// through its bias-add / shift-recombine / range-gate stages) MIRRORED per CLAUDE.md ("MIRROR THE
// GUEST STACK... never revert/exclude a leaf because it pushes a frame") — a prior draft left this
// frame entirely unmirrored (host C++ locals only, no c->r[29] descent at all).
//
// out            (a0): 10-word (40-byte) packet record. +4/+12/+20/+28/+36 = colour/uv/etc already
//                       filled in by the caller (verbatim-copied, this leaf never reads their
//                       meaning); +8/+16/+24/+32 = the 4 SXY slots THIS leaf fills via RTPT/RTPS.
// composedXform  (a1): 6 packed words = GTE VXY0/VZ0/VXY1/VZ1/VXY2/VZ2 (MTC2-ready model-space
//                       corner data for the RTPT), plus 2 more words at +24/+28 = VXY3/VZ3 for the
//                       RTPS 4th corner. The caller composes this (un-RE'd, outside this band —
//                       see quad_rtpt_submit.h); this leaf only consumes it.
// otzBias        (a2): added to the AVSZ4 result before the OT-bucket index is derived.
void QuadRtptSubmit::submitQuad(Core* c) {
  const uint32_t out = c->r[4];              // a0
  const uint32_t xf  = c->r[5];               // a1: composedXform
  const int32_t  otzBias = (int32_t)c->r[6];  // a2
  if (cfg_dbg("quadrtpt")) { static long n = 0; if (n++ % 512 == 0) cfg_logf("quadrtpt", "submitQuad call#%ld", n); }
  // CR-CONTRACT PROBE (temporary, cfg-gated — REDIRECT census, docs/fps60-rework.md): is the GTE
  // transform this quad projects under the PURE SCENE CAMERA (CR0-4 == scratchpad CAM_ROT 0x1F8000F8,
  // CR5-7 == CAM_TRANS 0x1F80010C) or a composed per-object transform? Decides whether this caller
  // class can be display-pass re-projected through the float camera (like billboardsRender).
  if (cfg_dbg("quadcr")) {
    bool rotEq = true, trEq = true;
    for (unsigned i = 0; i < 5; i++) if (gte_read_ctrl(i) != c->mem_r32(0x1F8000F8u + i * 4u)) { rotEq = false; break; }
    for (unsigned i = 0; i < 3; i++) if (gte_read_ctrl(5 + i) != c->mem_r32(0x1F80010Cu + i * 4u)) { trEq = false; break; }
    static long np = 0; if ((np++ & 63) == 0)
      cfg_logf("quadcr", "#%ld ra=%08X rotEq=%d trEq=%d node=%08X", np, c->r[31], rotEq, trEq, cur_render_node(c));
  }

  c->r[29] -= 16;
  const uint32_t sp = c->r[29];
  auto pop = [&] { c->r[29] += 16; };

  gte_write_data(0, c->mem_r32(xf + 0));      // VXY0
  gte_write_data(1, c->mem_r32(xf + 4));      // VZ0
  gte_write_data(2, c->mem_r32(xf + 8));      // VXY1
  gte_write_data(3, c->mem_r32(xf + 12));     // VZ1
  gte_write_data(4, c->mem_r32(xf + 16));     // VXY2
  gte_write_data(5, c->mem_r32(xf + 20));     // VZ2
  gte_op(c, 0x4A280030u);                     // RTPT (corners 0..2)
  c->mem_w32(sp + 0, gte_read_ctrl(31));
  if ((int32_t)c->mem_r32(sp + 0) < 0) { c->mem_w32(sp + 8, (uint32_t)-1); pop(); return; } // GTE FLAG error -> drop

  c->mem_w32(out + 8,  gte_read_data(12));    // SXY0
  c->mem_w32(out + 16, gte_read_data(13));    // SXY1
  c->mem_w32(out + 24, gte_read_data(14));    // SXY2

  gte_op(c, 0x4B400006u);                     // NCLIP (real, provably dead output — see banner)
  c->mem_w32(sp + 0, gte_read_data(24));      // MAC0 (clobbered below before ever being read)

  gte_write_data(0, c->mem_r32(xf + 24));     // VXY3
  gte_write_data(1, c->mem_r32(xf + 28));     // VZ3
  gte_op(c, 0x4A180001u);                     // RTPS (corner 3)
  c->mem_w32(sp + 0, gte_read_ctrl(31));
  if ((int32_t)c->mem_r32(sp + 0) < 0) { c->mem_w32(sp + 8, (uint32_t)-1); pop(); return; } // GTE FLAG error -> drop
  c->mem_w32(out + 32, gte_read_data(14));    // SXY3

  gte_op(c, 0x4B68002Eu);                     // AVSZ4
  c->mem_w32(sp + 4, gte_read_data(7));
  int32_t z0 = (int32_t)c->mem_r32(sp + 4);
  c->mem_w32(sp + 8, (uint32_t)z0);
  if (z0 < 0) { pop(); return; }               // raw AVSZ4 error -> drop (checked BEFORE bias, faithful)
  int32_t z = z0 + otzBias;
  c->mem_w32(sp + 8, (uint32_t)z);
  if (z < 0) { pop(); return; }                // biased z still must be non-negative

  int32_t shift = z >> 10;
  int32_t otz = (z >> (shift & 31)) + shift * 0x200;
  c->mem_w32(sp + 8, (uint32_t)otz);
  if (otz < 4 || otz > 0x7FF) { pop(); return; } // faithful range gate: valid index is [4, 0x7FF]

  // on-screen test: ANY of the 4 corners' SX in [0,320) (unsigned 16-bit compare — a negative/
  // wrapped coordinate fails), then ANY corner's SY in [0,240) — an OR gate, not AND (FIX
  // 2026-07-08: a prior draft used && here, dropping quads the substrate keeps whenever fewer
  // than all 4 corners were on-screen; ground truth gen_func_8003B320 jumps to "keep" the instant
  // one corner passes, same "any1"/"any2" convention as OverlayGt3Gt4/OverlayGroundGt3Gt4).
  auto sx = [&](uint32_t off) { return (uint16_t)c->mem_r16(out + off); };
  // xmax: 320 stock; the wide width under the genuine engine-wide FOV (submit.cpp submit_xmax
  // precedent — right-band content was culled out of widescreen; SBS legs run 4:3, unaffected).
  int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
  const uint16_t xmax = gpu_gpu_wide_engine(c) ? (uint16_t)gpu_gpu_wide_engine_w(c) : 320;
  bool xok = sx(8) < xmax || sx(16) < xmax || sx(24) < xmax || sx(32) < xmax;
  if (!xok) { pop(); return; }
  bool yok = sx(10) < 240 || sx(18) < 240 || sx(26) < 240 || sx(34) < 240;
  if (!yok) { pop(); return; }

  // bump-copy the whole 10-word record into the packet pool, OT-link it.
  const uint32_t POOL_PTR = 0x800BF544u;
  const uint32_t OT_BASE_PTR = 0x800ED8C8u;
  uint32_t pool = c->mem_r32(POOL_PTR);
  uint32_t otbase = c->mem_r32(OT_BASE_PTR);
  uint32_t slot = otbase + (uint32_t)otz * 4;
  uint32_t old_head = c->mem_r32(slot);
  c->mem_w32(pool, old_head | (9u << 24));    // tag: len=9 data words | old OT head
  c->mem_w32(slot, pool);                     // OT bucket head = this packet
  uint32_t dstw = pool + 4;
  for (uint32_t off = 4; off <= 36; off += 4, dstw += 4)
    c->mem_w32(dstw, c->mem_r32(out + off));
  c->mem_w32(POOL_PTR, pool + 40);

  // #67 (replaces the #65 DUAL-EMIT, deleted break-first): RECORD this surviving quad for the
  // display-pass producer Render::billboardsRender instead of pushing the GTE SXYs verbatim. The
  // MODEL corners are the xf words this leaf just projected; the composed GTE transform is FACTORED
  // against the scratchpad scene camera (pure at this point — per-object composes touch only the GTE
  // CRs) into a WORLD transform, so the display pass re-composes it with the (fps60-lerped) camera:
  // real and interpolated frames derive the quad identically from state (render.h WqRec banner).
  // Host memory only; the guest packet-pool copy above is untouched.
  if (!c->game->oracle) {
    Render::WqRec w;
    w.node = cur_render_node(c);
    // Per-node emission index this frame = stable lerp identity (emit order is deterministic).
    // Derived from the frame's own record list — no per-Core static state (SBS-safe).
    w.seq = 0;
    for (const Render::WqRec& p : rend(c)->mWqRecs) if (p.node == w.node) w.seq++;
    for (int i = 0; i < 4; i++) {
      const uint32_t vxy = c->mem_r32(xf + (uint32_t)(i < 3 ? i * 8 : 24));
      const uint32_t vzw = c->mem_r32(xf + (uint32_t)(i < 3 ? i * 8 + 4 : 28));
      w.vx[i] = (int16_t)vxy; w.vy[i] = (int16_t)(vxy >> 16); w.vz[i] = (int16_t)vzw;
    }
    // Factor the live GTE CR0-7 against the scratchpad camera (render_internal.h helpers).
    { constexpr float FX = 1.0f / 4096.0f;
      float crF[3][3], tr[3];
      uint32_t g0 = gte_read_ctrl(0), g1 = gte_read_ctrl(1), g2 = gte_read_ctrl(2),
               g3 = gte_read_ctrl(3), g4 = gte_read_ctrl(4);
      crF[0][0] = (int16_t)g0 * FX;         crF[0][1] = (int16_t)(g0 >> 16) * FX; crF[0][2] = (int16_t)g1 * FX;
      crF[1][0] = (int16_t)(g1 >> 16) * FX; crF[1][1] = (int16_t)g2 * FX;         crF[1][2] = (int16_t)(g2 >> 16) * FX;
      crF[2][0] = (int16_t)g3 * FX;         crF[2][1] = (int16_t)(g3 >> 16) * FX; crF[2][2] = (int16_t)g4 * FX;
      for (int i = 0; i < 3; i++) tr[i] = (float)(int32_t)gte_read_ctrl(5u + (unsigned)i);
      wq_factor_world(c, crF, tr, w.objR, w.objT);
    }
    { const uint32_t col = c->mem_r32(out + 4);
      for (int i = 0; i < 4; i++) w.wCol[i] = col; }
    w.wUv0 = c->mem_r32(out + 12); w.wUv1 = c->mem_r32(out + 20);
    w.wUv2 = c->mem_r32(out + 28); w.wUv3 = c->mem_r32(out + 36);
    rend(c)->mWqRecs.push_back(w);
    { static long np = 0; if ((np++ & 255) == 0)
        cfg_logf("quadrtpt", "rec #%ld node=%08X seq=%u", np, w.node, w.seq); }
  }
  pop();                                       // ascend the real 16-byte frame
}

// Wiring (frontier, 2026-07-08): both leaves are reached only via direct C calls the recompiler
// generates (`func_8003B054(c)`/`func_8003B320(c)`), which always route through the recompiler's
// own process-global g_override[] table. engine_set_override_main (runtime/recomp/override_registry.h)
// installs into the ONE process-global override registry, which runs gen_func_* on the oracle leg
// (core B) and the native handler everywhere else — NOT a raw shard_set_override, since these are
// engine/game natives and the oracle must run the pure recompiled body.
void QuadRtptSubmit::registerOverrides(Game*) {
  extern void gen_func_8003B054(Core*);
  extern void gen_func_8003B320(Core*);
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003B054u, &QuadRtptSubmit::rotateQuadCorners, gen_func_8003B054);
  engine_set_override_main(0x8003B320u, &QuadRtptSubmit::submitQuad,        gen_func_8003B320);
}
