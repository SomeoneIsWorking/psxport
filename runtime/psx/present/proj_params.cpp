// class ProjParams — impl. See proj_params.h.
//
// Also owns the free-function bridges that live in proj_params.h — thin one-liners forwarding to
// `ProjParams::current()`. They exist so callers with no `Core*` in scope (inner projection loops,
// callback bodies) can still reach the currently-bound per-Core state.
#include "proj_params.h"
#include "core.h" // Core::rsub.projParams — the per-Core projection record
#include "gte.h"
#include <lucent/log.h>

// The GTE control registers that hold the camera projection.
static constexpr uint32_t kGteCrOfx = 24; // 16.16 screen-centre X
static constexpr uint32_t kGteCrOfy = 25; // 16.16 screen-centre Y
static constexpr uint32_t kGteCrH = 26;   // projection-plane distance
static constexpr uint32_t kGteCrZsf3 = 29;
static constexpr uint32_t kGteCrZsf4 = 30;

// Depth-normalize. Affine in 1/pz; nearer (smaller pz)
// -> larger value, matching the renderer's GREATER_OR_EQUAL compare + 0.0 clear.
// Every depth this port writes is normalised against the near plane below (H/2), so two producers
// submitting into the SAME D32 buffer with different planes put their faces on different scales and
// the nearer one can lose. Reporting the transitions — with both values — is what separates "one
// plane all frame" from "three producers, three scales", which a depth value alone cannot say.
void ProjParams::reportProjH(uint16_t previous, uint16_t next) {
  lucent::debug("projplane", "projH {} -> {}{}", previous, next, previous == next ? " (unchanged)" : " CHANGED");
}

float ProjParams::pzToOrd(float pz) const {
  float nearp = projNearPz();
  if (pz < nearp) {
    pz = nearp;
  }
  float inv_near = 1.0f / nearp, inv_far = 1.0f / 65535.0f;
  float ord = (1.0f / pz - inv_far) / (inv_near - inv_far);
  return ord < 0.0f ? 0.0f : (ord > 1.0f ? 1.0f : ord);
}

void ProjParams::publishCam(const float R[3][3], const float T[3], psx::cpu::Gte &gte) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      mCamR[i][j] = R[i][j];
    }
    mCamT[i] = T[i];
  }
  mCamH = static_cast<float>(static_cast<std::uint16_t>(gte.readControl(kGteCrH)));
  mCamOFX = static_cast<float>(static_cast<std::int32_t>(gte.readControl(kGteCrOfx))) / 65536.0f;
  mCamOFY = static_cast<float>(static_cast<std::int32_t>(gte.readControl(kGteCrOfy))) / 65536.0f;
  mCamValid = true;
  setZsf(static_cast<std::int16_t>(gte.readControl(kGteCrZsf3)),
         static_cast<std::int16_t>(gte.readControl(kGteCrZsf4)));
}

// Depth-only projection (view-Z from world position via the published camera). Called by 2D billboards
// that need to occlude by their real world position (consistent with the terrain they stand on).
float ProjParams::camWorldOrd(float wx, float wy, float wz) const {
  float vz = mCamR[2][0] * wx + mCamR[2][1] * wy + mCamR[2][2] * wz + mCamT[2];
  return pzToOrd(vz);
}

// Full world→screen projection via the published stable scene camera. Same projection as
// The native projection's float path: sx = OFX + vx*(H/pz), with pz = max(H/2, view-Z). Used by the objid
// debug overlay to box each game object at its real world position.
bool ProjParams::camWorldScreen(float wx, float wy, float wz, float *sx, float *sy) const {
  if (!mCamValid || mCamH <= 0.0f) {
    return false;
  }
  float vx = mCamR[0][0] * wx + mCamR[0][1] * wy + mCamR[0][2] * wz + mCamT[0];
  float vy = mCamR[1][0] * wx + mCamR[1][1] * wy + mCamR[1][2] * wz + mCamT[1];
  float vz = mCamR[2][0] * wx + mCamR[2][1] * wy + mCamR[2][2] * wz + mCamT[2];
  if (vz <= 0.0f) {
    return false; // behind the camera
  }
  float pz = mCamH * 0.5f;
  if (vz > pz) {
    pz = vz; // near-plane clamp
  }
  float ph = mCamH / pz;
  *sx = mCamOFX + vx * ph;
  *sy = mCamOFY + vy * ph;
  return true;
}

// ---- libgte SetGeomOffset / SetGeomScreen (declared in proj_params.h) ------------------------------
// The GTE control-register write and the port's record of the projection, in ONE place so they cannot
// drift. See the header for why this is framework code with a per-game address.
void psx::gte::setGeomOffset(Core &core, int32_t ofx, int32_t ofy) {
  core.gte().writeControl(kGteCrOfx, (uint32_t)ofx << 16); // OFX/OFY are 16.16 fixed point
  core.gte().writeControl(kGteCrOfy, (uint32_t)ofy << 16);
  core.rsub.projParams.setGeomOffset((float)ofx, (float)ofy);
}

void psx::gte::setGeomScreen(Core &core, int32_t h) {
  core.gte().writeControl(kGteCrH, (uint32_t)h); // H is a plain integer
  core.rsub.projParams.setGeomScreen((float)h);
}
