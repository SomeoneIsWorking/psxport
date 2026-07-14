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
// PROPER OOP: one instance per Core, embedded on Render (`c->mRender->projParams`) — was a cluster of
// file-scope statics in gte_beetle.cpp (s_camR/T/H/OFX/OFY + s_proj_H/cx/cy). SBS's two cores need
// SEPARATE state so their published camera + per-frame projection constants don't clobber each other.
#pragma once
#include <cstdint>
class Core;

class ProjParams {
public:
  void bind(Core* c);                              // set the currently-bound ProjParams to this
  static ProjParams* current() { return sCurrent; }

  // -- camview (main scene camera view matrix) --------------------------------
  void  publishCam(const float R[3][3], const float T[3], float H, float OFX, float OFY);
  bool  camValid() const { return mCamValid; }
  float camWorldOrd(float wx, float wy, float wz) const;
  bool  camWorldScreen(float wx, float wy, float wz, float* sx, float* sy) const;

  // -- projection constants (per-frame; captured inside proj_native_xform) -----
  void     setProjH(uint16_t H)                { mProjH = H; }
  void     setProjCenter(float cx, float cy)   { mProjCx = cx; mProjCy = cy; }
  uint16_t projH()      const                  { return mProjH; }
  float    projCx()     const                  { return mProjCx; }
  float    projCy()     const                  { return mProjCy; }
  // Near-plane view-Z used by proj_pz_to_ord (= H/2, clamped >=1). SSAO needs it to invert the banded
  // depth back to a linear view-space Z (1/pz is affine in the stored depth).
  float    projNearPz() const                  { float n = mProjH ? (float)mProjH * 0.5f : 1.0f; return n < 1.0f ? 1.0f : n; }
  float    projPlaneH() const                  { return mProjH ? (float)mProjH : 1.0f; }
  void     projScreenCenter(float* cx, float* cy) const { if (cx) *cx = mProjCx; if (cy) *cy = mProjCy; }

  // Depth-normalize: view-Z → [0,1] D32 ord using this instance's projection plane. Kept as a
  // non-static method so a caller with `Core* c` in scope can just do `c->mRender->projParams.pzToOrd(pz)`.
  float pzToOrd(float pz) const;

  // Snapshot / restore (fps60.cpp Tier-1: the present-time camera-lerp terrain re-render calls
  // camview_publish/proj_set_H with the LERPED camera, same as the real path does with the real one —
  // this state is per-Core shared render state, not sink-local, so Tier-1 saves it before and restores
  // it after so the re-render leaves no observable trace for anything reading ProjParams later (the
  // READ-ONLY OVERLAY invariant: writes stay confined to the isolated capture, host state that outlives
  // the call must come back exactly as it was).
  struct Snapshot { float R[3][3]; float T[3]; float H, OFX, OFY; bool valid; uint16_t projH; float projCx, projCy; };
  Snapshot snapshot() const {
    Snapshot s;
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) s.R[i][j] = mCamR[i][j]; s.T[i] = mCamT[i]; }
    s.H = mCamH; s.OFX = mCamOFX; s.OFY = mCamOFY; s.valid = mCamValid;
    s.projH = mProjH; s.projCx = mProjCx; s.projCy = mProjCy;
    return s;
  }
  void restore(const Snapshot& s) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) mCamR[i][j] = s.R[i][j]; mCamT[i] = s.T[i]; }
    mCamH = s.H; mCamOFX = s.OFX; mCamOFY = s.OFY; mCamValid = s.valid;
    mProjH = s.projH; mProjCx = s.projCx; mProjCy = s.projCy;
  }

private:
  static ProjParams* sCurrent;

  // camview state
  float mCamR[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
  float mCamT[3]    = {0, 0, 0};
  float mCamH       = 0.0f;
  float mCamOFX     = 0.0f;
  float mCamOFY     = 0.0f;
  bool  mCamValid   = false;

  // projection constants (per-frame)
  uint16_t mProjH  = 0;
  float    mProjCx = 160.0f;
  float    mProjCy = 120.0f;
};

// ---- Free-function thin bridges for callers with no `Core*` in scope -------------------------------
// These are the ONE-LINE forwards to `ProjParams::current()->method()`. Kept in the public header so
// the "declare inline anywhere I need it" pattern in game/render/*.cpp is gone — include this header
// once and every helper is visible.
float proj_pz_to_ord(float pz);
void  proj_set_H(uint16_t h);
float proj_near_pz(void);
float proj_plane_h(void);
void  proj_screen_center(float* cx, float* cy);
void  camview_publish(const float R[3][3], const float T[3]);
int   camview_valid(void);
float proj_camview_world_ord(float wx, float wy, float wz);
int   proj_camview_world_screen(float wx, float wy, float wz, float* sx, float* sy);
