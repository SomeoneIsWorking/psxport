// Engine-owned render queue — the SINGLE ordering authority (plan: noble-purring-pelican / CLAUDE.md
// "engine owns render ordering, never the PSX OT"). Every drawable the engine owns is appended here as a
// resolved RqItem (already-projected float screen verts + decoded material + an explicit engine LAYER +
// a per-vertex real depth). At the frame's draw kick the queue is sorted by (layer, submission seq) and
// emitted to the VK rasterizer, which does fine-grained occlusion from the real D32 depth. Draw order is
// thus decided by the engine — NOT by walking the guest ordering table.
//
// M1 (this stage): the natively-owned WORLD geometry (terrain + GT3/GT4 + byte-packed GT4, all via
// RenderQueue::drawWorldQuad) routes through here as RQ_WORLD. Guest 2D (background/HUD) + un-owned submit
// variants still draw via the OT walk for now (M3 brings them in as RQ_BACKGROUND/RQ_HUD and retires the
// OT read entirely). Gated by PSXPORT_RQ until the full M1 is coherent; default keeps inline behavior.
#ifndef RENDER_QUEUE_H
#define RENDER_QUEUE_H
#include <stdint.h>

struct Core;
class Game;

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
  uint8_t  has_xyf;        // 1 = xsf/ysf are valid sub-pixel positions (world prims via drawWorldQuad)
  int      us[4], vs[4];   // texel coords
  uint8_t  rs[4], gs[4], bs[4];
  float    depth[4];       // normalized per-vertex D32 depth (proj_pz_to_ord)
  int      tp_x, tp_y, mode, clut_x, clut_y;          // resolved texpage / color mode / clut
  int      tw_mx, tw_my, tw_ox, tw_oy;                // texture-window
  int      da_x0, da_y0, da_x1, da_y1;                // draw-area clip
  int      tp_blend;                                  // semi blend mode

  // ---- fps60 TRUE per-object interpolation (host-only; never guest RAM, no lockstep-diff effect) ----
  // fps_scene: 1 = this prim was produced by the READ-ONLY native scene render (Render::sceneNative:
  //   terrain / per-object meshes / backdrop), armed via Fps60::mSceneTag around the sceneNative() call in
  //   Engine::drawOTag. 0 = an OT-walk prim (2D / HUD / billboard). The mid-present REBUILDS the fps_scene
  //   prims fresh at the interpolated transform (re-running sceneNative) and RE-EMITS the fps_scene=0 prims.
  uint8_t  fps_scene;
  // 3D-POSITIONED 2D QUAD (billboard): a screen-aligned sprite anchored at a 3D WORLD point, emitted by the
  // guest into the OT and picked up at the deferred OT walk (gpu_native.cpp) as an RQ_WORLD/RQ_OM_DEPTH
  // item. It is NOT a projection of model verts, so it cannot be per-vertex re-projected like a mesh; the
  // OT walk stamps it (Fps60::stampBillboard) with its object IDENTITY (fps_key) + captured WORLD anchor
  // position (fps_wpos, from node+46/50/54). The mid-present re-projects that anchor through the real
  // projection at the interpolated (prev,cur) world position + interpolated camera and translates the quad.
  uint8_t  fps_anchor;     // 1 = 3D-positioned 2D quad (billboard): re-project the world anchor at midpoint
  uint32_t fps_key;        // cross-frame billboard identity (object node ptr; 0 = not a tracked billboard)
  float    fps_wpos[3];    // billboard object WORLD position (node+46/50/54) — the anchor to project
  uint32_t dbg_node;       // DEBUG (objid overlay): the per-instance ENTITY NODE ptr this prim belongs to
                           // (0 = unknown). Its model id (node+0xe & 0x3fff) is the GAME object id we display.

  // ---- dynamic-shadow capture (host-only) ----------------------------------------------------------
  // Opaque world prims cast into the shadow map. The shadow GEOMETRY is part of THE FRAME (the queue),
  // not a side-channel: it is carried here and re-pushed to the shadow VBO by emitItem every time
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
  Game*    game = nullptr;   // back-pointer wired in Game() so methods can reach Core (game->core)
  RqItem   items[RQ_MAX];
  int      n        = 0;
  uint32_t seq      = 0;
  int      consumed = 1;   // start consumed so the first push begins a clean frame
  void     reset();
  RqItem*  push();         // NULL on overflow (reserves a slot; lazy per-frame reset)
  void     flush(Core* core);   // sort by (layer, seq), then capture (fps60) OR emit each, mark consumed
  void     sortQueue();    // stable_sort items by (layer, seq) — the engine draw order (fps60 mid-present)
  void     emitQueue(Core* core);   // emit each item to the VK rasterizer + mark consumed (no sort)
  void     mark_consumed();

  // push2dQuad: enqueue a 2D textured quad (HUD / overlay / background) into the render queue so it is
  // part of THE FRAME and gets re-emitted on both 60fps present passes (no direct gpu_gpu_draw_tritri
  // bypass that lands on one pass). layer = RQ_BACKGROUND/RQ_OVERLAY/RQ_HUD; order_2d_fg picks the 2D
  // far/near band. Body in render_queue.cpp.
  void push2dQuad(int layer, int order_2d_fg,
                  const int* xs, const int* ys, const int* us, const int* vs,
                  const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                  int tp_x, int tp_y, int mode, int raw, int clut_x, int clut_y,
                  int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1);

  // emitItem: emit one resolved item to the VK rasterizer. Used by both the inline path and the
  // queue flush so emission logic lives in one place.
  void emitItem(Core* core, const RqItem* it);

  // emitOrQueue: build an RqItem from already-resolved quad/tri data + material snapshot, then either
  // queue it (`capture`, engine owns the order, flushed at the draw kick) or emit it now. The ONE place
  // the submit paths (world quad, guest poly, guest sprite) funnel through.
  void emitOrQueue(Core* core, int capture, int layer, int order_mode, int nv, int semi, int raw,
                   const int* xs, const int* ys, const float* xsf, const float* ysf,
                   const int* us, const int* vs,
                   const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                   const float* depth, int mode, int tp_x, int tp_y, int clut_x, int clut_y,
                   int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1,
                   int tp_blend, const float (*sv)[3] = nullptr);

  // drawWorldQuad: PC-native world-quad draw — a quad already projected to FLOAT screen coords + real
  // per-vertex depth, teed as two triangles to the VK rasterizer through the queue. No GP0 packet, no
  // OT, no guest write.
  void drawWorldQuad(Core* core, const float* px, const float* py, const float* depth,
                     const int* u, const int* v, const unsigned char* r, const unsigned char* g,
                     const unsigned char* b, uint16_t tp, uint16_t clut, int semi,
                     const float (*sv)[3]);

private:
  // Debug OBJECT-ID overlay (REPL `debug objid`) — pure host HUD quads appended at flush time.
  static bool objidSolid(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
                         unsigned char r, unsigned char g, unsigned char b);
  static void objidChar(Core* core, const RqItem* ref, char ch, int x, int y, int s,
                        unsigned char r, unsigned char g, unsigned char b);
  static void objidStr(Core* core, const RqItem* ref, int x, int y, int s, const char* str,
                       unsigned char r, unsigned char g, unsigned char b);
  static void objidBox(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
                       unsigned char r, unsigned char g, unsigned char b, int t);
  void objidOverlay(Core* core);
};

int  rq_active(void);                  // PSXPORT_RQ — route owned world geometry through the queue

#endif
