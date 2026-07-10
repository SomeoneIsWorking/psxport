// game/render/fps60.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// TRUE PER-OBJECT interpolation (redesign 2026-07-10, user directive). The old tier was a post-hoc
// SCREEN-SPACE layer: it snapshotted the resolved render-queue prims, matched them across frames by a
// material fingerprint, and reprojected/translated the SCREEN verts at a crude packed-CR midpoint. It
// "looked like a hack" (billboards translated in screen space; terrain/backdrop juddered). That whole
// matcher is retired.
//
// The new tier interpolates at the OBJECT level and renders the in-between frame THROUGH THE REAL NATIVE
// SCENE RENDER (Render::sceneNative + the float projection path). Each logic frame the native render path
// captures, host-side, every object's WORLD transform (rotation matrix + position) and the scene CAMERA
// (view rotation + translation + projection constants) — the exact inputs the float projection consumes.
// For the mid-present, sceneNative is re-run with a MIDPOINT-transform provider armed: projComposeObject /
// terrain / backdrop consult this class and receive the t=0.5 lerp of (prev frame, this frame) instead of
// the raw guest value, so camera pan + object motion are reproduced perspective-correctly by the SAME
// projection that draws the real frame — no screen-space reproject. Billboards (guest-emitted 2D quads
// that reach the queue at the OT walk, not via sceneNative) carry their captured WORLD anchor position and
// are re-projected through the real projection at the interpolated anchor + interpolated camera.
//
// HOST-ONLY (the READ-ONLY OVERLAY invariant): every capture is a guest READ; every store is host memory;
// the mid-present re-runs sceneNative under a DisplayPassGuard so any stray guest write fails fast. When
// g_mods.fps60 is off, none of this arms and the 30fps path is byte-identical.
//
// De-globalization (2026-06-19): all state lives on this Fps60 instance owned by Game (game.h), reached
// via core->game->fps60; every touching function is a method (callers use c->game->fps60.method(...)).
#ifndef GAME_RENDER_FPS60_H
#define GAME_RENDER_FPS60_H
#include <stdint.h>
#include <unordered_map>

struct Core;
struct RqItem;
class Game;

// logic-rate detector (validated lrate_proto): votes on the number of frames each projected-geometry
// fingerprint is HELD, so the tier knows how many in-betweens to synthesize (Tomba2 logic = 30fps → 1).
typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;

// ---- Fps60 — the 60fps tier's per-instance interpolation state + methods ------------------------------
struct Fps60 {
  Game* game = nullptr;   // owner back-pointer (set in Game()) — gates via game->mods.fps60
  // ---- logic-rate detector (kept) --------------------------------------------------------------------
  uint64_t mFrameHash = 1469598103934665603ull;   // per-frame projected-geometry fingerprint (rate input)
  long     mFrameGeom = 0;                          // #verts folded this frame (0 => idle frame)
  long     mFence     = 0;                          // logic-frame counter
  RateDet  mRd = { 0, 0, 2, {}, 0 };
  void fold(uint32_t v);                            // fold a projected SXY into the frame fingerprint
  void rtp(uint32_t op);                            // gte RTP tap (fps60 gate) → fold the new SXY(s)
  void frame_commit(Core* core);                    // per-logic-frame fence + present orchestration

  // ---- transform capture / midpoint provider ---------------------------------------------------------
  // mSceneTag: armed around sceneNative() in Engine::drawOTag so every prim sceneNative queues is tagged
  //   fps_scene=1 (terrain/meshes/backdrop). OT-walk prims (2D/HUD/billboards) stay fps_scene=0. The
  //   mid-present rebuilds the fps_scene items fresh (interpolated) and re-emits the fps_scene=0 ones.
  // mInterp: true only while the mid-present re-runs sceneNative — the provider returns midpoint values.
  bool  mSceneTag = false;
  bool  mInterp   = false;
  bool  mSceneRan = false;      // did sceneNative run this frame (field)? gates the mid-present rebuild
  float mT        = 0.5f;       // in-between parameter (t=0.5 for one midpoint at 30→60fps)

  struct Cam    { float R[3][3]; float T[3]; float ofx, ofy, H; bool valid = false; };  // R in true (/4096) scale
  struct ObjX   { float R[3][3]; float T[3]; };                                          // R,T in raw int16 units
  struct Scroll { int x, y; bool valid = false; };                                       // backdrop tile scroll
  Cam mCamCur, mCamPrev;
  Scroll mScrollCur, mScrollPrev;
  std::unordered_map<uint32_t, ObjX> mObjCur, mObjPrev;   // per render-command (cmd ptr) object transform

  void beginCapture();          // clear this-frame captures (called at sceneNative top)
  // Scene camera read choke: fills R(int16 units)/T/ofx/ofy/H from the scratchpad view matrix; captures it,
  // and — if the mid-present is armed and a previous camera exists — overwrites with the t midpoint.
  void sceneCam(Core* c, float R[3][3], float T[3], float& ofx, float& ofy, float& H);
  // Object transform choke: R/T are the object's raw world rotation/position the caller just read from the
  // render command. Captured under `cmd`; on the mid-present, overwritten with the (prev,cur) t midpoint.
  void objXform(uint32_t cmd, float R[3][3], float T[3]);
  // Backdrop scroll choke: sx/sy are the tilemap scroll the backdrop drawer just read. Captured; on the
  // mid-present, overwritten with the (prev,cur) t midpoint so the parallax backdrop pans with the world.
  void bgScroll(int& sx, int& sy);

  // ---- billboard registry (3D-positioned 2D quads emitted by the guest into the OT) ------------------
  // recordBillboardSpan is called at the SAME instant the object publishes its packet-pool depth span; it
  // stores that span [lo,hi), the object identity, and the object's WORLD position (node+46/50/54). The OT
  // walk then matches each billboard prim's source OT-node to a recorded span and stamps the queued item
  // with the identity + world pos (stampBillboard). Per-Core so SBS's two cores keep separate registries.
  static constexpr int kBbMax = 1024;
  struct BbSpan { uint32_t lo, hi, ident; float wx, wy, wz; };
  BbSpan mBbCur[kBbMax] = {};
  int    mNBbCur = 0;

  // PER-PARTICLE billboard anchors (the true fps60 identity, 2026-07-10). A "billboard object" the guest
  // walks is a MANAGER node whose particle sub-list holds MANY visible sprites (all the gems/effect quads
  // of that class). Keying anchors by NODE makes every sprite share ONE anchor → they translate rigidly and
  // the per-sprite BOBBING (each particle's animated offset particle+14/+15) is lost — the "gems react
  // poorly at 60fps" bug. billboardEmit (perobj_billboard.cpp) instead records ONE entry PER PARTICLE it
  // emits: identity = the particle's guest ADDRESS (stable across frames while the gem lives), world anchor
  // = the manager node's world position (node+46/50/54) + the node-rotation (MAT_OUT, /4096) applied to the
  // particle's own 5×(p[14],p[15]) offset — so each sprite's anchor moves with its own animation. These are
  // searched BEFORE the node-level spans so a per-particle packet resolves to its particle, not its manager.
  static constexpr int kBbPartMax = 4096;
  BbSpan mBbPart[kBbPartMax] = {};
  int    mNBbPart = 0;

  void bbFrameReset() { mNBbCur = 0; mNBbPart = 0; }
  void recordBillboardSpan(Core* c, uint32_t lo, uint32_t hi, uint32_t ident);
  // Record one PER-PARTICLE anchor: span [pktLo,pktHi) is the particle's emitted OT packet, ident is the
  // particle's guest address, (wx,wy,wz) its interpolatable WORLD anchor. Called from billboardEmit.
  void recordBillboardParticle(uint32_t pktLo, uint32_t pktHi, uint32_t ident, float wx, float wy, float wz);
  int  billboardForNode(uint32_t node, uint32_t* identOut, float wpos[3]) const;
  void stampBillboard(Core* c, uint32_t node);

  // ---- present (interpolated in-between + real frame, paced 60fps 1-frame-behind) --------------------
  RqItem* mRqCur  = nullptr;    // this logic frame's resolved queue snapshot (captured at flush)
  RqItem* mRqPrev = nullptr;    // previous frame's snapshot (billboard prev-anchor source)
  int mNCur = 0, mNPrev = 0, mHavePrev = 0;
  void rq_capture(const RqItem* items, int n);      // copy the sorted queue snapshot
  void present_vk(Core* core);                      // build+present the in-between, then the real frame
  int  mDbg = -1;                                    // PSXPORT_DEBUG=fps60 lazy latch
  ~Fps60();
};

#endif // GAME_RENDER_FPS60_H
