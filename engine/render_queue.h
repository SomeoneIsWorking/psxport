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
  int      xs[4], ys[4];   // screen verts (with draw offset, rounded) — 2D/HUD + fallback path
  // Sub-pixel float screen XY (draw offset applied in float) for the engine-owned 3D world path. When
  // has_xyf is set the rasterizer uses these instead of the rounded xs/ys, so world geometry keeps its
  // sub-pixel position and stops snapping pixel-to-pixel (PS1 wobble) — vertex smoothing, issue #15.
  float    xsf[4], ysf[4];
  uint8_t  has_xyf;        // 1 = xsf/ysf are valid sub-pixel positions (world prims via gpu_draw_world_quad)
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
  uint8_t  fps_world;      // 1 = world prim with valid reprojection capture below (native mesh OR billboard)
  // 3D-POSITIONED 2D QUAD (billboard) capture (host-only). Collectable/overlay billboard quads are
  // screen-aligned 2D quads anchored at a 3D WORLD point — they are NOT projections of model verts, so
  // they cannot be per-vertex reprojected like a mesh. They reach the render queue at the DEFERRED OT walk
  // (gpu_native.cpp) as RQ_WORLD/RQ_OM_DEPTH items with NO sub-pixel float XY (has_xyf==0), inheriting the
  // object's PC-native world-position view-Z via obj_depth_lookup. They are tagged AT QUEUE TIME, in the OT
  // walk: engine_submit.cpp recorded each object's packet-pool SPAN + identity + composed transform
  // (fps60_record_billboard_span), and the OT walk — which knows the source OT-node — calls
  // fps60_stamp_billboard, which looks the node up in that span registry (fps60_billboard_for_node) and sets
  // fps_world=1, fps_anchor=1, fps_key = the object identity, fps_cr = the composed camera×object transform,
  // fps_mv[0] = the object origin (= the WORLD-POSITION anchor; CR5-7 is its view-space pos). build_lerp then
  // reprojects the ANCHOR at the A/B-midpoint camera and TRANSLATES the whole 2D quad by (anchor_mid -
  // anchor_B), so the billboard pans/moves smoothly instead of snapping to camera-B. The cross-frame match
  // is keyed on the object IDENTITY (fps_key), not a fragile depth-ord reverse-match.
  uint8_t  fps_anchor;     // 1 = 3D-positioned 2D quad: translate by the anchor's midpoint screen delta
  uint32_t fps_key;        // cross-frame ACTOR identity (cmd ptr from submit_perobj_flush; 0 = snap)
  uint32_t dbg_node;       // DEBUG (objid overlay): the per-instance ENTITY NODE ptr this prim belongs to
                           // (0 = unknown). Its model id (node+0xe & 0x3fff) is the GAME object id we display.
  uint32_t fps_cr[11];     // projection state at draw: [0..7]=composed CR0-7 (rot CR0-4, trans CR5-7),
                           // [8]=CR24 OFX, [9]=CR25 OFY, [10]=CR26 H — proj_native_xform reads all of these.
  int16_t  fps_mv[4][3];   // per-vertex MODEL-space coords = the input to proj_native_xform
  int16_t  fps_offx, fps_offy;  // draw offset baked into xs/ys (so a reproject reproduces them exactly)

  // ---- dynamic-shadow capture (host-only) ----------------------------------------------------------
  // Opaque world prims cast into the shadow map. The shadow GEOMETRY is part of THE FRAME (the queue),
  // not a side-channel: it is carried here and re-pushed to the shadow VBO by gpu_emit_rq_item every time
  // this item is emitted. Because the queue is emitted on BOTH 60fps present passes, the shadow map is
  // rebuilt identically on each — no keep_shadow side-channel, no strobe. View space = (x=vx, y=vy, z=pz),
  // the metric view space the deferred/light pass reconstructs; never interpolated (B positions on both
  // passes — build_lerp leaves these untouched), per the user's "shadows are not interpolated" design.
  uint8_t  sh_cast;        // 1 = opaque world prim that casts a shadow (push sh_v* as two tris at emit)
  float    sh_vx[4], sh_vy[4], sh_vz[4];   // view-space verts (the shadow VBO input)
};

// Per-frame prim capacity. Measured worst case in real play (later-273): steady-state field ≈ 1k prims/frame,
// but the AREA-TRANSITION frame (first field-load frame, sm[0x4e]→9) spikes to ≈ 43k as the whole area's
// geometry is submitted at once. 32768 was too small for that transient (it silently dropped prims). Sized
// to comfortably hold the observed worst case with headroom; push() FAIL-FASTS above this (a true runaway —
// e.g. a stuck render walk re-submitting forever — is a real bug, not a drop-and-continue).
#define RQ_MAX 65536

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

// Emit one resolved item to the VK rasterizer (defined in gpu_native.cpp, where the gpu_gpu_* draw
// entries live). Used by both the inline path and the queue flush so emission logic lives in one place.
void gpu_emit_rq_item(Core* core, const RqItem* it);

// Enqueue a 2D textured quad (HUD / overlay) into the render queue so it is part of THE FRAME and gets
// re-emitted on both 60fps present passes (no direct gpu_gpu_draw_tritri bypass that lands on one pass).
// layer = RQ_OVERLAY / RQ_HUD; order picks the 2D far/near band. Defined in gpu_native.cpp (RqItem fill).
void rq_push_2d_quad(Core* core, int layer, int order_2d_fg,
                     const int* xs, const int* ys, const int* us, const int* vs,
                     const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                     int tp_x, int tp_y, int mode, int raw, int clut_x, int clut_y,
                     int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1);

#endif
