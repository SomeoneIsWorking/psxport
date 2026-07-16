// game/render/tile_grid_layer.cpp — implementation + RE trace for the field's scrolling 16x16
// tile-grid background layer. See tile_grid_layer.h for the ownership/wiring summary and
// docs/native-render-2d-tilegrid.md for the original (partially superseded) RE writeup.
//
// ===================================================================================================
// ENTRY-POINT RESOLUTION (task STEP 0) — 0x80115364 is NOT a real function entry
// ===================================================================================================
// Ground truth checked three independent ways, all agreeing:
//  1. Live disas (`tools/disas.py --ram scratch/decomp/overlay_2d/freeroam.ram 0x8011534c --all`):
//     the word AT 0x80115364 is `27bdfff8` = `addiu sp,sp,-8` — the DELAY SLOT of the `beq v1,t2,
//     0x80115480` branch at 0x80115360, inside the function whose real prologue starts at
//     0x8011534C (`addu a3,a0,zero; lui v0,0x800f; addiu t2,zero,1; lbu v1,0(a3); lw t0,-12412(v0)`).
//     0x80115364 is reached naturally as straight-line fallthrough of 0x8011534C — it is NOT a
//     separate function.
//  2. The recompiler's OWN A00-overlay function splitter (generated/ov_a00_disp.c, built from a
//     ROM-wide static analysis independent of this investigation) has real switch cases + `idx()`
//     entries for 0x8011534C, 0x80115598 and 0x801158E0 — and NO case anywhere for 0x80115364.
//     Dispatching rec_dispatch(c, 0x80115364u) while A00 is resident would fall to ov_a00_dispatch's
//     `default:` -> rec_dispatch_miss -> abort.
//  3. generated/shard_7.c:4639 (gen_func_8003D0BC, i.e. Render::overlayTypeDispatch's substrate
//     body) really does contain `rec_dispatch(c, 0x80115364u);` for case 0x8003D1C4 — so this
//     literal, broken-looking call target IS a genuine artifact of the original ROM (some original
//     FUN_8003D1C4 did `j 0x80115364`, a same-region tail-continuation the ORIGINAL compiler treated
//     as reachable, that the recompiler's A00-side splitter never registered as an external entry).
//     Since AREA_TYPE case 0x8003D1C4 corresponds to whatever area-type index #13 in the 22-entry
//     jump table is (NOT area-type 0, the field's own type — see Engine::areaModeDispatch below,
//     whose idx-0 handler is a *different*, real address: 0x8011534C), and since "recomp_path runs
//     perfectly" is the project's own invariant (CLAUDE.md), this call is either genuinely never
//     reached by any live AREA_TYPE value the field ever sets 0x800BF870 to, or reached only while a
//     DIFFERENT overlay (not A00) is resident in the MODE slot (in which case 0x80115364 lands on
//     THAT overlay's own, unrelated code at the same offset). Either way it is NOT the field
//     tile-grid's call path — do not chase it further; the doc's "Function boundaries" open question
//     is answered: 0x8011534C is the one true entry, 0x80115364 is dead/unrelated for this feature.
//
// The REAL, LIVE call graph for the field's tile-grid layer (found by grepping for existing native
// callers, not by walking the dispatch tree further):
//   - Engine::areaModeDispatch / areaModeDispatchFaithful (game/core/engine.cpp, guest 0x8001CAC0,
//     the 22-way per-area-mode STEP dispatcher keyed on the SAME byte 0x800BF870 overlayTypeDispatch
//     reads) — mode idx 0's handler is `0x8011534Cu` directly, called with a0=0x800ED018. This is
//     the scroll-wrap STEP, run once per frame while the field's AREA_TYPE/mode is 0.
//   - FUN_8003DF04 (generated/shard_4.c gen_func_8003DF04, still SUBSTRATE/unowned) — a render-state
//     dispatcher; case state==0 calls `rec_dispatch(c, 0x80115598u)` with a0=0x800ED018 (computed as
//     (32783<<16)-12264 in the generated C, which is exactly 0x800ED018). This is the EMIT call,
//     already independently identified and native-host-ported as Render::backdropRender's own
//     banner ("reached via 0x8003df04's 16-state jump table @0x80014fc0; state 0 -> 0x8003df74 ->
//     0x80115598", render_walk.cpp).
// Both callers pass the SAME node address (0x800ED018 — "the seaside field's" tile-grid state,
// matching render_walk.cpp's own struct-layout comment), confirming this is one coherent object.
//
// ===================================================================================================
// DRIVER 0x801158E0 (task STEP 1) — RE'd, but NOT the caller of the above pair; documented + dropped
// ===================================================================================================
// RE'd via `tools/disas.py --ram ... 0x801158e0 --all 220`
// (scratch/decomp/overlay_2d/801158e0.disas.txt). Real entry (`addiu sp,-48`, spills s0/s1/s2/ra —
// matches the original RE doc's boundary guess), a0=node, 4-way dispatch on node+4 (u8):
//   node[4]==1                    -> 0x80115aa4 (not traced further — out of scope, see below)
//   node[4]==0                    -> 0x80115934: gated on a GLOBAL style/mode byte at 0x800BF9E0
//                                    (`< 28`), then walks a FIXED 28-entry area-record table at
//                                    0x80146f0c (each record: 2 header shorts + 3 more shorts + a
//                                    variable tail, `lh v0,0(s0); bgez...`), calling `jal 0x8003116c`
//                                    per matching record (id compare against node[5]) to spawn/attach
//                                    something (return value gates a `sb`/`sb` write to the spawned
//                                    object's +3 byte) — reads like an AREA ENTITY/PARTICLE SPAWNER
//                                    for whatever node[5] selects, unrelated to tile rendering.
//   node[4]==2 or 3                -> 0x80115b68 (not traced further)
//   node[4]>=4 (or after the above) -> 0x80115b70; the function continues well past 0x80115c4c
//                                    (a per-frame countdown/animation-index decrement block reading
//                                    a DIFFERENT record table at 0x80147d84, unrelated to 0x800ED018).
// Critically: this function's body (disassembled through 0x80115c4c, ~220 instructions) contains NO
// jal/rec_dispatch to 0x8011534C or 0x80115598 anywhere — it does not call either leaf. And grepping
// every generated shard (both MAIN's shard_*.c and A00's own ov_a00_shard_*.c) for a literal
// `rec_dispatch(c, 0x801158E0u)` / `jal 0x801158e0` finds NO caller at all while A00 is resident (the
// only hits are OTHER overlays reusing the same numeric address for unrelated code, e.g. A01/A05/A08).
// So the original RE doc's guess ("the caller/driver that owns the node and decides WHEN this
// emitter runs") is WRONG: 0x801158E0 is a genuine, separate object-type's per-frame state machine
// (entity/particle spawn + animation countdown) that happens to sit adjacent to the tile-grid pair in
// the A00 overlay's code layout, almost certainly reached via a per-object function-pointer table for
// SOME OTHER node kind, not this one. It is out of scope here — not owned, not wired, left for
// whoever RE's that other object type.
//
// ===================================================================================================
// FUN_8011534C — scroll-wrap helper (RE, exact byte offsets from the live disas)
// ===================================================================================================
// Real frame: `addiu sp,-8`, no register spills, no stack memory ops anywhere in the body (the 8B is
// allocated but unused scratch — mirror sp only, no guest stack WRITE needed).
// node[0] (u8) state: 0 = one-time init (below), 1 = per-frame scroll-wrap recompute, other = no-op.
// State 0 (init, only ever runs once — the body itself advances state to 1):
//   srcRecord = *0x800ECF84 (a fixed global pointer to a 20+-byte configuration record)
//   node[0]=1; node[3]=0
//   node[4],[6],[8],[10],[12],[14] (6x u16) = srcRecord[0],[2],[4],[6],[8],[10]   (verbatim copy)
//   node[16] (u8 grid W) = srcRecord[12]; node[17] (u8 grid H) = srcRecord[13]
//   node[0x2C] = 2304 (fixed); node[0x2E] = 2280 (fixed) — NOT W/H-derived, literal constants
//   node[0x14] (tile-ID table ptr) = (srcRecord+20) + srcRecord.u16[16]
//   node[0x18] = srcRecord+20;  node[0x1C] = node[0x18] + srcRecord.u16[14];  node[0x34] = node[0x1C]
//   node[0x30] = (((2304*W) * 0x38e38e39) >> 32) >> 5      [signed 32x32->64 mult, hi32, then >>5 —
//   node[0x32] = (((2280*H) * 0x38e38e39) >> 32) >> 5       a fixed-point "multiply by ~0.222" idiom
//                                                            that numerically resolves to ~16*W/16*H,
//                                                            i.e. the tile grid's pixel pitch]
//   node[0x38] = 1  (armed/re-init countdown seed)
// State 1 (per-frame wrap, RE'd fully from 0x80115480-0x8011558c):
//   dX = s16 mem16(0x1F8000F2); kX = node.u16[0x2C] (== 2304)
//   wrapX = ((kX+320)>>1) - ((dX*kX)>>12); wrap into [0, node.u16[0x30])
//   dY = s16 mem16(0x1F8000F0); kY = node.u16[0x2E] (== 2280); h8 = node.u8[17]<<3   (H<<3)
//   wrapY = ((dY*kY)>>12) + h8 - 32; wrap into [0, node.u16[0x32])
//   node[0x38] -= 1 (u8); if ((s8)node[0x38] <= 0) node[3] = 1
//   node[0x28] = wrapX (u16);  node[0x2A] = wrapY (u16)
// (The self-referencing `bltz`/`beq` wrap loops in the disas — e.g. `801154bc: bltz a1,0x801154bc`
// with its own delay slot re-adding the same amount — are the standard MIPS-GCC compiled form of a
// plain `while (x < 0) x += n;` loop; the trailing `subu` right after each loop cancels the
// delay-slot's one extra add on the exiting iteration. Net effect: a plain wrap loop, ported as such.)
//
// ===================================================================================================
// FUN_80115598 — tile-grid sprite-packet emitter (RE, exact byte offsets from the live disas)
// ===================================================================================================
// Real frame: `addiu sp,-80`, spills ra/fp/s7/s6/s5/s4/s3/s2/s1/s0 at sp+76..+40 (10 words) —
// mirrored below per CLAUDE.md's guest-stack rule.
// Row/col wrap-window math is IDENTICAL to the already-ported, already-verified
// Render::backdropRender (render_walk.cpp) — that function's own banner documents it as a literal
// transcription of this exact leaf's integer math, and it has years of RMSE-driven verification
// behind it (issue #60 etc.). Reused here (not re-derived) for the geometry; this file adds the
// GUEST packet writes backdropRender never had to do (it is host-only).
// Per-visible-tile packet (16 bytes @ `thisAddr`, corrected from the original RE doc's guess about
// the OT-splice mechanism — see below):
//   +20 (u32) = 0x7D808080                    (rgb0|code template)
//   +23 (u8)  = 0x7C                            (patches the template's top byte -> SPRITE-16x16 cmd)
//   +19 (u8)  = 3                                (fixed per-tile flag/size byte)
//   +24 (u32) = X(u16, low) | Y(u16, high)        (screen-space tile position)
//   +28 (u16) = ((tile&0xF)<<4) | (((tile&0xF0)+8)<<8)   (packed atlas U | V<<8; +8 = field V-bias)
//   +30 (u16) = clutBase + ((tile&0xF00)>>2)
//   +0  (u32) = (thisAddr+16) | 0x03000000        (tag/next: EVERY tile chains forward to the very
//               next tile's own address — written using the pool pointer which is bumped to
//               `thisAddr+16` just before this store; NOT a "one-iteration-late" scheme as the
//               original doc guessed — each tile sets its own forward-chaining tag directly)
// After the double loop: the LAST tile's tag is a stray pointer to one-past-the-final-tile (since
// every tile, including the last, always computes "next = pool-after-me" the same way) — it gets
// PATCHED: keep the top length byte, OR in the OLD OT[0x7FF] head (mem32(0x800ED8C8) then
// mem32(that+0x1FFC)), completing the chain into whatever was already queued in that bucket.
// CORRECTION vs the original RE doc: `jal 0x80083de0` (func_80083DE0, already RE'd as a libgpu
// draw-mode/texwin PACKET-HEADER BUILDER in wide_re_libgpu_leaves.cpp, NOT an OT-splice helper) does
// NOT perform the OT link itself — it only fills in a TRAILING packet's mode/texwin fields (called
// here with rgbBitsSrc=0, modeFlag=0, texWinSrc=0 -> a plain 0xE1000000 DR_TPAGE reset word, texwin
// word 0). The REAL OT[0x7FF] splice is separate, inline code straight after the call: the new
// header packet's own tag is set to `first_tile | 0x02000000` (chaining forward into the tile batch)
// and OT[0x7FF] is overwritten with the header packet's own address — i.e. this leaf PREPENDS one
// mode-reset packet + the whole tile batch onto whatever was already in bucket 0x7FF.
#include "core.h"
#include "game.h"
#include "render.h"
#include "render_queue.h"
#include "cfg.h"
#include "tile_grid_layer.h"
#include <cstdint>

namespace {
constexpr uint32_t kPktPoolPtr = 0x800BF544u;   // shared bump-allocator pool ptr (same as overlay_ground_gt3gt4.cpp)
constexpr uint32_t kOtBaseGlobal = 0x800ED8C8u; // shared OT-base global
constexpr uint32_t kOtBucketBg = 0x1FFCu;       // OT bucket 0x7FF (background layer, static depth)
}  // namespace

// FUN_8011534C
void TileGridLayer::scrollStep(Core* c) {
  const uint32_t node = c->r[4];
  c->r[29] -= 8;   // real 8B frame; no spills, no stack memory ops in the body (mirror sp only)

  const uint8_t state = c->mem_r8(node + 0u);
  if (state == 1u) {
    int32_t dX = c->mem_r16s(0x1F8000F2u);
    const uint16_t kX = c->mem_r16(node + 0x2Cu);
    int32_t wrapX = (((int32_t)kX + 320) >> 1) - ((dX * (int32_t)kX) >> 12);
    const uint16_t pitchX = c->mem_r16(node + 0x30u);
    while (wrapX < 0) wrapX += pitchX;
    while (wrapX >= (int32_t)pitchX) wrapX -= pitchX;

    int32_t dY = c->mem_r16s(0x1F8000F0u);
    const uint16_t kY = c->mem_r16(node + 0x2Eu);
    const int32_t h8 = (int32_t)c->mem_r8(node + 17u) << 3;
    int32_t wrapY = ((dY * (int32_t)kY) >> 12) + h8 - 32;
    const uint16_t pitchY = c->mem_r16(node + 0x32u);
    while (wrapY < 0) wrapY += pitchY;
    while (wrapY >= (int32_t)pitchY) wrapY -= pitchY;

    const uint8_t countdown = (uint8_t)(c->mem_r8(node + 0x38u) - 1u);
    c->mem_w8(node + 0x38u, countdown);
    if ((int8_t)countdown <= 0) c->mem_w8(node + 3u, 1u);

    c->mem_w16(node + 0x28u, (uint16_t)wrapX);
    c->mem_w16(node + 0x2Au, (uint16_t)wrapY);
  } else if (state == 0u) {
    const uint32_t src = c->mem_r32(0x800ECF84u);
    c->mem_w8(node + 0u, 1u);
    c->mem_w8(node + 3u, 0u);
    for (int i = 0; i < 6; i++)
      c->mem_w16(node + 4u + (uint32_t)i * 2u, c->mem_r16(src + (uint32_t)i * 2u));
    const uint8_t gridW = c->mem_r8(src + 12u);
    const uint8_t gridH = c->mem_r8(src + 13u);
    c->mem_w8(node + 16u, gridW);
    c->mem_w8(node + 17u, gridH);
    c->mem_w16(node + 0x2Cu, 2304u);
    c->mem_w16(node + 0x2Eu, 2280u);
    const uint16_t f14 = c->mem_r16(src + 14u), f16 = c->mem_r16(src + 16u);
    const uint32_t rec18 = src + 20u;
    const uint32_t rec14 = rec18 + f16;
    const uint32_t rec1c = rec18 + f14;
    c->mem_w32(node + 0x14u, rec14);
    c->mem_w32(node + 0x18u, rec18);
    c->mem_w32(node + 0x1Cu, rec1c);
    c->mem_w32(node + 0x34u, rec1c);
    const int64_t prodW = (int64_t)(2304 * (int32_t)gridW) * (int64_t)(int32_t)0x38e38e39;
    const int64_t prodH = (int64_t)(2280 * (int32_t)gridH) * (int64_t)(int32_t)0x38e38e39;
    c->mem_w16(node + 0x30u, (uint16_t)((int32_t)(prodW >> 32) >> 5));
    c->mem_w16(node + 0x32u, (uint16_t)((int32_t)(prodH >> 32) >> 5));
    c->mem_w8(node + 0x38u, 1u);
  }
  // state >= 2: no-op

  c->r[29] += 8;
}

// FUN_80115598
void TileGridLayer::emit(Core* c) {
  const uint32_t node = c->r[4];

  // Real 80B guest-stack frame, 10 callee-save spills. This leaf's own working state lives in C++
  // locals below (never touches c->r[16..23]/r30/r31), so the caller's incoming values are the only
  // thing that ever needs to land in guest memory here (matches overlay_ground_gt3gt4.cpp's
  // entityLoop idiom: spill the caller's live values, run the body, nothing else observes these
  // registers mid-call).
  c->r[29] -= 80;
  c->mem_w32(c->r[29] + 76u, c->r[31]);
  c->mem_w32(c->r[29] + 72u, c->r[30]);
  c->mem_w32(c->r[29] + 68u, c->r[23]);
  c->mem_w32(c->r[29] + 64u, c->r[22]);
  c->mem_w32(c->r[29] + 60u, c->r[21]);
  c->mem_w32(c->r[29] + 56u, c->r[20]);
  c->mem_w32(c->r[29] + 52u, c->r[19]);
  c->mem_w32(c->r[29] + 48u, c->r[18]);
  c->mem_w32(c->r[29] + 44u, c->r[17]);
  c->mem_w32(c->r[29] + 40u, c->r[16]);

  const int W = c->mem_r8(node + 0x10u), H = c->mem_r8(node + 0x11u);
  if (W == 0 || H == 0) { c->r[29] += 80; return; }   // never true in practice (init always sets both), guards the % below
  const int rowstride = W * 2, mapbytes = rowstride * H;
  const int scrollX = c->mem_r16s(node + 0x28u), scrollY = c->mem_r16s(node + 0x2Au);
  const uint32_t tileTable = c->mem_r32(node + 0x14u);
  const uint16_t clutBase  = c->mem_r16(node + 0x06u);
  const uint16_t tpage     = c->mem_r16(node + 0x04u);

  const int cx = 160, cy = 120, winw = 0x160;   // guest-exact 4:3 geometry (no wide extension here)
  int rowtile = ((scrollY - cy) >> 4) % H; if (rowtile < 0) rowtile += H;
  int coltile = ((scrollX - cx) >> 4) % W; if (coltile < 0) coltile += W;
  int t2 = rowtile * rowstride;
  const int coloff0 = coltile * 2;
  const int xoff = (int16_t)(cx - 8 - scrollX);
  const int yoff = (int16_t)(cy - 8 - scrollY);
  const int outer_bound = (int16_t)(scrollY - cy) + 0x100;
  const int t5 = (int16_t)(scrollX - cx) + winw;

  uint32_t pool = c->mem_r32(kPktPoolPtr);
  const uint32_t first = pool;
  uint32_t last = pool;

  // NO host queue push here: the pc_render picture for this layer is ALREADY owned by
  // Render::backdropRender (render_walk.cpp — the host-only transcription of this exact leaf's
  // math, RMSE-verified, #60). Pushing quads from this tap would DOUBLE-DRAW under pc_render, and
  // pushing under psx_render/oracle would contaminate the pure reference (the OT walk already
  // draws these guest packets there). This port's job is native OWNERSHIP of the guest half only.
  static long emitted = 0;

  for (int t8 = scrollY - cy;;) {
    const int Y = (int16_t)((t8 & 0xFFF0) + yoff);
    const int t6 = (int16_t)t2;
    int t0 = coloff0;
    for (int t1 = scrollX - cx;;) {
      const int X = (int16_t)((t1 & 0xFFF0) + xoff);
      const uint16_t tile = c->mem_r16(tileTable + (uint32_t)(t6 + t0));
      const int u = (tile & 0xFu) << 4;
      const int v = (int)(tile & 0xF0u) + 8;   // field V-bias (+8) — this leaf is field-only

      // Field offsets are relative to THE TILE'S OWN 16-byte packet base. In the gen body the
      // data-store base register is a2 = s1 - 16 (0x80115744) while s1 advances FIRST (0x80115788),
      // so its literal encodings 19/20/23/24/28/30(a2) are +3/+4/+7/+8/+12/+14 off the tile base —
      // the canonical SPRT layout: [tag][color|code][xy][uv|clut]. A first draft copied the a2-based
      // literals as base-relative and shifted every field +16 (SBS f117 packet_pool divergence).
      const uint32_t thisAddr = pool;
      c->mem_w32(thisAddr + 4u,  0x7D808080u);
      c->mem_w8 (thisAddr + 7u,  0x7Cu);
      c->mem_w8 (thisAddr + 3u,  3u);
      c->mem_w32(thisAddr + 8u,  (uint32_t)(uint16_t)X | ((uint32_t)(uint16_t)Y << 16));
      c->mem_w16(thisAddr + 12u, (uint16_t)((u & 0xFF) | ((v & 0xFF) << 8)));
      c->mem_w16(thisAddr + 14u, (uint16_t)(clutBase + ((tile & 0xF00u) >> 2)));
      pool += 16u;
      c->mem_w32(thisAddr + 0u, pool | 0x03000000u);
      last = thisAddr;

      if ((emitted++ & 511) == 0)
        cfg_logf("tileq", "[tilegrid] emit #%ld xy=(%d,%d) tile=%04X uv=(%d,%d) clut=%04X",
                 emitted, X, Y, tile, u, v, (unsigned)(clutBase + ((tile & 0xF00u) >> 2)));

      t0 += 2; if (t0 >= rowstride) t0 = 0;
      t1 += 16;
      if (!((int16_t)t1 < t5)) break;
    }
    t2 += rowstride; if ((int16_t)t2 >= mapbytes) t2 -= mapbytes;
    t8 += 16;
    if (!((int16_t)t8 < outer_bound)) break;
  }

  // Patch the last tile's tag: keep the length byte, splice in the pre-existing OT[0x7FF] head.
  const uint32_t otBase = c->mem_r32(kOtBaseGlobal);
  const uint32_t oldHead = c->mem_r32(otBase + kOtBucketBg);
  c->mem_w32(last + 0u, (c->mem_r32(last + 0u) & 0xFF000000u) | oldHead);

  // Trailing DR_TPAGE-reset header packet (func_80083DE0, already RE'd — wide_re_libgpu_leaves.cpp;
  // unowned/substrate, invoked via rec_dispatch exactly like Font::glyphEmit's own tail call).
  const uint32_t header = pool;
  c->r[4] = header; c->r[5] = 0u; c->r[6] = 0u; c->r[7] = tpage;   // r7 unused by the callee (alias only)
  c->mem_w32(c->r[29] + 16u, 0u);                                  // 5th arg (stack): texWinSrc = 0
  rec_dispatch(c, 0x80083DE0u);
  c->mem_w32(header + 0u, first | 0x02000000u);
  c->mem_w32(otBase + kOtBucketBg, header);
  pool = header + 12u;

  c->mem_w32(kPktPoolPtr, pool);

  c->r[31] = c->mem_r32(c->r[29] + 76u);
  c->r[30] = c->mem_r32(c->r[29] + 72u);
  c->r[23] = c->mem_r32(c->r[29] + 68u);
  c->r[22] = c->mem_r32(c->r[29] + 64u);
  c->r[21] = c->mem_r32(c->r[29] + 60u);
  c->r[20] = c->mem_r32(c->r[29] + 56u);
  c->r[19] = c->mem_r32(c->r[29] + 52u);
  c->r[18] = c->mem_r32(c->r[29] + 48u);
  c->r[17] = c->mem_r32(c->r[29] + 44u);
  c->r[16] = c->mem_r32(c->r[29] + 40u);
  c->r[29] += 80;
}

void TileGridLayer::registerOverrides(Game*) {
  extern void ov_a00_gen_8011534C(Core*);
  extern void ov_a00_gen_80115598(Core*);
  extern void engine_set_override_a00(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_a00(0x8011534Cu, &TileGridLayer::scrollStep, ov_a00_gen_8011534C);
  engine_set_override_a00(0x80115598u, &TileGridLayer::emit,       ov_a00_gen_80115598);
}
