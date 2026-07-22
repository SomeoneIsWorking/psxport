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

// Reserved dbg_node sentinel for TERRAIN prims (native_terrain.cpp, tagged via Render::diag.beginObject/
// endObject around the quad-draw loop). Distinguishes them from the OTHER dbg_node==0 RQ_WORLD producer,
// Render::fieldEntityRender (grass/props/"terrain props" — the SOP field-overlay SCENE TABLE walk) — a
// real guest node pointer is always inside the 2 MB main-RAM window (< 0x80200000), so this value can
// never collide with one. Fps60::tier1Render (docs/fps60-rework.md "Object-tier attempt") re-renders
// terrain under this sentinel, so its queue-lerp exclusion keys on it, not "dbg_node==0" generally.
static constexpr uint32_t kTerrainDbgNode = 0xFFFF0001u;

// Reserved dbg_node sentinel for SCENE-TABLE (grass/terrain-prop) prims: Render::fieldEntityRender now
// scopes its own diag.beginObject/endObject (submit.cpp) the same way native_terrain.cpp scopes terrain,
// so Fps60::tier1Render — extended to re-run fieldEntityRender (camera-only, same projComposeCamera path
// terrainRenderAll uses) — can exclude exactly its own prims from the queue-lerp, symmetric with
// kTerrainDbgNode. Distinct value so the two tier-1-owned producers never collide with each other or with
// a real guest node pointer.
static constexpr uint32_t kSceneTableDbgNode = 0xFFFF0002u;

// Reserved dbg_node sentinel for the NATIVE SCROLLING-BACKDROP prims: Render::backdropRender (the field's
// sky/parallax tilemap, render_walk.cpp) scopes its own diag.beginObject/endObject around its push2dQuad
// loop, the same way terrain/scene-table do. #54 (main-menu widescreen+fps60): RQ_BACKGROUND is NOT a
// single-producer layer — the generic guest-OT walk (runtime/recomp/gpu_native.cpp) ALSO classifies any
// full-screen 2D poly/sprite/FillRect (menu backdrop art, hut-interior clear, SOP-narration fills, #52's
// FillRect widen) as RQ_BACKGROUND by SCREEN COVERAGE, with no relation to backdropRender. Those OT-walk
// items keep dbg_node==0 (no beginObject scope wraps the OT walk) — this sentinel is what lets
// Fps60::isTier1Owned key on "backdropRender's OWN prims" specifically, instead of the whole layer, so
// non-field 2D backdrops fall through to the normal per-prim match+lerp path instead of being silently
// dropped from every interpolated frame (found via the title-menu screen going backdrop-less at 60fps).
static constexpr uint32_t kBackdropDbgNode = 0xFFFF0003u;

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

  // dbg_node: the per-instance ENTITY NODE ptr this prim belongs to (0 = unknown/un-owned) — the
  // objid overlay's display identity, and the tier1-ownership discriminator for fps60's present merge
  // (kTerrainDbgNode/kSceneTableDbgNode/kBackdropDbgNode sentinels).
  uint32_t dbg_node;

  // GAME SORT KEY (kanban #11): the OT bucket index the GAME'S OWN submitter computes for this face
  // (recomputed natively in submit.cpp game_sort_key from the RE'd gen_func_8007FDB0/8008007C bodies —
  // AVSZ/min/max policy by GP0 code, guest log-compression, per-command sub-bucket shift). -1 = no key
  // (2D, terrain, prims the guest's own range check would drop). key_ord = the key mapped back into the
  // SAME normalized ord scale as depth[] (submit.cpp key_to_ord) — one shared value per key, used by
  // resolveKeyOrder when the depth buffer would contradict the game's authored face order.
  int32_t  sort_key;
  float    key_ord;

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
  void     zfightScan(Core* core);  // PSXPORT_ZFIGHT diag: SW-rasterize opaque depth prims, find near-equal top-2 contests
  void     mark_consumed();

  // push2dQuad: enqueue a 2D textured quad (HUD / overlay / background) into the render queue so it is
  // part of THE FRAME and gets re-emitted on both 60fps present passes (no direct gpu_vk_draw_tritri
  // bypass that lands on one pass). layer = RQ_BACKGROUND/RQ_OVERLAY/RQ_HUD; order_2d_fg picks the 2D
  // far/near band. Body in render_queue.cpp.
  void push2dQuad(int layer, int order_2d_fg,
                  const int* xs, const int* ys, const int* us, const int* vs,
                  const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                  int tp_x, int tp_y, int mode, int raw, int clut_x, int clut_y,
                  int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1,
                  int semi = 0);   // semi=1 -> blended quad (the guest's GP0 0x62-class semi-transparent prims)

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
                   int tp_blend, const float (*sv)[3] = nullptr,
                   int sort_key = -1, float key_ord = 0.0f);

  // drawWorldQuad: PC-native world-quad draw — a quad already projected to FLOAT screen coords + real
  // per-vertex depth, teed as two triangles to the VK rasterizer through the queue. No GP0 packet, no
  // OT, no guest write. sort_key/key_ord: the game's own OT sort key for this face (see RqItem), -1 =
  // none (producers with no guest sort key — terrain, mesh overlays — keep pure per-vertex depth).
  void drawWorldQuad(Core* core, const float* px, const float* py, const float* depth,
                     const int* u, const int* v, const unsigned char* r, const unsigned char* g,
                     const unsigned char* b, uint16_t tp, uint16_t clut, int semi,
                     const float (*sv)[3], int sort_key = -1, float key_ord = 0.0f);

  // GAME-SORT-KEY ORDER RESOLUTION (kanban #11) — see the implementation banner in render_queue.cpp.
  // Runs once per frame at flush: within each object, finds face pairs whose interpolated per-vertex
  // depth would CONTRADICT the game's own sort-key order and snaps those faces' test depth to their
  // key's shared ord. Zero bias, zero constants: every value is the game's own computation.
  void resolveKeyOrder(Core* core);

  // finalize — turn a fully-submitted queue into a DRAWABLE one: resolve the game's authored face
  // order, then sort. THE one place that answers "what does a finished queue look like", so a real
  // frame and an interpolated frame cannot drift apart in how they are built (the fps60 principle:
  // no difference between the two except the lerp, and the lerp lives in the INPUTS). Both callers
  // are the two queue-finishing sites: RenderQueue::flush for the real frame, and Fps60::tier1Render
  // for the re-rendered world of the in-between frame. Adding a third ordering step belongs HERE, not
  // at a call site — kanban #17 is what happens when it doesn't (resolveKeyOrder ran only on the real
  // frame, so the barrel's snapped faces alternated every other frame and flickered at 60fps).
  void finalize(Core* core);

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
