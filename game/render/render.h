// class Render — the PC-native RENDER SUBSYSTEM umbrella owned by Core.
//
// PROPER OOP: one instance per Core, reached as `rend(c)->...`. This class exists to group the
// per-Core render-side subsystems (currently just NodeXform; scene-graph submit / walk / project etc.
// will migrate in here over time, out of the submit.cpp / render_frame.cpp / render_walk.cpp grab-bags).
//
// Owned by Core via a POINTER (`Core::mRender`) — construction/destruction lives in the Core ctor/dtor
// in runtime/recomp/core.cpp; back-pointer `mCore` is wired there, and each embedded sub-subsystem's
// own back-pointer is wired there too. Callers reach members as `rend(c)->mNodeXform.build(node)`.
#pragma once
#include "node_xform.h"
#include "projection.h"     // EObjXform (per-Core active per-object xform lives on Render below)
#include "render_native.h"      // class NativeScenePass — the decoupled native render subsystem
#include "margin_render.h"    // class MarginRenderer — widescreen margin collect-and-flush
#include "lighting.h"           // class Lighting — per-area light registry (sun / lava+torch)
#include <unordered_set>
#include <vector>
class Core;

class Render {
public:
  Core* mCore = nullptr;

  // ---- render-side per-Core subsystems ------------------------------------
  // (The host-only render SUBSTRATE members — mode/diag/pktSpan/otAttr/dualviewSnapshot/stats/projprim/
  // pgxp/projParams — moved to the framework-owned `class RenderSubstrate` (runtime/recomp/
  // render_substrate.h), reached as `mCore->rsub.<member>`, so the framework no longer includes this
  // game umbrella just to reach a projection cache or compare-mode toggle. Byte-neutral host layout.)
  // Active per-object xform for the GT3/GT4 submitters. Set once per render command by the per-object
  // flush (projSetActive), read by the per-vertex projection (projVertexActive), cleared by
  // projClearActive. Was file-scope in projection.cpp; per-Core here so SBS's two cores don't
  // share a transform between their emits (2026-07-03).
  EObjXform         mActiveXform{};
  bool              mActiveXformSet = false;
  NodeXform         mNodeXform;        // scene-node WORLD-TRANSFORM builder (guest FUN_80051844)
  // PSXPORT_BDTAG per-node attribution names for the walk dispatch cases (render_walk.cpp).
  // The gp0 classify is deferred one frame, so the names live in a per-Core ring, not walk locals.
  NativeScenePass   mNativeScene;      // decoupled native render pass (collect + drawObject)
  MarginRenderer    margin;            // widescreen margin re-include (collect in cull, flush post-walk)
  Lighting          lighting;          // per-area light registry (selected once per frame by shadeSelect)
  // Light config selected for this frame by shadeSelect(); the hot per-face shading routine reads the
  // cached pointer instead of re-reading guest RAM. Falls back to the SUN default when unset.
  const LightConfig* mShadeCfg = nullptr;

  // AREA-SCOPED CACHE trust latches — read-only, host-only (no guest writes). Two guest structures
  // that a per-area/per-beat SETUP step (re)populates and a per-tick STEP function refreshes while its
  // owner is active — SCENE_ENT_TABLE (0x800F2418: count@+6, grid-ptr@+0xC, refreshed by the SOP
  // cam-frustum prepass) and PARALLAX_BG_SM (0x800ED018: W@+0x10/H@+0x11, tilemap-ptr@+0x14, refreshed
  // by the backdrop tile scroller) — go STALE for a handful of ticks at the exact moment SOP narration
  // hands off to the field-area load: the narration's own prepass/scroller stops ticking them, the
  // field-area machine's equivalent (still substrate) hasn't started yet, and the just-loaded field
  // overlay has REPURPOSED the memory their leftover pointers still reference. Reading through a stale
  // pointer into repurposed bytes is garbage geometry / a garbage tilemap (found via the narration-end
  // -> fisherman-cutscene loading-screen bug: pc_render showed a tiled noise/atlas-grid garbage frame
  // where the oracle — pure PSX render, same recomp gameplay — shows the expected black load-hold,
  // because the PSX render path simply isn't drawing from these structures during the same window).
  //
  // Both structures happen to get explicitly RE-ZEROED (count/W momentarily 0) as part of the SAME
  // "next area setup" step that eventually repopulates them fresh — that zero is the observable proof
  // their owner has taken over again. Track (per structure) whether that zero has been seen since the
  // last narration -> field handoff before trusting a nonzero value; each structure's own EXISTING
  // "count==0 / W==0 -> skip" guard already makes the zero-window itself safe to read. Mirrors the
  // ScreenFade held-fade latch: pure state derived from guest READS, no guest writes, per-Core host
  // memory only.
  bool mSceneTableTrusted  = false;   // may fieldEntityRender(SCENE_ENT_TABLE) run this frame?
  bool mBackdropTrusted    = false;   // may render_bg_tilemap_native(PARALLAX_BG_SM) run this frame?
  bool mAreaCacheWasNarration = false;   // previous frame's sop_narration value (shared edge detector)

  // NATIVE-DRAWN-NODE provenance (bug #48, docs/findings/render.md "Z-fight sweep 2026-07-14"):
  // node addresses Render::sceneNative's object loop WILL draw this frame via perObjFlush (the real
  // per-object float path — real identity + real per-vertex depth). cmdListDispatch
  // (perobj_dispatch.cpp) walks the SAME per-node cmd list (node+8 count, node+0xC0 cmd array,
  // geomblk=cmd+0x40) from the substrate walk cluster (perObjRenderDispatch's cases), so any node
  // present here has its substrate guest-OT emission made fully redundant — register it into the
  // native-cover span registry instead of letting it fall through to obj_depth's billboard
  // promotion (which produced the observed exact-duplicate class: the SAME mesh painted twice, once
  // by perObjFlush with real identity/depth, once by the guest-OT copy at a flat billboard depth
  // with dbg_node==0). PROVENANCE-DRIVEN: nativeObjDrawn re-derives this set straight from guest
  // state using perObjFlush's OWN inclusion test (see render_walk.cpp's banner for why it can't just
  // be a registry perObjFlush writes — call ordering), never a heuristic address-range/list-
  // membership guess — objects on OTHER walk lists (e.g. the Bcf4 aux list) that this test never
  // visits are simply absent from the set and stay uncovered, exactly as before. Cached per s_frame.
  std::unordered_set<uint32_t> mNativeDrawnNodes;
  int mNativeDrawnFrame = -1;
  bool nativeObjDrawn(Core* c, uint32_t node);   // cmdListDispatch: will perObjFlush draw this node this frame?

  // ---- object-render projection ops (impl in projection.cpp) ----------
  // Compose an EObjXform from the object's REAL WORLD coordinates: its world rotation matrix (cmd+0x18)
  // and world position (cmd+0x2C), transformed by the live scene camera (scratchpad view matrix
  // 0x1F8000F8 / translation 0x1F80010C). Projection constants are the camera's (CR24-26). No gte_op.
  void projComposeObject(uint32_t cmd, EObjXform* out);
  // Host-supplied-transform variant: composes the same camera-x-object core as projComposeObject, but
  // Robj/Tobj are HOST float values the caller already computed (margin's read-only path — a culled
  // node's cmd+0x18/0x2C are stale, so we can't read them from guest RAM). Camera state is still read
  // from scratchpad (never stale). No guest reads of Robj/Tobj, no guest writes anywhere.
  void projComposeObjectHost(const float Robj[3][3], const float Tobj[3], EObjXform* out);
  // Compose an EObjXform from the scene camera ALONE (no per-object matrix) — for geometry already in
  // WORLD space (field entity render loop), where view = Rcam·world + Tcam directly. No gte_op.
  void projComposeCamera(EObjXform* out);
  // Per-command active xform (owned by the GT3/GT4 submitters).
  void projSetActive(const EObjXform* w);
  void projClearActive();
  bool projActive() const { return mActiveXformSet; }
  void projVertexActive(int vx, int vy, int vz, ProjVtx* out);
  // Pack the ACTIVE float xform into the CR0-7 + CR24/25/26 layout the fps60 midpoint reprojection consumes.
  void projActiveCr(uint32_t cr[11]);

  // ---- per-frame render orchestrators (called by Engine::fieldFrame/X) ----
  // frame  (guest 0x8003F9A8) — the primary per-frame render orchestrator; runs the non-walk PSX
  //   passes (the walk cluster is owned by SceneNative::render in render_walk.cpp).
  //   Was ov_render_frame in render_frame.cpp.
  // frameX (guest 0x8003FA44) — the mid-transition variant (reduced pass set). Was ov_render_frame_x.
  void frame();
  void frameX();

  // abortUnimplemented (USER 2026-07-15): FAIL-FAST for the ONE native renderer. pc_render produces the
  // picture ONLY from native scene producers (sceneNative for the field world, …). A scene — or a scene
  // LAYER (e.g. the field's 2D HUD/dialog/menu overlay) — with no native producer must NOT fall back to
  // transcribing the guest OT/GP0: it aborts here with a precise identity (stage / sm[0x4a] / sm[0x4c] /
  // overlay signature) so the crash sequence IS the native-renderer work queue. There is no fallback and
  // no env escape hatch; the guest-OT walk survives ONLY under psx_render (the reference renderer).
  [[noreturn]] void abortUnimplemented(const char* scene);

  // perObjFlush: per-object native GT3/GT4 flush — composes the float camera×object transform from
  // the object's real world coords and submits every geomblk cmd on node+0xC0 through gt3gt4.
  // Taxi-parameter c->r[4] = node (recomp-shaped body, mirrors the guest ABI).
  void perObjFlush();
  // perObjFlushPreComposed: perObjFlush for the F174-class node types whose cmd+0x18 holds a
  // PRE-COMPOSED camera∘object MATRIX (renderWalk table types 1/4 — text labels etc.), factored
  // against the scene camera (wq_factor_world) back to a world transform so the native compose
  // (and the fps60 interp re-run's lerped camera) applies the camera exactly once.
  void perObjFlushPreComposed();

  // terrain (guest 0x8002AB5C): the field terrain render entry. Picks the area's light config for
  // the frame, then either super-calls the recomp body (dual-core diff neutralize path) or runs the
  // PC-native float terrain render. Taxi-parameter c->r[4] = node. Was ov_terrain.
  void terrain();

  // terrainRenderAll: scan the field's render-list (0x800F2624) for terrain nodes (render fn ==
  // 0x8002AB5C) and call terrain() on each — the ENUMERATION half of sceneNative's terrain block. ONE
  // call sequence used by both: the real per-logic-frame walk (render_walk.cpp sceneNative) and Fps60's
  // Tier-1 present-time camera-lerp re-render (fps60.cpp present_vk) — see docs/fps60-rework.md
  // "Object-tier attempt ... Why Tier 1 isn't built" for why re-invoking the SAME sequence (not a
  // parallel one) is what makes the lerped-camera re-render provably the same render path.
  void terrainRenderAll();

  // Per-frame WORLD-pass gates, shared by the real render (sceneNative) and the fps60 interp re-render
  // (Fps60::tier1Render) so the in-between can never draw a pass the real frame skipped (#67 bug class:
  // the interp re-run bypassing a real-frame gate paints content only every other present). Pure guest
  // READS of state unchanged since the real frame (fps60's present-time invariant).
  bool worldVoidBeat() const;   // SOP narration VOID beat (0x800bf9b4==5): no terrain/scene-table/backdrop
  bool fieldAreaInit() const;   // GAME field-area object-placement init frame: models unattached, no world

  // shadeSelect: pick this area's light config once per world frame (cheap guest-RAM fingerprint via
  // Lighting::areaKeyFrom); caches the result in mShadeCfg for the per-face shading routine.
  void shadeSelect();

  // gt3gt4 (gen_func_800803DC's first body): the generic GT3/GT4 renderer — split the geomblk's packed
  // prim counts (low16 tri, high16 quad) and run the two native submitters in sequence.
  void gt3gt4(uint32_t geomblk, uint32_t otbase);

  // sceneNative: the master scene-render walker (per-frame terrain + entity/object walk over
  // the 3 doubly-linked object lists, with backdrop + collectable-quad + native BG tilemap).
  // Called by game_tomba2.cpp's ov_draw_otag every field-stage frame (and by margin_render's
  // widescreen re-include pass). Was ov_scene_native.
  void sceneNative();

  // ---- pc_render scene DISPATCH: classify the current scene, then run its native producer -------------
  // The picture for every pc_render frame comes from ONE of these per-scene producers (no guest-OT
  // transcription). drawOTag (game_tomba2.cpp) calls renderScene() after the psx_render reference guard;
  // renderScene classifies by stage/sub-state and dispatches. Each producer owns its DisplayPassGuard +
  // fps60 eligibility. A stage with no producer aborts (abortUnimplemented) — the crash IS the backlog.
  enum class SceneKind { Loading, StartBoot, Title, Field, HutInterior, SopNarration, Unknown };
  SceneKind classifyScene();     // stage 0x801FE00C + sub-state selectors -> which scene this frame is
  void renderScene();            // classify + dispatch to the producer below
  void renderLoading();          // #0 task-switch handoff — black loader (task not yet initialized)
  void renderStartBoot();        // #1 START.BIN loader — black frame
  void renderTitle();            // #2 DEMO/title front-end (s2 = titleNative; other substates = black)
  void renderField();            // #3 walkable field — native world (sceneNative)
  void renderHutInterior();      // #4 hut/door authored sub-scene — objects-only (fieldObjectsRender)
  void renderSopNarration();     // #5 SOP intro narration — native world (sceneNative + void-beat guard)

  // Dialog/prompt TEXT is produced by the FUN_8007CC00 tap (Panel::pushDialogGlyphs, game/ui/
  // panel.cpp) — fires whenever the guest dialog emitter draws, with the highlight palette the
  // earlier flat-list producer here couldn't see (it lacked the DialogBox pointer).

  // titleNative: the DEMO/title (stage 0x801062E4, substate sm[0x48]==2) read-only native producer.
  // The title = a black backdrop + 2 logo sprites (op 0x65, fixed layout, fn 0x8010696C) + 3 menu FT4
  // quads (fn 0x8007E2F8) + native font text. Its builders write GUEST packets (byte-exact) which
  // pc_render does not walk; titleNative re-emits the same picture to the native queue from source state
  // (constants / menu-cursor state), host-only. See docs/native-render-rebuild.md #2. Font/menu emit
  // lands incrementally (font-glyph dual-emit + owning 0x8007E2F8); logo is exact from decoded packets.
  void titleNative();

  // s3MenuNative: the DEMO/title front-end SECOND menu page (sm[0x48]==3, reached from the title by
  // confirming New Game). RE'd (docs/findings/render.md '#2b'): Demo::s3 unconditionally draws the SAME
  // chrome + a page-1 menu (FUN_80106824(param1=1) -> item templates 0x90/0x91) — NOT the SOP narration.
  // Read-only producer; built from the shared data-driven menu emitter below.
  void s3MenuNative();

  // --- shared DATA-DRIVEN menu emitter (reproduces the guest menu builders, read-only) --------------
  // menuChrome: black backdrop + the 2 logo sprites (FUN_80106690) shared by every front-end menu page.
  void menuChrome();
  // menuItemsAndCursor: reproduces FUN_80106824(param1, param2) — the cursor (template 0x98) + the two
  // item text-images (templates {0x8e,0x8f} for page 0 / {0x90,0x91} for page 1), with the selected item
  // RAW (bright) and the other modulated 0x50 (dim), per the live selection. param2 selects which item is
  // bright AND indexes the cursor-X table @0x80107704.
  void menuItemsAndCursor(int param1, int param2);
  // emitMenuFt4: reproduces FUN_8007e1b8 (POLY_FT4 case) — resolve a menu template (idx -> table
  // @0x80017334 -> header @base 0x80158000 -> `count` 16-byte entries) and emit each as a native FT4 quad,
  // reading the game's OWN geometry (position/uv/clut/tpage) from guest RAM. attr high-nibble 0 -> RAW,
  // else modulated by (attr,attr,attr). Decoder validated field-for-field against the title's known-good
  // quads (templates 0x8e/0x8f/0x98). Retires the hand-decoded per-item constants titleNative used.
  void emitMenuFt4(int anchorX, int anchorY, uint32_t templateIdx, uint32_t attr, int layer);

  // narrationSwirlRender: native producer for the SOP intro-narration full-screen SWIRL — the type-0x20
  // render node (0x800ED9E8) whose CUSTOM render fn (node+0x18 = 0x8010BF54, SOP overlay) the substrate
  // walk dispatches via its default case. Draws the 36-byte quad-record mesh @0x8010CC08 twice (two
  // blades, +0x800 yaw apart) with the RE'd transform chain in host float. Read-only; real depth.
  // See game/render/narration_swirl.cpp for the full RE. Dispatched from fieldObjectsRender's walk.
  void narrationSwirlRender(uint32_t node);

  // cineBarsRender: native producer for the cinematic LETTERBOX bars (UI-effect manager slot type 1,
  // base 0x80100400). Reads the slot table read-only and emits the top/bottom black bars. Emits nothing
  // when disarmed. See game/render/cine_bars.cpp. Call from cutscene-capable scenes.
  void cineBarsRender();

  // fieldObjectsRender: the field's OBJECT pass — walk the 3 entity lists + Tomba's G-block and flush each
  // live object's geomblk (perObjFlush → projComposeObject → gt3gt4). Factored OUT of sceneNative (which
  // mutates per-frame trust latches and can't be re-run) so Fps60's interp present can RE-RUN it under the
  // lerped per-object transforms (mObjOverrideOn) — the SAME object walk the real frame ran, replacing the
  // matchAndLerp output heuristic (docs/fps60-rework.md step 2b). Pure reads + queue emits, no state mutation.
  void fieldObjectsRender();

  // ---- BILLBOARD display-pass producer (#67 RE work — REDIRECT doctrine, USER: both frame kinds
  // derive from game state) ---------------------------------------------------------------------
  // The billboardEmit particle system (perobj_billboard.cpp — AP gems / flames / apples / splash /
  // windmill sprites) runs at GUEST-EXECUTION time inside the substrate render walk; its packets are
  // guest state, but its PICTURE must come from the display pass so the fps60 interp re-run derives
  // it under lerped inputs like every other world prim. Each real frame, billboardEmit RECORDS one
  // BbRec per emitted particle (host memory only): the particle's local quad corners (the ×5 ints
  // func_8003B220 built), the node's composed MAT_OUT rotation + world anchor, and the RESOLVED
  // material words (post node+92 override + node+13 case patches). billboardsRender (called from
  // fieldObjectsRender, so field + hut + tier1Render's interp re-run all reach it) projects each
  // record through the SAME float camera path the world uses (sceneCam choke — fps60-lerped at the
  // interp present) and emits an RQ_WORLD quad with real per-particle identity (dbg_node=node).
  struct BbRec {
    uint32_t node, particle;              // identity (node = dbg_node; particle addr = stable key)
    int16_t  cx[4], cy[4];                // local corner ints (already ×5; z=0 in local space)
    int16_t  rotR[3][3];                  // live CR0-4 rotation at emit, 4.12 fixed (row-major)
    float    wx, wy, wz;                  // world anchor, factored from live CR5-7 (camᵀ·(tr−camT))
    uint32_t wColor, wUv0, wUv1, wUv2, wUv3;   // resolved record words BUF+4/+12/+20/+28/+36
  };
  std::vector<BbRec> mBbRecs;             // this logic frame's records, guest-walk emit order
  // (No BbRec prev buffer: effect particles have no stable cross-frame identity — the sub-lists
  // reuse/walk particle addresses — so they never lerp; they draw at their own frame's state under
  // the lerped camera. WqRecs below DO lerp, keyed by stable (node, per-node seq).)

  // WqRec — the GENERIC composed-CR quad record (QuadRtptSubmit::submitQuad's classes: the A00-overlay
  // flame/rope emitter 0x801341xx, case188 particles, B704 beams). At capture the composed GTE
  // transform is FACTORED against the scratchpad scene camera (which stays pure while only the GTE
  // CRs get per-object composes): CR = cam∘obj, tr = cam·pos + cam.t  ⇒  objR = camᵀ·CR/4096,
  // objT = camᵀ·(tr − camT). The display pass re-composes with the (fps60-lerped) camera:
  // corner_view = cam'·(objR·corner + objT) + camT' — exact at the endpoints for ANY CR content,
  // world-glued in between. One mechanism for every submitQuad caller, no per-emitter RE needed.
  struct WqRec {
    uint32_t node;                        // identity (dbg_node)
    uint32_t seq;                         // per-node emission index this frame (lerp identity key)
    float    objR[3][3];                  // factored world rotation (unit scale)
    float    objT[3];                     // factored world position
    int16_t  vx[4], vy[4], vz[4];         // model-space corners (the xf words submitQuad projected)
    // Material record words. wCol[0] carries the code byte (>>24: semi/raw bits); FT4 emitters fill
    // all four with the same flat word, the GT4 emitter (widescreen_margin_quad) per-vertex colors.
    uint32_t wCol[4];
    uint32_t wUv0, wUv1, wUv2, wUv3;      // uv words (clut in wUv0>>16, tpage in wUv1>>16)
  };
  std::vector<WqRec> mWqRecs;
  std::vector<WqRec> mWqRecsPrev;

  void bbFrameReset() { mBbRecs.clear(); mWqRecs.clear(); }          // per logic frame, pre-walk
  void bbSwapPrev()   { mWqRecs.swap(mWqRecsPrev); }                 // fps60 rotation (WqRec lerp source)
  void billboardsRender();                // display-pass producer: project + emit every BbRec + WqRec

  // textLabelEmit (FUN_80039F4C, text_label.cpp): the per-character 3D text-label renderer —
  // faithful substrate-mirror orchestrator (mesh pass + per-char glyph packets, all callees still
  // substrate) + WqRec display-pass capture per surviving glyph. Replaces the RenderObserver wrap.
  void textLabelEmit();

  // fieldEntityRender: world-space GT3/GT4 scene-table renderer. Walks the entity-list struct at
  // `es` (per-list ptr headers at es+0x10..; packed geometry base at es+0xC; count at es+6),
  // dispatches each entry through submit_poly_gt3_native / submit_poly_gt4_native under the
  // camera-composed eproj xform. Called for the ground scene table 0x800F2418 (in Render::frame
  // groundnative diag branch and in the field object walk in render_walk). Was
  // ov_field_entity_render (taxi-parameter c->r[4]).
  void fieldEntityRender(uint32_t es);

  // backdropRender (guest overlay FUN_80115598, the seaside field's state-0 background drawer): draws
  // the scrolling sky/parallax tilemap as native RQ_BACKGROUND quads. Reads the PARALLAX_BG_SM struct at
  // `t4`; its only per-frame-varying inputs are the scroll offsets at t4+0x28/+0x2A (game-logic-driven —
  // ParallaxBg::step, not camera projection), read through Fps60::bgScroll so Fps60's Tier-1 present-time
  // re-render (fps60.cpp tier1Render) can feed it a wrap-aware lerped scroll instead of a guest read. Was
  // the file-local `render_bg_tilemap_native` (render_walk.cpp); promoted to a method so tier1Render can
  // call the SAME pass, mirroring terrainRenderAll/fieldEntityRender.
  void backdropRender(uint32_t t4);

  // ---- SUBSTRATE MIRROR: per-object cmd-list dispatch (guest FUN_8003CDD8 / FUN_8003F698) --------
  // These run UNDER the render-underneath architecture (issue #32): the substrate walk cluster calls
  // them as PLAIN intra-shard C calls (func_8003CDD8/func_8003F698), never through rec_dispatch, so
  // they are owned via the shard override table (shard_set_override) — same mechanism RenderObserver
  // already uses for the sibling 0x8003xxxx render leaves. Guest-writing here is LEGAL/REQUIRED (the
  // scratchpad GTE compose + the OT/packet-pool writes made by the callee are part of BOTH SBS cores'
  // faithful RAM) — this is NOT the read-only pc_render display pass. See perobj_dispatch.cpp for the
  // full RE (byte-exact substrate mirror via the shared gte_op/gte_write_ctrl primitives, not a
  // pc_render rebuild).
  // cmdListDispatch (FUN_8003CDD8): a0=node (r4), a1=flag (r5). For each active render command on the
  // node's persistent list, composes the WORLD object transform (camera-rot x object-local, via the
  // same MVMVA ops submit.cpp's later-133 comment already RE'd) into CR0-7, then dispatches through
  // perModeDispatch.
  void cmdListDispatch();
  // perModeDispatch (FUN_8003F698): a0=geomblk (r4), a1=otbase (r5), a2=flag (r6). Selects the area's
  // per-mode renderer via the mode-select byte + jump table, or falls back to the generic GT3/GT4
  // packet emitter (func_800803DC) when the generic-force flag or a2&1 is set.
  void perModeDispatch();

  // ---- SUBSTRATE MIRROR: per-object render-type dispatch + billboard/special-effect leaves --------
  // Guest FUN_8003CCA4 / FUN_8003C2D4 / FUN_8003C464 / FUN_8003C8F4 — the walk cluster's per-object
  // TYPE dispatch (CCA4) and 3 of its "special effect" leaf renderers (C2D4/C464/C8F4 build a
  // camera-composed billboard quad per particle and emit it into the OT/packet pool — see
  // perobj_billboard.cpp for the full RE). Same ownership mechanism as cmdListDispatch/perModeDispatch
  // above: reached as plain intra-shard C calls, owned via shard_set_override. These were previously
  // wrapped by the transparent RenderObserver (depth-tag) wrapper — CCA4/C2D4/C464/C8F4 fold that
  // wrapping in directly now that they're native (see perobj_billboard.cpp), so RenderObserver no
  // longer wraps these 4 addresses.
  // perObjRenderDispatch (FUN_8003CCA4): a0=node (r4). Case 0/4 funnels here call cmdListDispatch();
  // the other cases wrap it with one of 5 still-substrate special-effect leaves.
  void perObjRenderDispatch();
  // billboardCompose1/2 (FUN_8003C2D4 / FUN_8003C464): a0=node (r4). Compose a pure Z-rotation "local"
  // matrix (C464 additionally seeds its base operand via the still-substrate FUN_800517BC) through the
  // persistent camera MATRIX, transform the node's world position, then hand off to billboardEmit.
  void billboardCompose1();
  void billboardCompose2();
  // billboardEmit (FUN_8003C8F4): a0=node (r4), a1=flag (r5). Walks the node's active particle
  // sub-list, RTPT/RTPS-projects each particle's quad corners, culls off-screen, buckets into the OT
  // by averaged depth, and emits a 10-word (tag+9) GT4-style packet into the packet pool.
  void billboardEmit();

  // ---- SUBSTRATE MIRROR: the render-WALK loop (guest FUN_8003C048) --------------------------------
  // renderWalk (FUN_8003C048, no args): iterates the global render-node list (head @0x800F2624, next
  // ptr @node+36), skipping dead nodes (mem8(node+1)==0) and out-of-range case indices
  // (mem8(node+11)>=33), and dispatches each live node through a 33-entry jump table (@0x800104B8) to
  // one of: perObjRenderDispatch/billboardCompose1/billboardCompose2 (owned siblings, called
  // natively), several still-substrate leaves (func_8003F174/EF9C/80039F4C/800726D4/C5F8/C788 +
  // rec_dispatch to 0x8012A43C/801295B4/80129114/8013DD58), a "generic particle" case (0x8003C188,
  // mode-4 direct dispatch or a func_8003B054+80084660/90+8003B320 packet-emit sequence), a fully
  // dynamic per-node dispatch through node+24 (case 0x8003C29C), and a no-op skip entry. THE point of
  // owning this loop: it is the caller CCA4Frame/CmdListFrame's register-faithfulness fixes assumed —
  // gen keeps the loop's node pointer and next pointer LIVE in the real r16/r17 registers (and two
  // constants in r18/r19) across every nested call, which the still-substrate callees' own prologues
  // spill as "caller state" (see perobj_billboard.cpp's CCA4Frame / perobj_dispatch.cpp's
  // CmdListFrame). See render_walk_dispatch.cpp for the full RE + the f118 residual this closes.
  void renderWalk();

  // ---- SUBSTRATE MIRROR: per-AREA-TYPE overlay dispatch (guest FUN_8003D0BC) -----------------------
  // overlayTypeDispatch (FUN_8003D0BC, a0=list — never touched by this fn's own body, plain
  // pass-through from the caller, gen_func_8003F9A8 passes SCENE_ENT_TABLE @0x800F2418): reads the
  // AREA_TYPE byte @0x800BF870 (the same render-mode-select byte perModeDispatch/renderWalk's case
  // 0x8003C188 both read); if >=22, no-op. Else dispatches through a 22-entry jump table
  // (@0x80014EF0) to one of 20 per-area-type overlay leaves — area type 0 reaches the ALREADY-OWNED
  // OverlayGroundGt3Gt4::entityLoop (FUN_801401B8) -> gt3/gt4; the other 19 are still-substrate. See
  // overlay_type_dispatch.cpp for the full RE.
  void overlayTypeDispatch();

  // ---- SUBSTRATE MIRROR: libgpu GPU-DMA completion-callback QUEUE (wide-RE, wired 2026-07-10) -----
  // 0x80082D04/0x80082FB4/0x80083364/0x80082424 — see game/render/wide_re_gpu_dma_queue.cpp for the
  // full RE (struct map, call graph, ring-entry layout). Reached as plain intra-shard C calls from
  // the substrate (NOT via rec_dispatch), owned via engine_set_override_main so the SBS oracle keeps
  // running the pure gen_func_* body.
  // Guest ABI args are read from c->r[4..7] inside each method (same convention as
  // perObjRenderDispatch/billboardCompose1 above) so the override thunk's `native(Core*)` shape
  // needs no per-address adapter.
  void gpuDmaQueueEnqueue();  // FUN_80082D04(fn,argValOrPtr,sizeBytes,arg3) -> queue depth / -1 timeout
  void gpuDmaQueueDrain();    // FUN_80082FB4() — also the GPU-DMA-completion ISR body
  void gpuDmaQueueSync();     // FUN_80083364(mode) — internal DrawSync-shaped blocking/poll
  void gpuDmaSend();          // FUN_80082424(arrayPtr,count) — the OT-linked-list DMA kick

  // ---- SUBSTRATE MIRROR: libgpu GPU-sys jump-table leaves DrawSync/ClearOTagR (wired 2026-07-10) ---
  // 0x80080F6C / 0x80081458 — see game/render/wide_re_libgpu_leaves.cpp for the full RE.
  void drawSync();     // FUN_80080F6C(mode)
  void clearOTagR();   // FUN_80081458(ot,entries)

  // ---- SUBSTRATE MIRROR: libgpu LoadImage()-internal chunked GP0-FIFO pixel streamer (wired 2026-07-10)
  // 0x80082734 — see game/render/wide_re_gpu_loadimage_streamer.cpp for the full RE (struct map,
  // control flow, dead-code note). Guest ABI: a0(r4)=rectPtr, a1(r5)=srcPtr; ret v0.
  void gpuLoadImageStream();  // FUN_80082734(rectPtr,srcPtr) -> -1 timeout/empty, else 0

  // ---- SUBSTRATE MIRROR: the 4 still-substrate OBJECT-LIST WALKERS reached from FUN_8003F9A8's
  // field-frame draw dispatcher (docs/findings/render.md "0x8003F9A8 474-prim attribution resolved") --
  // no args (guest ABI: none read), each walks a fixed object-pointer list/array, dispatching each live
  // entry's TYPE byte through a table to one of: the already-owned perObjRenderDispatch/
  // billboardCompose1/billboardCompose2 (this file's siblings), a handful of still-substrate leaves
  // (called as plain func_XXXX(c), matching gen), or a per-object vtable slot (rec_dispatch, per
  // CLAUDE.md — never dropped). See game/render/objlist_walk.cpp for the full RE (per-function
  // scratchpad cursor layout, jump-table addresses, case-label maps).
  void objListWalk1();          // FUN_8003BB50 — list @0x800F2410, cursor 0x1F80013C/146
  void objListWalk2();          // FUN_8003BCF4 — list @0x800F26C8, cursor 0x1F800148/152 (1st entry only)
  void objListWalk2Continue();  // FUN_8003BED8 — shared tail: continues objListWalk2's SAME guest frame
  void objListWalk3();          // FUN_8003BF00 — list @0x800F2738 (positional array), cursor 0x1F800154/15E
  void objListWalk4();          // FUN_8003EEC0 — list head *0x800F2738 (linked via node+0x24 "next")

private:
  // Native POLY_GT3/GT4 submitters (guest-ABI bodies: rec/otbase/count in r4/r5/r6).
  static void submitPolyGt3Native(Core* c);   // gen_func_8007FDB0
  static void submitPolyGt4Native(Core* c);   // gen_func_80080114
};
