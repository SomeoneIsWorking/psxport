// Engine-owned render queue — the SINGLE ordering authority (plan: noble-purring-pelican / CLAUDE.md
// "engine owns render ordering, never the PSX OT"). Every drawable the engine owns is appended here as a
// resolved RqItem (already-projected float screen verts + decoded material + an explicit engine LAYER +
// a per-vertex real depth). At the frame's draw kick the queue is sorted by (layer, submission seq) and
// emitted to the VK rasterizer, which does fine-grained occlusion from the real D32 depth. Draw order is
// thus decided by the engine — NOT by walking the guest ordering table.
//
// M1 (this stage): the natively-owned WORLD geometry (terrain + GT3/GT4 + byte-packed GT4, all via
// gpu_draw_world_quad) routes through here as RQ_WORLD. Guest 2D (background/HUD) + un-owned submit
// variants still draw via the OT walk for now (M3 brings them in as RQ_BACKGROUND/RQ_HUD and retires the
// OT read entirely). Gated by PSXPORT_RQ until the full M1 is coherent; default keeps inline behavior.
#ifndef RENDER_QUEUE_H
#define RENDER_QUEUE_H
#include <stdint.h>

struct Core;

// Explicit engine draw layers, painted low->high. The depth buffer resolves occlusion WITHIN RQ_WORLD;
// the backdrop/HUD layers exist so screen-space 2D is ordered by what it IS, not by OT position.
enum RqLayer { RQ_BACKGROUND = 0, RQ_WORLD = 1, RQ_OVERLAY = 2, RQ_HUD = 3 };

// How the rasterizer gets this item's depth at emit. DEPTH = real per-vertex view-Z (3D world);
// the 2D_* modes select the renderer's far/near screen-space bands (set_order_2d_bg / set_order_2d),
// preserving the existing 2D depth semantics — the queue changes only the draw ORDER, not depth.
enum RqOrderMode { RQ_OM_DEPTH = 0, RQ_OM_2D_BG = 1, RQ_OM_2D_FG = 2 };

// One resolved drawable: a quad (two triangles) with its decoded material + real per-vertex depth. All
// values are captured at enqueue time (after texpage/clut resolution + draw-offset/rounding) so flush is
// independent of any GpuState mutated between enqueue and flush.
struct RqItem {
  uint8_t  layer;          // RqLayer
  uint8_t  semi;           // semi-transparent (blended) quad
  uint8_t  nv;             // vertex count: 3 = triangle (one tri), 4 = quad (two tris)
  uint8_t  raw;            // raw texel (no color modulation)
  uint8_t  order_mode;     // RqOrderMode — how depth is applied at emit
  uint32_t seq;            // submission order — stable tiebreak within a layer
  int      xs[4], ys[4];   // screen verts (with draw offset, rounded)
  int      us[4], vs[4];   // texel coords
  uint8_t  rs[4], gs[4], bs[4];
  float    depth[4];       // normalized per-vertex D32 depth (proj_pz_to_ord)
  int      tp_x, tp_y, mode, clut_x, clut_y;          // resolved texpage / color mode / clut
  int      tw_mx, tw_my, tw_ox, tw_oy;                // texture-window
  int      da_x0, da_y0, da_x1, da_y1;                // draw-area clip
  int      tp_blend;                                  // semi blend mode

  // ---- fps60 reprojection capture (host-only; never guest RAM, does not affect a lockstep diff) ----
  // Captured at native GTE projection (fps60_stamp_world) so the interpolated-60fps tier can recompose +
  // reproject this prim's verts at the A/B midpoint WITHOUT re-running any guest/interpreted render code
  // (the actor-transform layer the user mandated — engine/fps60.cpp). Only the GTE-composed world path
  // (proj_native_xform) fills these; terrain/2D/HUD leave fps_world=0 and snap.
  uint8_t  fps_world;      // 1 = GTE-composed world prim with valid capture below
  uint32_t fps_key;        // cross-frame ACTOR identity (cmd ptr from submit_perobj_flush; 0 = snap)
  uint32_t fps_cr[11];     // projection state at draw: [0..7]=composed CR0-7 (rot CR0-4, trans CR5-7),
                           // [8]=CR24 OFX, [9]=CR25 OFY, [10]=CR26 H — proj_native_xform reads all of these.
  int16_t  fps_mv[4][3];   // per-vertex MODEL-space coords = the input to proj_native_xform
  int16_t  fps_offx, fps_offy;  // draw offset baked into xs/ys (so a reproject reproduces them exactly)
};

#define RQ_MAX 32768

// Per-instance (on Game) so two cores keep independent queues; pure host render data (never guest RAM),
// so it does not affect a Core::ram lockstep diff.
struct RenderQueue {
  RqItem   items[RQ_MAX];
  int      n        = 0;
  uint32_t seq      = 0;
  int      consumed = 1;   // start consumed so the first push begins a clean frame
  void     reset();
  RqItem*  push();         // NULL on overflow
  void     flush(Core* core);   // sort by (layer, seq), emit each, mark consumed
  void     mark_consumed();
};

int  rq_active(void);                  // PSXPORT_RQ — route owned world geometry through the queue
RqItem* rq_push(Core* core);           // reserve a slot (lazy per-frame reset); NULL on overflow
void rq_flush(Core* core);             // emit the frame's queued items in engine order

// Emit one resolved item to the VK rasterizer (defined in gpu_native.cpp, where the gpu_vk_* draw
// entries live). Used by both the inline path and the queue flush so emission logic lives in one place.
void gpu_emit_rq_item(Core* core, const RqItem* it);

#endif
