// class ProjParams — impl. See proj_params.h.
#include "proj_params.h"

ProjParams* ProjParams::sCurrent = nullptr;

void ProjParams::bind(Core* /*c*/) { sCurrent = this; }

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
  extern float proj_pz_to_ord(float);
  return proj_pz_to_ord(vz);
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
