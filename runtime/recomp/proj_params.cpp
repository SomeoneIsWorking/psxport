// class ProjParams — impl. See proj_params.h.
//
// Also owns the free-function bridges that live in proj_params.h — thin one-liners forwarding to
// `ProjParams::current()`. They exist so callers with no `Core*` in scope (inner projection loops,
// callback bodies) can still reach the currently-bound per-Core state.
#include "proj_params.h"

// GTE control-reg reads for camview_publish (CR24=OFX, CR25=OFY, CR26=H). Declared here to avoid
// pulling the full Beetle GTE header into proj_params.h.
extern "C" uint32_t GTE_ReadCR(unsigned which);

ProjParams* ProjParams::sCurrent = nullptr;

void ProjParams::bind(Core* /*c*/) { sCurrent = this; }

// Depth-normalize (was file-scope proj_pz_to_ord in gte_beetle.cpp). Affine in 1/pz; nearer (smaller pz)
// -> larger value, matching the renderer's GREATER_OR_EQUAL compare + 0.0 clear.
float ProjParams::pzToOrd(float pz) const {
  float nearp = projNearPz();
  if (pz < nearp) pz = nearp;
  float inv_near = 1.0f / nearp, inv_far = 1.0f / 65535.0f;
  float ord = (1.0f / pz - inv_far) / (inv_near - inv_far);
  return ord < 0.0f ? 0.0f : (ord > 1.0f ? 1.0f : ord);
}

void ProjParams::publishCam(const float R[3][3], const float T[3], float H, float OFX, float OFY) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) mCamR[i][j] = R[i][j];
    mCamT[i] = T[i];
  }
  mCamH = H; mCamOFX = OFX; mCamOFY = OFY;
  mCamValid = true;
}

// Depth-only projection (view-Z from world position via the published camera). Called by 2D billboards
// that need to occlude by their real world position (consistent with the terrain they stand on).
float ProjParams::camWorldOrd(float wx, float wy, float wz) const {
  float vz = mCamR[2][0]*wx + mCamR[2][1]*wy + mCamR[2][2]*wz + mCamT[2];
  return pzToOrd(vz);
}

// Full world→screen projection via the published stable scene camera. Same projection as
// proj_native_xform's float path: sx = OFX + vx*(H/pz), with pz = max(H/2, view-Z). Used by the objid
// debug overlay to box each game object at its real world position.
bool ProjParams::camWorldScreen(float wx, float wy, float wz, float* sx, float* sy) const {
  if (!mCamValid || mCamH <= 0.0f) return false;
  float vx = mCamR[0][0]*wx + mCamR[0][1]*wy + mCamR[0][2]*wz + mCamT[0];
  float vy = mCamR[1][0]*wx + mCamR[1][1]*wy + mCamR[1][2]*wz + mCamT[1];
  float vz = mCamR[2][0]*wx + mCamR[2][1]*wy + mCamR[2][2]*wz + mCamT[2];
  if (vz <= 0.0f) return false;                   // behind the camera
  float pz = mCamH * 0.5f; if (vz > pz) pz = vz;  // near-plane clamp
  float ph = mCamH / pz;
  *sx = mCamOFX + vx * ph;
  *sy = mCamOFY + vy * ph;
  return true;
}

// ---- Free-function bridges (declared in proj_params.h) ---------------------------------------------
// Thin forwards to `ProjParams::current()` for callers with no `Core*` in scope. When the harness has
// bound this core (gte_bind), sCurrent points to `c->rsub.projParams` and every call reaches
// this-core state; before the first bind these are safe null-checks.

float proj_pz_to_ord(float pz) { auto* pp = ProjParams::current(); return pp ? pp->pzToOrd(pz) : 0.0f; }
int   proj_zsf3(void)          { auto* pp = ProjParams::current(); return pp ? pp->zsf3() : 0; }
int   proj_zsf4(void)          { auto* pp = ProjParams::current(); return pp ? pp->zsf4() : 0; }
void  proj_set_H(uint16_t h)   { if (auto* pp = ProjParams::current()) pp->setProjH(h); }
float proj_near_pz(void)       { auto* pp = ProjParams::current(); return pp ? pp->projNearPz() : 1.0f; }
float proj_plane_h(void)       { auto* pp = ProjParams::current(); return pp ? pp->projPlaneH() : 1.0f; }
void  proj_screen_center(float* cx, float* cy) {
  auto* pp = ProjParams::current();
  if (pp) pp->projScreenCenter(cx, cy);
  else    { if (cx) *cx = 160.0f; if (cy) *cy = 120.0f; }
}

// camview_publish reads live GTE CR24/25/26 (per-instance Beetle GTE state via GTE_ReadCR) so the
// projection constants match what proj_native_xform used this frame. Same shape as the old file-scope
// function in gte_beetle.cpp; state lands on the currently-bound ProjParams.
void camview_publish(const float R[3][3], const float T[3]) {
  auto* pp = ProjParams::current(); if (!pp) return;
  // CR24 already carries the widescreen center (Engine::initDisplay writes nw/2 when wide), so this
  // published camera and every native projection agree with the guest GTE — no per-read wide adjustment.
  float H   = (float)(uint16_t)GTE_ReadCR(26);
  float OFX = (float)(int32_t)GTE_ReadCR(24) / 65536.0f;
  float OFY = (float)(int32_t)GTE_ReadCR(25) / 65536.0f;
  pp->publishCam(R, T, H, OFX, OFY);
  // ZSF3/ZSF4 (CR29/CR30): the AVSZ scale factors the guest submitters feed into their OT sort key.
  // Captured here — the once-per-frame GTE-read choke — so the native submitters can recompute that
  // key without a present-time GTE read (kanban #11 game-sort-key discriminator).
  pp->setZsf((int16_t)(uint16_t)GTE_ReadCR(29), (int16_t)(uint16_t)GTE_ReadCR(30));
}
int   camview_valid(void)                                  { auto* pp = ProjParams::current(); return pp && pp->camValid() ? 1 : 0; }
float proj_camview_world_ord(float wx, float wy, float wz) { auto* pp = ProjParams::current(); return pp ? pp->camWorldOrd(wx, wy, wz) : 0.0f; }
int   proj_camview_world_screen(float wx, float wy, float wz, float* sx, float* sy) {
  auto* pp = ProjParams::current(); return pp && pp->camWorldScreen(wx, wy, wz, sx, sy) ? 1 : 0;
}
