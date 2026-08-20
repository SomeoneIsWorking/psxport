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
#include "painter_object_layer.h"
#include "present_ledger.h"
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

// The screen space a 2D producer's x coordinates are ALREADY in.
//
// The queue CANNOT infer this, and must not try: two producers reach it through the very same leaf.
// Render::emitUiFt4 serves both the 4:3-authored field HUD and the projection-anchored score popup,
// so the space is a property of the CALLER, not of the layer, the order mode, or the material. The
// producer declares it (RenderQueue::Space2dScope); everything that does not declare is authored 4:3,
// which is the overwhelming majority and the historical behaviour.
enum Rq2dSpace {
  // x is as the game authored it, in [0, native_w): a fixed HUD / menu / panel / dialogue layout.
  // Widescreen CENTRES it in the wide frame (or, for a uniform background fill, stretches it).
  RQ_2D_AUTHORED_4_3 = 0,
  // x already came out of a projection the framework itself WIDENED — the guest GTE with CR24 = OFX
  // = nw/2 (native_boot.cpp), or the native camera — so it is in [0, ww) and ALREADY lines up with
  // the world. Centring it again moves it off whatever it is anchored to by exactly one margin.
  // That was Tomba2 kanban #73: the score popup sat 54 px right of the character at 16:9.
  RQ_2D_WIDE_FINAL = 1,
};

// The resolved widescreen layout transform for one 2D submission. Split from the decision so the
// SAME decision applies to the integer and the float vertex arrays without being re-derived.
struct Rq2dXform {
  int shift = 0;        // x + shift        (the centring margin)
  bool stretch = false; // x * num / den    (a uniform background fill spread across the wide FB)
  int num = 1, den = 1;
  int apply(int x) const {
    return stretch ? (int)((long long)x * num / den) : x + shift;
  }
  float applyf(float x) const {
    return stretch ? x * (float)num / (float)den : x + (float)shift;
  }
};

// rq_2d_xform: THE widescreen 2D layout rule, as pure arithmetic — no Core, no GPU, no globals — so
// it is hermetically testable (tests/test_rq_widen_2d.cpp). `ww` is the wide framebuffer width and
// `native_w` the GAME'S OWN 4:3 width; passing a hardcoded 320 here is the bug psxport a0b88136 /
// 94e52472 / 2c54ce71 / 6dda8528 each fixed one instance of (Spyro renders 512 wide). `flat` and
// `untextured` describe the quad's material and select the background stretch. At 4:3, ww ==
// native_w, the margin is 0 and every path is the identity.
Rq2dXform rq_2d_xform(int ww, int native_w, Rq2dSpace space, int layer, bool flat, bool untextured);

// Single-coordinate convenience over rq_2d_xform (tests and any caller with one x).
inline int rq_widen_2d_x(int x, int ww, int native_w, Rq2dSpace space, int layer, bool flat, bool untextured) {
  return rq_2d_xform(ww, native_w, space, layer, flat, untextured).apply(x);
}

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
  uint8_t layer;                  // RqLayer
  uint8_t semi;                   // semi-transparent (blended) quad
  uint8_t nv;                     // vertex count: 3 = triangle (one tri), 4 = quad (two tris)
  uint8_t raw;                    // raw texel (no color modulation)
  uint8_t order_mode;             // RqOrderMode — how depth is applied at emit
  uint8_t painter_flags;          // legacy scope flags, mapped into explicit per-item state at capture
  uint8_t shade_gouraud;          // original G3/G4 shading opcode; cannot be inferred from equal RGB
  uint8_t dither;                 // original draw-mode DTD bit, carried per item
  PainterObjectId painter_object; // 0 = ordinary path; non-zero = local authored-order object
  uint32_t seq;                   // submission order — stable tiebreak within a layer
  int xs[4], ys[4];               // screen verts (with draw offset, rounded) — 2D/HUD + fallback path
  // Sub-pixel float screen XY (draw offset applied in float) for the engine-owned 3D world path. When
  // has_xyf is set the rasterizer uses these instead of the rounded xs/ys, so world geometry keeps its
  // sub-pixel position and stops snapping pixel-to-pixel (PS1 wobble) — vertex smoothing, issue #15.
  float xsf[4], ysf[4];
  uint8_t has_xyf;  // 1 = xsf/ysf are valid sub-pixel positions (world prims via drawWorldQuad)
  int us[4], vs[4]; // texel coords
  uint8_t rs[4], gs[4], bs[4];
  float depth[4];                       // normalized per-vertex D32 depth (proj_pz_to_ord)
  int tp_x, tp_y, mode, clut_x, clut_y; // resolved texpage / color mode / clut
  int tw_mx, tw_my, tw_ox, tw_oy;       // texture-window
  int da_x0, da_y0, da_x1, da_y1;       // draw-area clip
  int tp_blend;                         // semi blend mode

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
  int32_t sort_key;
  float key_ord;

  // ---- dynamic-shadow capture (host-only) ----------------------------------------------------------
  // Opaque world prims cast into the shadow map. The shadow GEOMETRY is part of THE FRAME (the queue),
  // not a side-channel: it is carried here and re-pushed to the shadow VBO by emitItem every time
  // this item is emitted. Because the queue is emitted on BOTH 60fps present passes, the shadow map is
  // rebuilt identically on each — no keep_shadow side-channel, no strobe. View space = (x=vx, y=vy, z=pz),
  // the metric view space the deferred/light pass reconstructs; never interpolated (B positions on both
  // passes — build_lerp leaves these untouched), per the user's "shadows are not interpolated" design.
  uint8_t sh_cast;                    // 1 = opaque world prim that casts a shadow (push sh_v* as two tris at emit)
  float sh_vx[4], sh_vy[4], sh_vz[4]; // view-space verts (the shadow VBO input)
};

// Per-frame prim capacity. Measured worst case in real play (later-273): steady-state field ≈ 1k prims/frame,
// but the AREA-TRANSITION frame (first field-load frame, sm[0x4e]→9) spikes to ≈ 43k as the whole area's
// geometry is submitted at once. 32768 was too small for that transient (it silently dropped prims). Sized
// to comfortably hold the observed worst case with headroom; push() FAIL-FASTS above this (a true runaway —
// e.g. a stuck render walk re-submitting forever — is a real bug, not a drop-and-continue).
#define RQ_MAX 65536

// CONTEST — are two faces of ONE object a pair whose relative order the depth buffer cannot be
// trusted with? Symmetric in A and B. Two distinct rules (same OT bucket + exactly coincident, or
// differing keys with the farther-keyed face interpolating nearer somewhere both cover) — see the
// definition in render_queue.cpp. Exposed so the test suite can assert resolveKeyOrderFaces against
// a brute-force existence oracle built from this same predicate.
bool rq_faces_in_contest(const RqItem &A, const RqItem &B);

// Per-instance (on Game) so two cores keep independent queues; pure host render data (never guest RAM),
// so it does not affect a Core::ram lockstep diff.
struct RenderQueue {
  Game *game = nullptr; // back-pointer wired in Game() so methods can reach Core (game->core)
  RqItem items[RQ_MAX];
  int n = 0;
  uint32_t seq = 0;
  // MONOTONIC PUSH ODOMETER — never reset, by design. `n` and `seq` both go back to 0 on the lazy
  // first-push-of-a-frame reset inside push(), so "prims this call emitted" measured as a delta of `n`
  // reads NEGATIVE whenever the call straddles that reset. That is not hypothetical: it is how the
  // first version of the redirect prim census reported "prims=-51 ... EMPTY=1" and would have reported
  // a false "this draw emitted nothing" (#103). Anything asking how many prims a given call pushed must
  // difference THIS, which only ever goes up.
  unsigned long long pushed_total = 0;
  int consumed = 1; // start consumed so the first push begins a clean frame
  void reset();
  RqItem *push();              // NULL on overflow (reserves a slot; lazy per-frame reset)
  void flush(Core *core);      // sort by (layer, seq), then capture (fps60) OR emit each, mark consumed
  void sortQueue();            // stable_sort items by (layer, seq) — the engine draw order (fps60 mid-present)
  void emitQueue(Core *core);  // emit each item to the VK rasterizer + mark consumed (no sort)
  void histogram();            // `debug rqhist` layer x opaque/semi census; called from flush(),
                               // never emitQueue() — fps60 never reaches emitQueue (see the .cpp)
  void zfightScan(Core *core); // PSXPORT_ZFIGHT diag: SW-rasterize opaque depth prims, find near-equal top-2 contests
  void mark_consumed();
  PainterObjectPlan buildPainterObjectPlan(PainterObjectLimits limits = {}) const;

  // The screen space of the 2D quads being pushed RIGHT NOW. Producers that author 4:3 layout — the
  // overwhelming majority: HUD, menus, panels, dialogue — leave it alone; a producer whose x is
  // already wide-final raises a Space2dScope around its pushes. Per-RenderQueue (never a file-scope
  // flag) so SBS's two cores cannot see each other's scope.
  Rq2dSpace m2dSpace = RQ_2D_AUTHORED_4_3;
  // CAPTURED vs PRESENTED, per layer (present_ledger.h). Per-RenderQueue so SBS's two cores cannot
  // see each other's counts, same as m2dSpace.
  PresentLedger mLedger;
  PainterObjectId mPainterObject = 0;
  uint8_t mPainterFlags = PAINTER_OBJECT_NONE;
  uint16_t mPainterScopeDepth = 0;
  bool mPainterInvalidId = false;
  bool mPainterRegrouping = false;

  class PainterObjectScope {
  public:
    PainterObjectScope(RenderQueue &rq, PainterObjectId id, uint8_t flags = PAINTER_OBJECT_NONE)
        : mRq(rq), mPrevId(rq.mPainterObject), mPrevFlags(rq.mPainterFlags) {
      ++rq.mPainterScopeDepth;
      if (!id) {
        rq.mPainterInvalidId = true;
      }
      rq.mPainterObject = id;
      rq.mPainterFlags = flags;
    }
    ~PainterObjectScope() {
      --mRq.mPainterScopeDepth;
      mRq.mPainterObject = mPrevId;
      mRq.mPainterFlags = mPrevFlags;
    }
    PainterObjectScope(const PainterObjectScope &) = delete;
    PainterObjectScope &operator=(const PainterObjectScope &) = delete;

  private:
    RenderQueue &mRq;
    PainterObjectId mPrevId;
    uint8_t mPrevFlags;
  };

  // RAII declaration of the screen space of everything pushed inside it. Restores the previous value
  // rather than resetting to the default, so a producer nested inside another cannot silently
  // re-space its parent's remaining quads.
  //
  //   { RenderQueue::Space2dScope wide(rq, RQ_2D_WIDE_FINAL);   // x came from the widened camera
  //     ...pushes... }
  class Space2dScope {
  public:
    Space2dScope(RenderQueue &rq, Rq2dSpace space) : mRq(rq), mPrev(rq.m2dSpace) {
      rq.m2dSpace = space;
    }
    ~Space2dScope() {
      mRq.m2dSpace = mPrev;
    }
    Space2dScope(const Space2dScope &) = delete;
    Space2dScope &operator=(const Space2dScope &) = delete;

  private:
    RenderQueue &mRq;
    Rq2dSpace mPrev;
  };

  // push2dQuad: enqueue a 2D textured quad (HUD / overlay / background) into the render queue so it is
  // part of THE FRAME and gets re-emitted on both 60fps present passes (no direct gpu_vk_draw_tritri
  // bypass that lands on one pass). layer = RQ_BACKGROUND/RQ_OVERLAY/RQ_HUD; order_2d_fg picks the 2D
  // far/near band. Body in render_queue.cpp.
  void push2dQuad(int layer,
                  int order_2d_fg,
                  const int *xs,
                  const int *ys,
                  const int *us,
                  const int *vs,
                  const unsigned char *rs,
                  const unsigned char *gs,
                  const unsigned char *bs,
                  int tp_x,
                  int tp_y,
                  int mode,
                  int raw,
                  int clut_x,
                  int clut_y,
                  int tw_mx,
                  int tw_my,
                  int tw_ox,
                  int tw_oy,
                  int da_x0,
                  int da_y0,
                  int da_x1,
                  int da_y1,
                  int semi = 0); // semi=1 -> blended quad (the guest's GP0 0x62-class semi-transparent prims)

  // emitItem: emit one resolved item to the VK rasterizer. Used by both the inline path and the
  // queue flush so emission logic lives in one place.
  void emitItem(Core *core, const RqItem *it);

  // emitOrQueue: build an RqItem from already-resolved quad/tri data + material snapshot, then either
  // queue it (`capture`, engine owns the order, flushed at the draw kick) or emit it now. The ONE place
  // the submit paths (world quad, guest poly, guest sprite) funnel through.
  void emitOrQueue(Core *core,
                   int capture,
                   int layer,
                   int order_mode,
                   int nv,
                   int semi,
                   int raw,
                   const int *xs,
                   const int *ys,
                   const float *xsf,
                   const float *ysf,
                   const int *us,
                   const int *vs,
                   const unsigned char *rs,
                   const unsigned char *gs,
                   const unsigned char *bs,
                   const float *depth,
                   int mode,
                   int tp_x,
                   int tp_y,
                   int clut_x,
                   int clut_y,
                   int tw_mx,
                   int tw_my,
                   int tw_ox,
                   int tw_oy,
                   int da_x0,
                   int da_y0,
                   int da_x1,
                   int da_y1,
                   int tp_blend,
                   const float (*sv)[3] = nullptr,
                   int sort_key = -1,
                   float key_ord = 0.0f,
                   int shade_gouraud = 0,
                   int dither = 0);

  // drawWorldQuad: PC-native world-quad draw — a quad already projected to FLOAT screen coords + real
  // per-vertex depth, teed as two triangles to the VK rasterizer through the queue. No GP0 packet, no
  // OT, no guest write. sort_key/key_ord: the game's own OT sort key for this face (see RqItem), -1 =
  // none (producers with no guest sort key — terrain, mesh overlays — keep pure per-vertex depth).
  void drawWorldQuad(Core *core,
                     const float *px,
                     const float *py,
                     const float *depth,
                     const int *u,
                     const int *v,
                     const unsigned char *r,
                     const unsigned char *g,
                     const unsigned char *b,
                     uint16_t tp,
                     uint16_t clut,
                     int semi,
                     const float (*sv)[3],
                     int sort_key = -1,
                     float key_ord = 0.0f);

  // GAME-SORT-KEY ORDER RESOLUTION (kanban #11) — see the implementation banner in render_queue.cpp.
  // Runs once per frame at flush: within each object, finds face pairs whose interpolated per-vertex
  // depth would CONTRADICT the game's own sort-key order and snaps those faces' test depth to their
  // key's shared ord. Zero bias, zero constants: every value is the game's own computation.
  void resolveKeyOrder(Core *core);

  // The algorithm behind resolveKeyOrder, with no Core dependency — `frame` only labels diagnostics.
  // Split out so the rule can be tested on its inputs alone (tests/test_render_queue_keyorder.cpp):
  // build a queue, push the faces, call this, inspect the snap decisions.
  void resolveKeyOrderFaces(uint32_t frame);

  // Pair tests performed by the last resolveKeyOrderFaces call. The rule asks an EXISTENCE question
  // per face ("is this face in contest with any other face of its object"), so this must scale with
  // the number of keyed faces, NOT with their square — an exhaustive pairwise scan is what wedged
  // the Tomba!2 frame loop at gpu f1822 (596,134,804 tests in one frame). Gated on by the test suite.
  uint64_t keyOrderPairTests = 0;

  // finalize — turn a fully-submitted queue into a DRAWABLE one: resolve the game's authored face
  // order, then sort. THE one place that answers "what does a finished queue look like", so a real
  // frame and an interpolated frame cannot drift apart in how they are built (the fps60 principle:
  // no difference between the two except the lerp, and the lerp lives in the INPUTS). Both callers
  // are the two queue-finishing sites: RenderQueue::flush for the real frame, and Fps60::tier1Render
  // for the re-rendered world of the in-between frame. Adding a third ordering step belongs HERE, not
  // at a call site — kanban #17 is what happens when it doesn't (resolveKeyOrder ran only on the real
  // frame, so the barrel's snapped faces alternated every other frame and flickered at 60fps).
  void finalize(Core *core);

private:
  // Debug OBJECT-ID overlay (REPL `debug objid`) — pure host HUD quads appended at flush time.
  static bool objidSolid(
      Core *core, const RqItem *ref, int x0, int y0, int x1, int y1, unsigned char r, unsigned char g, unsigned char b);
  static void objidChar(
      Core *core, const RqItem *ref, char ch, int x, int y, int s, unsigned char r, unsigned char g, unsigned char b);
  static void objidStr(Core *core,
                       const RqItem *ref,
                       int x,
                       int y,
                       int s,
                       const char *str,
                       unsigned char r,
                       unsigned char g,
                       unsigned char b);
  static void objidBox(Core *core,
                       const RqItem *ref,
                       int x0,
                       int y0,
                       int x1,
                       int y1,
                       unsigned char r,
                       unsigned char g,
                       unsigned char b,
                       int t);
  void objidOverlay(Core *core);
};

int rq_active(void); // PSXPORT_RQ — route owned world geometry through the queue

#endif
