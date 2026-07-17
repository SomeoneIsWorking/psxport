// game/render/text_label.cpp — Render::textLabelEmit, the per-character 3D TEXT-LABEL renderer
// (FUN_80039F4C, renderWalk case 0x8003C0E8 — the REDIRECT census "0x80039F4C score strip").
//
// WHAT IT DRAWS (RE: Ghidra scratch/decomp/quad_emitters.c + ground truth generated/shard_1.c
// gen_func_80039F4C, cross-checked instruction-by-instruction):
//   - runs FUN_8003F174(node, 1) first — the node's MESH pass: per cmd (node+0xC0[i]) it loads GTE
//     CR0-7 DIRECTLY from the PRE-COMPOSED matrix stored on the cmd at +0x18..0x34 (NOT the
//     cmdListDispatch camera∘object compose — a different cmd layout for this node class) and runs
//     func_8003F698 (the generic geomblk submit) — still substrate.
//   - then per CHARACTER of the label text: one glyph quad from the fixed template
//     V0(-3,-7,-1) V1(5,-7,-1) V2(-3,9,-1) V3(5,9,-1) (built into the REAL guest stack sp+16..47),
//     projected by func_8003F7D8 (RTPT/RTPS/AVSZ4, same shape as QuadRtptSubmit::submitQuad) under
//     the per-char PRE-COMPOSED MATRIX at cmd+0x18 loaded via libgte SetRotMatrix/SetTransMatrix
//     (func_80084660/func_80084690 — NOT "pool-span markers"; that older note was a mis-RE).
//     Glyph UV comes from func_80039E80 (char*8 → atlas u, ((char+32)>>5)*16+8 → v; space = skip);
//     material patches: code 0x2D (textured raw), tpage half 0x1F, clut 0x7DFF (0x7C7F for the
//     "Clear" variant node+3==2). Text = strcpy("Clear")+strcat(DAT_80014A1C) when node+3==2, else
//     string table 0x800A33CC[node.s16[+96] * 3].ptr (word +4 of the 12-byte entry).
//   - packet bump-copied at the pool tail (0x800BF544, 40 bytes) and OT-linked at (otz-1).
//
// WHY OWNED: the faithful guest body stays byte-identical (all callees substrate, real guest frame,
// register-faithful) — owning the ORCHESTRATOR lets the display pass get the label's PICTURE from
// game state: at the link point each surviving glyph is captured as a Render::WqRec (corners =
// template constants, world transform = the cmd+0x18 matrix FACTORED against the scene camera —
// render.h WqRec banner) and emitted by Render::billboardsRender through the float camera path,
// fps60-lerped like every other world prim. Replaces the RenderObserver wrapper this address
// carried (whose depth-tag behavior is preserved via withDepthTag below).
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include "render_internal.h"   // withDepthTag / cur_render_node
#include "guest_abi.h"         // GuestFrame / GuestFrameSpill / guest_call
#include "cfg.h"
#include <stdint.h>

void func_8003F174(Core*);   // still-substrate: the node's pre-composed-matrix mesh pass
void func_80039E80(Core*);   // still-substrate: per-char glyph UV fill (space → -1)
void func_8003F7D8(Core*);   // still-substrate: RTPT/RTPS/AVSZ4 glyph projector
void func_80084660(Core*);   // libgte SetRotMatrix  (CR0-4 <- MATRIX.m)
void func_80084690(Core*);   // libgte SetTransMatrix(CR5-7 <- MATRIX.t)
void func_8009A5B0(Core*);   // libc strcpy (substrate)
void func_8009A490(Core*);   // libc strcat (substrate)
int  gpu_gpu_wide_engine(Core*);     // gpu_gpu.cpp — genuine engine-wide FOV active
int  gpu_gpu_wide_engine_w(Core*);   // gpu_gpu.cpp — the wide screen width (nw)

namespace {
constexpr uint32_t PKT_POOL_PTR = 0x800BF544u;   // packet-pool bump-allocator write pointer
constexpr uint32_t OTBASE_PTR   = 0x800ED8C8u;   // *this = the active ordering-table base

// Guest-stack frame contract — tools/abi_extract.py 0x80039F4C --scaffold --guestabi (ground truth).
constexpr GuestFrameSpill kSpills_80039F4C[7] = {
  { 20, 104 }, { 31 /*ra*/, 112 }, { 21, 108 }, { 19, 100 }, { 18, 96 }, { 17, 92 }, { 16, 88 },
};

// The fixed glyph template (model space, s16) the guest builds at sp+16..47 every call.
constexpr int16_t kGlyphX[4] = { -3, 5, -3, 5 };
constexpr int16_t kGlyphY[4] = { -7, -7, 9, 9 };
constexpr int16_t kGlyphZ    = -1;

void textLabelBody(Core* c) {
  GuestFrame<120, 7> frame(c, kSpills_80039F4C);
  const uint32_t sp   = c->r[29];
  const uint32_t node = c->r[4];
  c->r[20] = node;                                  // gen keeps the node live in r20

  // (1) mesh pass: per-cmd pre-composed-matrix geomblk submit (still substrate).
  c->r[4] = node; c->r[5] = 1;
  guest_call(c, 0x80039F78u, func_8003F174);

  // (2) glyph template into the REAL guest stack (sp+16..47) — byte order per gen.
  for (int i = 0; i < 4; i++) {
    c->mem_w16(sp + 16u + (uint32_t)i * 8u, (uint16_t)kGlyphX[i]);
    c->mem_w16(sp + 18u + (uint32_t)i * 8u, (uint16_t)kGlyphY[i]);
    c->mem_w16(sp + 20u + (uint32_t)i * 8u, (uint16_t)kGlyphZ);
  }

  // (3) label text pointer: "Clear"+suffix into the guest stack buffer (sp+48), or the string table.
  uint32_t text;
  if (c->mem_r8(node + 3u) == 2u) {
    c->r[4] = sp + 48u; c->r[5] = c->mem_r32(0x800A3A8Cu);          // strcpy(buf, "Clear")
    c->r[18] = sp + 48u;                                            // gen: r18 = buf (live)
    guest_call(c, 0x80039FE4u, func_8009A5B0);
    c->r[4] = c->r[18]; c->r[5] = 0x80014A1Cu;                      // strcat(buf, suffix)
    guest_call(c, 0x80039FF4u, func_8009A490);
    text = c->r[18];
  } else {
    const int32_t idx = (int32_t)(int16_t)c->mem_r16(node + 96u);
    // gen: mem32(0x800A33C8 + idx*12 + 4) — 12-byte string-table entries based at 0x800A33C8, the
    // string POINTER is word +4 of the entry (a first draft mis-based this at 0x800A33CC and read
    // the next word — one whole extra/different string, caught by SBS at f190).
    text = c->mem_r32(0x800A33C8u + (uint32_t)(idx * 12) + 4u);
  }
  c->r[18] = text;

  // (4) per-character loop — counts re-read from the node each iteration, exactly like gen.
  if (c->mem_r8(node + 9u) == 0u) return;
  c->r[17] = 0;
  if ((int32_t)c->mem_r8(node + 8u) <= 0) return;
  c->r[21] = (uint32_t)32780u << 16;                // gen's live pool-base register (callees spill it)
  c->r[19] = node;                                   // cmd cursor (node + i*4; cmd read at +0xC0)
  for (;;) {
    const uint32_t ch = c->mem_r8(c->r[18]);
    if (ch == 0u) break;
    // glyph UV fill into the packet at the pool tail (space → v0=-1 → skip).
    c->r[4] = c->r[18]; c->r[5] = c->mem_r32(PKT_POOL_PTR);
    guest_call(c, 0x8003A05Cu, func_80039E80);
    if ((int32_t)c->r[2] != -1) {
      const uint32_t cmd = c->mem_r32(c->r[19] + 192u);
      c->r[16] = c->mem_r32(PKT_POOL_PTR);                          // this glyph's packet
      c->mem_w32(PKT_POOL_PTR, c->r[16] + 40u);                     // bump
      c->r[4] = cmd + 24u;  guest_call(c, 0x8003A080u, func_80084660);   // SetRotMatrix(cmd+0x18)
      c->r[4] = c->mem_r32(c->r[19] + 192u) + 24u;
      guest_call(c, 0x8003A08Cu, func_80084690);                    // SetTransMatrix(cmd+0x18)
      c->r[4] = c->r[16]; c->r[5] = sp + 16u; c->r[6] = sp + 80u;
      guest_call(c, 0x8003A09Cu, func_8003F7D8);                    // project the template
      const int32_t otzm1 = (int32_t)c->r[2] - 1;
      if (otzm1 >= 0) {
        const uint32_t pk = c->r[16];
        auto sx = [&](uint32_t off) { return (uint32_t)c->mem_r16(pk + off); };
        // xmax widened under the engine-wide FOV (submit_xmax precedent; SBS legs run 4:3).
        const uint32_t xmax = gpu_gpu_wide_engine(c) ? (uint32_t)gpu_gpu_wide_engine_w(c) : 320u;
        const bool xok = sx(8) < xmax || sx(16) < xmax || sx(24) < xmax || sx(32) < xmax;
        const bool yok = sx(10) < 240u || sx(18) < 240u || sx(26) < 240u || sx(34) < 240u;
        if (xok && yok) {
          c->mem_w8(pk + 7u, 45u);                                  // code 0x2D (textured raw)
          c->mem_w16(pk + 22u, 31u);                                // tpage half
          c->mem_w16(pk + 14u, c->mem_r8(node + 3u) == 2u ? 31871u : 32255u);   // clut
          const uint32_t otbase = c->mem_r32(OTBASE_PTR);
          const uint32_t slot = otbase + (uint32_t)otzm1 * 4u;
          c->mem_w32(pk + 0u, c->mem_r32(slot) | 0x09000000u);
          c->mem_w32(slot, pk);
          // #67 display-pass capture: this glyph's picture comes from state — template corners +
          // the cmd's pre-composed matrix factored against the scene camera (render.h WqRec).
          if (!c->game->oracle) {
            Render::WqRec w;
            w.node = node;
            w.seq = 0;
            for (const Render::WqRec& p : rend(c)->mWqRecs) if (p.node == w.node) w.seq++;
            for (int i = 0; i < 4; i++) { w.vx[i] = kGlyphX[i]; w.vy[i] = kGlyphY[i]; w.vz[i] = kGlyphZ; }
            float crF[3][3], tr[3];
            wq_read_matrix(c, cmd + 24u, crF, tr);
            wq_factor_world(c, crF, tr, w.objR, w.objT);
            { const uint32_t col = c->mem_r32(pk + 4u);
              for (int i = 0; i < 4; i++) w.wCol[i] = col; }
            w.wUv0 = c->mem_r32(pk + 12u); w.wUv1 = c->mem_r32(pk + 20u);
            w.wUv2 = c->mem_r32(pk + 28u); w.wUv3 = c->mem_r32(pk + 36u);
            rend(c)->mWqRecs.push_back(w);
          }
        }
      }
    }
    c->r[19] += 4u; c->r[17] += 1u; c->r[18] += 1u;
    if ((int32_t)c->r[17] >= (int32_t)c->mem_r8(node + 9u)) break;
    if ((int32_t)c->r[17] >= (int32_t)c->mem_r8(node + 8u)) break;
  }
}

void ov_textLabelEmit(Core* c) { rend(c)->textLabelEmit(); }
} // namespace

void Render::textLabelEmit() {
  Core* c = mCore;
  // Preserve the retired RenderObserver wrapper's behavior: oracle runs the body pure (handled by
  // the engine_set_override_main thunk routing core B to gen); everyone else gets the packet-span
  // depth tag + dbg_node scope around the body.
  withDepthTag(c, c->r[4], textLabelBody);
}

void text_label_install() {
  extern void gen_func_80039F4C(Core*);
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x80039F4Cu, ov_textLabelEmit, gen_func_80039F4C);
}
