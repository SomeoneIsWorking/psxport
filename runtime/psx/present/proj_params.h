// class ProjParams — per-Core camera view + projection constants used by the native depth path.
//
// Two clusters of state:
//   (1) camview — the MAIN scene camera view matrix, PUBLISHED once per frame by native_terrain at
//       terrain-draw time (when the scratchpad holds the real scene camera, before the per-object
//       compose overwrites it). Read by object-position → view-Z / screen projections for stable
//       billboard depth + the objid debug overlay's world→screen for 2D bounding boxes.
//   (2) proj_H / proj_cx / proj_cy — per-frame projection constants captured by Gte::execute at each
//       RTPS/RTPT from the GTE control regs (CR24 OFX, CR25 OFY, CR26 H). Read by depth
//       normalization (pzToOrd, projNearPz, projPlaneH, projScreenCenter).
//
// PROPER OOP: one instance per Core, embedded on Render (`c->rsub.projParams`) — was a cluster of
// file-scope statics in gte_beetle.cpp (s_camR/T/H/OFX/OFY + s_proj_H/cx/cy). SBS's two cores need
// SEPARATE state so their published camera + per-frame projection constants don't clobber each other.
#pragma once
#include <cstdint>

namespace psx::cpu {
class Gte;
}
class Core;

class ProjParams {
public:
  // -- camview (main scene camera view matrix) --------------------------------
  // The scene camera R/T, with H, OFX/OFY and ZSF3/ZSF4 read from `gte`.
  void publishCam(const float R[3][3], const float T[3], psx::cpu::Gte &gte);
  bool camValid() const {
    return mCamValid;
  }
  float camWorldOrd(float wx, float wy, float wz) const;
  bool camWorldScreen(float wx, float wy, float wz, float *sx, float *sy) const;

  // -- projection constants (per-frame; captured by Gte::execute) -----
  // The near plane every depth normalisation is measured against (pzToOrd). It is per-frame state a
  // producer may install for the length of its own submission, so a frame in which producers disagree
  // puts their faces on two different depth scales in ONE shared D32 buffer — the shape of a
  // wrong-occlusion report. reportProjH prints every install as `previous -> next` on channel
  // `projplane`, so a frame's distinct planes are countable. It does NOT name the installer, so
  // "they all agree" and "one producer never installed a plane" still read the same; a verdict that
  // needs the owner has to add it here and at every setProjH call site.
  void setProjH(uint16_t H) {
    reportProjH(mProjH, H);
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
  // Near-plane view-Z used by pzToOrd (= H/2, clamped >=1).
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
  // This is the source the CAMERA path reads: reading the projection back out of the GTE would couple
  // the native camera to whatever the guest last left in the control registers.
  //
  // NOT the same thing as mProjCx/mProjCy/mProjH above, even though the numbers agree: those are
  // captured by Gte::execute after each RTPS/RTPT, whose input those control registers are.
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

  // GTE Z scale factors (CR29 ZSF3 / CR30 ZSF4), captured when a camera is published (frame build) so the
  // native submitters can recompute the game's own AVSZ3/AVSZ4 OT sort key without a present-time GTE
  // read (same rule as H). 0 until first publish = "no key".
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

private:
  // Out of line so the header carries no logging dependency; see proj_params.cpp for what it prints.
  static void reportProjH(uint16_t previous, uint16_t next);

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
// the implementation is FRAMEWORK and only the ADDRESSES are per-game (psx::hle::StockLibgte, bound by the
// title in registerOverrides like the other stock library leaves).
//
// They live next to ProjParams because the GTE write and the port's own record of the projection must
// happen together: title projection owners read the recorded copy, and if a caller could move
// one without the other the two would drift — which is precisely the failure the recorded copy exists
// to remove.
//
// SAFE ON BOTH SBS LEGS, so PlatformHle (which fires on the oracle core too) is the right registrar
// rather than the oracle-gated override registry: these touch NO guest RAM, only GTE control registers
// and host-side ProjParams, both of which are per-Core.
namespace psx::gte {

void setGeomOffset(Core &core, int32_t ofx, int32_t ofy);
void setGeomScreen(Core &core, int32_t h);

} // namespace psx::gte
