// class ProjParams — per-Core camera view + projection constants used by the native depth path.
//
// Two clusters of state:
//   (1) camview — the MAIN scene camera view matrix, PUBLISHED once per frame by native_terrain at
//       terrain-draw time (when the scratchpad holds the real scene camera, before the per-object
//       compose overwrites it). Read by object-position → view-Z / screen projections for stable
//       billboard depth + the objid debug overlay's world→screen for 2D bounding boxes.
//   (2) proj_H / proj_cx / proj_cy — per-frame projection constants captured inside proj_native_xform
//       from the GTE control regs (CR24 OFX, CR25 OFY, CR26 H). Read by SSAO / lighting / depth
//       normalization (proj_pz_to_ord, proj_near_pz, proj_plane_h, proj_screen_center).
//
// PROPER OOP: one instance per Core, embedded on Render (`c->rsub.projParams`) — was a cluster of
// file-scope statics in gte_beetle.cpp (s_camR/T/H/OFX/OFY + s_proj_H/cx/cy). SBS's two cores need
// SEPARATE state so their published camera + per-frame projection constants don't clobber each other.
#pragma once
#include <cstdint>
class Core;

class ProjParams {
public:
  void bind(Core *c); // set the currently-bound ProjParams to this
  static ProjParams *current() {
    return sCurrent;
  }

  // -- camview (main scene camera view matrix) --------------------------------
  void publishCam(const float R[3][3], const float T[3], float H, float OFX, float OFY);
  bool camValid() const {
    return mCamValid;
  }
  float camWorldOrd(float wx, float wy, float wz) const;
  bool camWorldScreen(float wx, float wy, float wz, float *sx, float *sy) const;

  // -- projection constants (per-frame; captured inside proj_native_xform) -----
  void setProjH(uint16_t H) {
    mProjH = H;
  }
  void setProjCenter(float cx, float cy) {
    mProjCx = cx;
    mProjCy = cy;
  }
  uint16_t projH() const {
    return mProjH;
  }
  float projCx() const {
    return mProjCx;
  }
  float projCy() const {
    return mProjCy;
  }
  // Near-plane view-Z used by proj_pz_to_ord (= H/2, clamped >=1). SSAO needs it to invert the banded
  // depth back to a linear view-space Z (1/pz is affine in the stored depth).
  float projNearPz() const {
    float n = mProjH ? (float)mProjH * 0.5f : 1.0f;
    return n < 1.0f ? 1.0f : n;
  }
  float projPlaneH() const {
    return mProjH ? (float)mProjH : 1.0f;
  }
  void projScreenCenter(float *cx, float *cy) const {
    if (cx) {
      *cx = mProjCx;
    }
    if (cy) {
      *cy = mProjCy;
    }
  }

  // -- camera projection constants, FROM THE GAME'S OWN SETTER -----------------
  // OFX / OFY / H as the game STATED them, recorded where libgte SetGeomOffset / SetGeomScreen run.
  // This is the source the CAMERA path reads. It replaces `gte_read_ctrl(24)/(25)/(26)` in
  // Fps60::sceneCam — reading the projection back out of the GTE is asking engine state, after the
  // fact, for a constant the game already handed us, and it couples the native camera to whatever the
  // guest last left in the control registers.
  //
  // NOT the same thing as mProjCx/mProjCy/mProjH above, even though the numbers agree: those are
  // captured per-vertex inside proj_native_xform, which is a native reimplementation of the GTE's own
  // RTPS/RTPT instruction and therefore legitimately reads the GTE's control registers — that IS its
  // input. The camera is not a GTE instruction and has no business there.
  //
  // Two setters, not one, because the game has two: they are separate guest routines called at
  // separate times (offset at display init, screen distance again per area with the area's draw
  // range), and a camera assembled from only one of them has a fabricated half. geomValid() is false
  // until BOTH have run, and the defaults are 0 — deliberately NOT the stock 160/120/350, so "the
  // game never set a projection" can never be mistaken for "the game set the stock projection".
  void setGeomOffset(float ofx, float ofy) {
    mGeomOfx = ofx;
    mGeomOfy = ofy;
    mGeomOffsetSet = true;
  }
  void setGeomScreen(float h) {
    mGeomH = h;
    mGeomScreenSet = true;
  }

  // WIDESCREEN re-assert of the horizontal centre only. The window is created lazily (first present),
  // so the boot-time offset is baked at the 4:3 centre and the real one is only knowable once a width
  // exists; the title FrameDriver re-asserts it per frame. Vertical centre never moves, and this does NOT
  // mark the projection as set — an aspect adjustment is not the game stating a projection, and if the
  // game never stated one this must not make it look like it did.
  void setGeomOfxForAspect(float ofx) {
    mGeomOfx = ofx;
  }
  bool geomValid() const {
    return mGeomOffsetSet && mGeomScreenSet;
  }
  float geomOfx() const {
    return mGeomOfx;
  }
  float geomOfy() const {
    return mGeomOfy;
  }
  float geomH() const {
    return mGeomH;
  }

  // The ONLY way the render path should read the projection: hand back the game-set constants, or
  // ABORT naming the caller and the producer beneath it. `who` is the asking site. There is
  // deliberately no fallback and no "return false and let the caller cope" — a caller that could cope
  // would cope by inventing a projection, which is the thing being removed. One implementation so the
  // refusal cannot drift between the two readers (Fps60::sceneCam and the game's scene builder).
  void requireGeom(const char *who, float &ofx, float &ofy, float &H) const;

  // GTE Z scale factors (CR29 ZSF3 / CR30 ZSF4), captured at camview_publish time (frame build) so the
  // native submitters can recompute the game's own AVSZ3/AVSZ4 OT sort key without a present-time GTE
  // read (same rule as H — see submit.cpp's proj_set_H banner). 0 until first publish = "no key".
  void setZsf(int16_t zsf3, int16_t zsf4) {
    mZsf3 = zsf3;
    mZsf4 = zsf4;
  }
  int16_t zsf3() const {
    return mZsf3;
  }
  int16_t zsf4() const {
    return mZsf4;
  }

  // Depth-normalize: view-Z → [0,1] D32 ord using this instance's projection plane. Kept as a
  // non-static method so a caller with `Core* c` in scope can just do `c->rsub.projParams.pzToOrd(pz)`.
  float pzToOrd(float pz) const;

  // Snapshot / restore (fps60.cpp Tier-1: the present-time camera-lerp terrain re-render calls
  // camview_publish/proj_set_H with the LERPED camera, same as the real path does with the real one —
  // this state is per-Core shared render state, not sink-local, so Tier-1 saves it before and restores
  // it after so the re-render leaves no observable trace for anything reading ProjParams later (the
  // READ-ONLY OVERLAY invariant: writes stay confined to the isolated capture, host state that outlives
  // the call must come back exactly as it was).
  struct Snapshot {
    float R[3][3];
    float T[3];
    float H, OFX, OFY;
    bool valid;
    uint16_t projH;
    float projCx, projCy;
    int16_t zsf3, zsf4;
  };
  Snapshot snapshot() const {
    Snapshot s;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        s.R[i][j] = mCamR[i][j];
      }
      s.T[i] = mCamT[i];
    }
    s.H = mCamH;
    s.OFX = mCamOFX;
    s.OFY = mCamOFY;
    s.valid = mCamValid;
    s.projH = mProjH;
    s.projCx = mProjCx;
    s.projCy = mProjCy;
    s.zsf3 = mZsf3;
    s.zsf4 = mZsf4;
    return s;
  }
  void restore(const Snapshot &s) {
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        mCamR[i][j] = s.R[i][j];
      }
      mCamT[i] = s.T[i];
    }
    mCamH = s.H;
    mCamOFX = s.OFX;
    mCamOFY = s.OFY;
    mCamValid = s.valid;
    mProjH = s.projH;
    mProjCx = s.projCx;
    mProjCy = s.projCy;
    mZsf3 = s.zsf3;
    mZsf4 = s.zsf4;
  }

private:
  static ProjParams *sCurrent;

  // camview state
  float mCamR[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  float mCamT[3] = {0, 0, 0};
  float mCamH = 0.0f;
  float mCamOFX = 0.0f;
  float mCamOFY = 0.0f;
  bool mCamValid = false;

  // projection constants (per-frame)
  uint16_t mProjH = 0;
  float mProjCx = 160.0f;
  float mProjCy = 120.0f;

  // GTE Z scale factors (AVSZ3/AVSZ4), captured alongside the camera publish
  int16_t mZsf3 = 0;
  int16_t mZsf4 = 0;

  // camera projection constants as the GAME set them. 0 (not the stock 160/120/350) so an unset
  // projection is observably unset — see the setters above.
  float mGeomOfx = 0.0f;
  float mGeomOfy = 0.0f;
  float mGeomH = 0.0f;
  bool mGeomOffsetSet = false;
  bool mGeomScreenSet = false;
};

// ---- libgte SetGeomOffset / SetGeomScreen ---------------------------------------------------------
// The two SDK leaves through which a PSX game STATES its camera projection: where the screen centre is
// (OFX/OFY) and how far the projection plane sits from the eye (H). Their whole behaviour is
// `CR24 = ofx << 16; CR25 = ofy << 16` and `CR26 = h` — identical in every game that links libgte, so
// the implementation is FRAMEWORK and only the ADDRESSES are per-game (GameConfig::hle.setGeomOffset /
// .setGeomScreen, registered by PlatformHle exactly like the other libcd/libgpu/libmdec leaves).
//
// They live next to ProjParams because the GTE write and the port's own record of the projection must
// happen together: the native camera reads the recorded copy (requireGeom), and if a caller could move
// one without the other the two would drift — which is precisely the failure the recorded copy exists
// to remove.
//
// SAFE ON BOTH SBS LEGS, so PlatformHle (which fires on the oracle core too) is the right registrar
// rather than the oracle-gated override registry: these touch NO guest RAM, only GTE control registers
// and host-side ProjParams, both of which are per-Core.
void libgte_set_geom_offset(Core *c, int32_t ofx, int32_t ofy);
void libgte_set_geom_screen(Core *c, int32_t h);

// ---- Free-function thin bridges for callers with no `Core*` in scope -------------------------------
// These are the ONE-LINE forwards to `ProjParams::current()->method()`. Kept in the public header so
// the "declare inline anywhere I need it" pattern in game/render/*.cpp is gone — include this header
// once and every helper is visible.
float proj_pz_to_ord(float pz);
void proj_set_H(uint16_t h);
int proj_zsf3(void); // captured GTE CR29 (AVSZ3 scale) — 0 until the first camview_publish
int proj_zsf4(void); // captured GTE CR30 (AVSZ4 scale) — 0 until the first camview_publish
float proj_near_pz(void);
float proj_plane_h(void);
void proj_screen_center(float *cx, float *cy);
void camview_publish(const float R[3][3], const float T[3]);
int camview_valid(void);
float proj_camview_world_ord(float wx, float wy, float wz);
int proj_camview_world_screen(float wx, float wy, float wz, float *sx, float *sy);
