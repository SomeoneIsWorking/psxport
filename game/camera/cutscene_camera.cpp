// class CutsceneCamera — the general FIELD / FOLLOW engine camera, PC-native (see cutscene_camera.h). It is
// the free-roam camera as well as the SOP/cutscene one (RESOLVED later-293 — docs/findings/camera.md).
// The arithmetic is the RE'd engine behaviour (docs/engine_re.md "CutsceneCamera"; later-173..176); this file
// restructures it into methods over named state (no guest register convention). It is verified per-call
// against the recomp oracle on the live SOP scene via `cam_snap_follow` (PSXPORT_DEBUG=camverify) and by the
// oracle UNIT TEST over every method incl. the driver (game/camera/cutscene_camera_selftest.cpp).
#include "cutscene_camera.h"
#include "game_ctx.h"
#include "cfg.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold (camverify)
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "mtx.h"
#include "trig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Retained libgte helpers (a math library — kept substrate, not PSX hardware).
// rsin (0x80083E80) and rcos (0x80083F50) are owned by class Trig (game/math/trig.h) — the class
// methods Trig::rsin / Trig::rcos have superseded the T2_RSIN / T2_RCOS `call(...)` shims here.
static constexpr uint32_t T2_ISQRT  = 0x80084080u;   // isqrt(x)      -> floor(sqrt)
// ratan2 (0x80085690) is owned by class Trig (game/math/trig.h) — Trig::ratan2 superseded T2_RATAN2/LA_RATAN2.
// angleCmp (0x80077768) is owned by class Trig — Trig::angleCmp superseded T2_ANGCMP.
// lookAt's matrix helpers (NB its isqrt is a DIFFERENT entry from rotBuild's).
static constexpr uint32_t LA_ISQRT  = 0x80077FB0u;
static constexpr uint32_t LA_MRINIT = 0x80051794u;   // MR_init (identity)
static constexpr uint32_t LA_MULMAT = 0x80084250u;   // MulMatrix0(a0,a1)
static constexpr uint32_t LA_COPYMAT= 0x800847B0u;   // CopyMatrix(a0,a1)
// shakeTail's helpers (a utility RNG + a sound/rumble-effect request queue — a library, kept substrate).
static constexpr uint32_t SHAKE_RAND = 0x8009A450u;    // PSX-style rand(): next = next*0x41c64e6d+12345
static constexpr uint32_t SHAKE_FX   = 0x800521F4u;    // queue a shake sound effect (a0,a1,id,pri)

// clamp(delta, -maxStep, +maxStep) on sign-extended s16 (engine FUN_8006CE74).
static inline int32_t cam_clamp(int16_t delta, int16_t maxStep) {
  if (delta >= 0) return delta < maxStep ? delta : maxStep;
  return delta < (int16_t)(-maxStep) ? (int16_t)(-maxStep) : delta;
}
// 32-bit mult-lo, matching the recomp's exact truncating arithmetic.
static inline int32_t mlo(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

int32_t CutsceneCamera::call(uint32_t fn, int32_t a0, int32_t a1, int32_t a2, int32_t a3) {
  c->r[4] = (uint32_t)a0; c->r[5] = (uint32_t)a1; c->r[6] = (uint32_t)a2; c->r[7] = (uint32_t)a3;
  rec_dispatch(c, fn);
  return (int32_t)c->r[2];
}

bool CutsceneCamera::followAxis(uint32_t accAddr, uint32_t tgt32Addr, uint16_t tgtInt, uint16_t curInt,
                        int16_t maxStep) {
  int16_t delta = (int16_t)(tgtInt - curInt);          // low-16 sign-extended
  if ((uint16_t)(delta + 10) < 21) {                   // |delta| <= 10 -> snap to the target
    w32(accAddr, r32(tgt32Addr));
    return true;
  }
  int32_t step = cam_clamp(delta, maxStep) << 13;
  w32(accAddr, r32(accAddr) + (uint32_t)step);
  return false;
}

// ── follow accumulators ──────────────────────────────────────────────────────────────────────────
bool CutsceneCamera::trackXZ(uint32_t target) {   // FUN_8006D960
  if (eng(c).mCamTpPending) {                        // one-shot debug teleport of Tomba's master pos
    auto& e = eng(c);
    e.mCamTpPending = false;
    w32(MASTER_X, (uint32_t)e.mCamTpX << 16);
    w32(MASTER_Y, (uint32_t)e.mCamTpY << 16);
    w32(MASTER_Z, (uint32_t)e.mCamTpZ << 16);
    w32(G + 0x44, 0);                                   // master speed
    fprintf(stderr, "[tp] Tomba -> (%d,%d,%d)\n", e.mCamTpX, e.mCamTpY, e.mCamTpZ);
  }
  bool snapX = followAxis(S + 0x0C, target + 0, r16(target + 2),  r16(S + 0x0E), 6144);
  bool snapZ = followAxis(S + 0x14, target + 8, r16(target + 10), r16(S + 0x16), 6144);
  return snapX && snapZ;
}
bool CutsceneCamera::trackY(uint32_t target) {   // FUN_8006DA54
  return followAxis(S + 0x10, target + 4, r16(target + 6), r16(S + 0x12), 5632);
}

// ── rotBuild (special-camera rotation / look-at builder) ─────────────────────────────────────────
void CutsceneCamera::yawDistAccumulate(int32_t dx, int32_t dz) {
  int32_t yaw  = (int16_t)trigOf(c).ratan2(-dz, dx);
  int32_t dist = (int16_t)call(T2_ISQRT, dx * dx + dz * dz);
  int32_t rc2  = trigOf(c).rcos(yaw);
  w32(S + 0x00, r32(S + 0x00) + ((rc2 * dist) >> 1));
  int32_t rs2  = trigOf(c).rsin(yaw);
  w32(S + 0x08, r32(S + 0x08) - ((rs2 * dist) >> 1));
  if (dist < 401) camW8(0x66, camR8(0x66) | 1);
}
void CutsceneCamera::lookatTail(int32_t theta, int32_t radius) {
  int32_t rc    = trigOf(c).rcos(theta);
  int32_t lookX = (int32_t)r16(0x1F800160u) + ((rc * radius) >> 12);
  int32_t rs    = trigOf(c).rsin(theta);
  int32_t lookZ = (int32_t)r16(0x1F800164u) - ((rs * radius) >> 12);
  int32_t camZ = (int32_t)r16(S + 0x0A);
  int32_t camX = (int32_t)r16(S + 0x02);
  int32_t dz   = (int16_t)(lookZ - camZ);
  int32_t dx   = (int16_t)(lookX - camX);
  yawDistAccumulate(dx, dz);
}

// ── scripted-camera look-angle builders (0x8006DC38/DAD8/DF88/DEF0 — used by snapFollowA/B) ─────────
void CutsceneCamera::posBuildA() {   // FUN_8006DC38 — overwrite the X/Z look accumulators
  int32_t P  = mlo(trigOf(c).rcos((int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
  int32_t x  = (int32_t)(int16_t)r16(S + 0x0e) + (mlo(trigOf(c).rcos((int16_t)camR16(0x70)), P) >> 12);
  int32_t cz = mlo(trigOf(c).rsin((int16_t)camR16(0x70)), P) >> 12;
  w32(S + 0x00, (uint32_t)(x << 16));
  int32_t z  = (int32_t)(int16_t)r16(S + 0x16) - cz;
  w32(S + 0x08, (uint32_t)(z << 16));
  camW8(0x66, camR8(0x66) | 1);
}
void CutsceneCamera::posBuildB() {   // FUN_8006DAD8 — place a look point then yaw/dist accumulate
  int32_t P  = (int16_t)(mlo(trigOf(c).rcos((int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12);
  int32_t x  = (int32_t)(uint16_t)r16(S + 0x0e) + (mlo(trigOf(c).rcos((int16_t)camR16(0x70)), P) >> 12);
  int32_t cz = mlo(trigOf(c).rsin((int16_t)camR16(0x70)), P) >> 12;
  int32_t dz = (int16_t)((int32_t)(uint16_t)r16(S + 0x16) - cz - (int32_t)(uint16_t)r16(S + 0x0a));
  int32_t dx = (int16_t)(x - (int32_t)(uint16_t)r16(S + 0x02));
  yawDistAccumulate(dx, dz);
}
void CutsceneCamera::headBuildA(uint32_t nonzero) {   // FUN_8006DF88
  if (nonzero == 0) {
    int32_t off = (int32_t)(int16_t)camR16(0x26) - 320;
    w16(S + 6, (uint16_t)((int32_t)r16(S + 0x12) + off));
  } else {
    int32_t step = mlo(trigOf(c).rsin((int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
    w16(S + 6, (uint16_t)((int32_t)r16(S + 0x12) - step));
  }
  camW8(0x66, camR8(0x66) | 2);
}
void CutsceneCamera::headBuildB() {   // FUN_8006DEF0
  int32_t step = mlo(trigOf(c).rsin((int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
  int32_t s6  = (int32_t)r16(S + 6);
  int32_t s12 = (int32_t)r16(S + 0x12);
  int32_t d   = (s6 + step) - s12;
  if ((uint16_t)(d + 10) < 21) {                 // |d| <= 10 -> snap
    w16(S + 6, (uint16_t)(s12 - step));
    camW8(0x66, camR8(0x66) | 2);
  } else {
    int32_t s = (int32_t)((uint32_t)d << 16) >> 3;
    w32(S + 4, (uint32_t)((int32_t)r32(S + 4) - s));
  }
}
void CutsceneCamera::joinE640(int32_t delta, int32_t radius) {
  int32_t sum = (int32_t)r16(G + 0x140) + (int32_t)camR16(0x52) + delta;
  uint16_t theta16 = (uint16_t)sum;
  camW16(0x8C, theta16);
  lookatTail((int16_t)theta16, radius);
}
static inline int32_t t1_e570(CutsceneCamera* self, Core* c, uint32_t G, uint32_t cam) {
  int32_t g140 = c->mem_r16s(G + 0x140), g56 = c->mem_r16s(G + 0x56);
  int32_t cam56 = (int32_t)c->mem_r16(cam + 0x56);
  return (g140 == g56) ? cam56 : -cam56;
}
int32_t CutsceneCamera::table1Delta() {
  uint8_t idx = r8(G + 0x164);
  switch (idx) {
    case 1: case 9: case 11: {
      if (r32(G + 0x158) == 0) return t1_e570(this, c, G, cam_);
      int32_t g140 = (int16_t)r16(G + 0x140), g56 = (int16_t)r16(G + 0x56);
      int32_t cam56 = (int32_t)camR16(0x56);
      return (g140 == g56) ? -cam56 : cam56;                    // inverted sense
    }
    case 3: {
      if (camR8(0x77) != 0)          return t1_e570(this, c, G, cam_);
      if (r8(0x800BF870u) == 6)      return t1_e570(this, c, G, cam_);
      int32_t g140 = (int16_t)r16(G + 0x140), g56 = (int16_t)r16(G + 0x56);
      int32_t cam56 = (int32_t)camR16(0x56);
      int32_t g168  = (int32_t)r8(G + 0x168) << 6;
      return (g140 != g56) ? (g168 - cam56) : (cam56 - g168);
    }
    default:
      return t1_e570(this, c, G, cam_);
  }
}
void CutsceneCamera::table2(int32_t radius) {
  uint32_t a1   = r8(G + 0x61);
  uint32_t idx2 = ((a1 & 0xff) >> 4) - 1;        // unsigned; >=8 -> default
  int32_t  theta;
  if (idx2 == 0) {
    if (a1 & 1) theta = (int32_t)r16(0x1F800196u) + 512;
    else {
      int32_t s196 = (int16_t)r16(0x1F800196u);
      int32_t g140 = (int16_t)r16(G + 0x140);
      int32_t d    = ((s196 - g140) & 0xfff) >> 1;
      theta = (int32_t)r16(0x1F800196u) + d;
    }
  } else if (idx2 == 1) {
    if (a1 & 1) {
      int32_t r = Trig::angleCmp((int16_t)r16(G + 0x56), (int16_t)r16(G + 0x140), 0);   // FUN_80077768(a,b,mode=0) -> native
      theta = (int32_t)r16(G + 0x140) + (r == 0 ? 512 : 1536);
    } else {
      int32_t a = (int16_t)r16(0x1F800194u);
      int32_t b = (int16_t)r16(0x1F800196u);
      theta = (int32_t)r16(G + 0x140) + (a != b ? 512 : 1536);
    }
  } else if (idx2 == 2) {
    int32_t vu = (int32_t)r16(0x1F800196u);
    theta = vu + ((int16_t)vu / 2);
  } else if (idx2 == 3) {
    theta = (int32_t)r16(0x1F800196u) + 512;
  } else if (idx2 == 7) {
    theta = (int32_t)r16(G + 0x140) + 1024;
  } else {
    theta = (int32_t)r16(G + 0x140) + 512;
  }
  lookatTail(theta & 0xfff, radius);
}
void CutsceneCamera::rotBuild() {   // FUN_8006E464
  if (camR8(0x76)) return;                              // disabled this frame
  uint32_t a1 = r8(G + 0x61);
  uint32_t a0 = a1 & 0xff;
  if (a0 != 0 && (camR8(0x72) & 0x80)) return;
  int32_t negEE  = -(int32_t)r16(0x1F8000EEu);
  int32_t radius = (int16_t)negEE;
  uint8_t c114   = camR8(0x72);
  if (c114 & 0x40) { lookatTail((int16_t)camR16(0x8C), radius); return; }   // SHAPE A
  if (a0 != 0 && (a1 & 1) == 0) { table2(radius); return; }
  if (r8(G + 0x17a)) { joinE640(0, (int16_t)(negEE - 600)); return; }       // radius override
  if (c114 & 2) {
    int32_t delta = (c114 & 1) ? -(int32_t)camR16(0x56) : (int32_t)camR16(0x56);
    joinE640(delta, radius);
    return;
  }
  joinE640(table1Delta(), radius);                     // TABLE 1
}

// ── distSolve (distance/zoom solver) ─────────────────────────────────────────────────────────────
void CutsceneCamera::distSolve() {   // FUN_8006D2AC
  // 1. settle timer cam[+0x22]
  uint8_t g61 = r8(G + 0x61);
  int16_t timer;
  if      (g61 & 0x80)             timer = 0;
  else if (r8(G + 0x17a))          timer = 0;
  else if ((g61 & 0xff) == 0)      timer = 240;
  else if (g61 & 1)                timer = 240;
  else if (r8(0x800BF816u))        timer = 0;
  else                             timer = 240;
  camW16(0x22, (uint16_t)timer);
  uint8_t flags = camR8(0x72);
  if (flags & 4) camW16(0x22, 0);
  flags = camR8(0x72);

  // 2. target planar point (tX,tZ in 16.16) + heading "mode" byte
  int32_t tX, tZ, mode;
  if (flags & 2) {
    tX = (int32_t)r32(G + 0x2c);
    tZ = (int32_t)r32(G + 0x34);
    mode = flags & 1;
  } else {
    uint8_t idx = r8(G + 0x164);
    uint8_t g147 = r8(G + 0x147);
    if (idx >= 13) idx = 8;
    switch (idx) {
      case 2: case 3: {
        uint32_t p = r32(G + 0x10);
        tX = (int32_t)(r32(p + 0x2c) << 16);
        tZ = (int32_t)(r32(p + 0x34) << 16);
        mode = g147; break;
      }
      case 12: {
        tX = (int32_t)(r16(0x1F800200u) << 16);
        tZ = (int32_t)(r16(0x1F800204u) << 16);
        mode = g147; break;
      }
      case 7: {
        tX = (int32_t)(r16(G + 0x14c) << 16);
        tZ = (int32_t)(r16(G + 0x150) << 16);
        mode = g147; break;
      }
      case 8: {
        tX = (int32_t)r32(G + 0x2c);
        tZ = (int32_t)r32(G + 0x34);
        mode = g147; break;
      }
      default: {
        tX = (int32_t)r32(G + 0x2c);
        tZ = (int32_t)r32(G + 0x34);
        mode = (r32(G + 0x158) == 0) ? (int32_t)g147 : (1 - (int32_t)g147);
        break;
      }
    }
  }

  // 3. base heading + the look-point offset using the settle timer
  int32_t baseAng = (int32_t)r16(G + 0x140);
  if (mode & 0xff) baseAng += 2048;
  int32_t s0a   = (int16_t)(uint16_t)baseAng;
  int32_t cam22 = (int16_t)camR16(0x22);
  int32_t s2 = tX + (mlo(trigOf(c).rcos(s0a), cam22) << 4);
  int32_t s1 = tZ - (mlo(trigOf(c).rsin(s0a), cam22) << 4);

  int32_t g140s = (int16_t)r16(G + 0x140);
  int32_t cam58 = (int32_t)camR32(0x58);
  int32_t coordX = tX + (mlo(trigOf(c).rcos(g140s), cam58) >> 4);
  int32_t coordZ = tZ - (mlo(trigOf(c).rsin(g140s), cam58) >> 4);
  int32_t s2q = (s2 - coordX) >> 8;
  int32_t s1q = (s1 - coordZ) >> 8;
  int32_t s0d = call(T2_ISQRT, mlo(s2q, s2q) + mlo(s1q, s1q)) << 8;
  int32_t ang = trigOf(c).ratan2(-s1q, s2q);
  int32_t angd = (ang - (int32_t)r16(G + 0x140) - 1024) & 0xfff;

  // 4. smooth cam[+0x14] toward the distance, accumulate into cam[+0x58], place camera X/Z
  int32_t cam14;
  int32_t cur = (int32_t)camR32(0x14);
  if (0x140000 < s0d) {
    if (angd < 2048) {
      int32_t neg = -s0d;
      if (cur < neg)        cam14 = ((int32_t)0xffd80000 < neg) ? neg : (int32_t)0xffd80000;
      else                  cam14 = (cur > 0 ? 0 : cur) - 65536;
    } else {
      if (s0d < cur)        cam14 = (0x0027ffff < s0d) ? 0x00280000 : s0d;
      else                  cam14 = (cur < 0 ? 0 : cur) + 65536;
    }
  } else {
    cam14 = (angd < 2048) ? -s0d : s0d;
  }
  camW32(0x14, (uint32_t)cam14);
  int32_t cam58n = cam58 + (cam14 >> 8);
  camW32(0x58, (uint32_t)cam58n);
  camW32(0x08, (uint32_t)(tX + (mlo(trigOf(c).rcos(g140s), cam58n) >> 4)));
  camW32(0x10, (uint32_t)(tZ - (mlo(trigOf(c).rsin(g140s), cam58n) >> 4)));
}

// ── angleStep ────────────────────────────────────────────────────────────────────────────────────
void CutsceneCamera::angleStep() {   // FUN_8006E010
  int32_t cur = (int32_t)camR32(0x34);
  if (r8(G + 0x164) != 3) {                             // not mode 3: drive cam[+0x34] toward 0 by ±8
    if (cur <= 0) { int32_t r = cur + 8; camW32(0x34, (uint32_t)r); if (r >= 0) camW32(0x34, 0); }
    else          { int32_t r = cur - 8; camW32(0x34, (uint32_t)r); if (r <= 0) camW32(0x34, 0); }
    return;
  }
  uint8_t idx = r8(G + 0x168);
  int32_t target;
  switch (idx >= 5 ? 4 : idx) {
    case 0: case 1: target = 0;    break;
    case 2:         target = -128; break;
    case 3:         target = -256; break;
    default:        target = -384; break;
  }
  if (target < cur) { int32_t r = cur - 8; camW32(0x34, (uint32_t)r); if (r < target) camW32(0x34, (uint32_t)target); }
  else              { int32_t r = cur + 8; camW32(0x34, (uint32_t)r); if (target < r) camW32(0x34, (uint32_t)target); }
}

// ── yFloor (camera-Y floor clamp, per render mode) ───────────────────────────────────────────────
void CutsceneCamera::yFloor() {   // FUN_8006C80C
  uint8_t mode1 = r8(0x800BF870u);
  uint32_t idx = (uint32_t)(uint8_t)(mode1 - 1);
  if (idx >= 13) return;
  const uint32_t YA = 0x1F8000E2u;
  int32_t Y = (int16_t)r16(YA);
  switch (idx) {
    case 0:
      if (Y < -10140) w16(YA, (uint16_t)(int16_t)-10140);
      break;
    case 3: {
      if (r8(0x800BF871u) == 7) {
        if ((int16_t)r16(0x800E7EB6u) < 6800) {
          if (Y < -7299) { if (!(Y < -6499)) w16(YA, (uint16_t)(int16_t)-6500); }
          else           { w16(YA, (uint16_t)(int16_t)-7300); }
        }
      } else {
        if (!(Y < -6599)) w16(YA, (uint16_t)(int16_t)-6600);
      }
      break;
    }
    case 5:
      if (r8(0x1F800207u) == 14) { if (Y < -7200) w16(YA, (uint16_t)(int16_t)-7200); }
      else                       { if (Y < -9200) w16(YA, (uint16_t)(int16_t)-9200); }
      break;
    case 9:
      if (Y < -2160) w16(YA, (uint16_t)(int16_t)-2160);
      break;
    case 12:
      if (!(Y < -1399)) w16(YA, (uint16_t)(int16_t)-1400);
      break;
    default:
      break;
  }
}

// ── pitch (vertical-look height smoother) ────────────────────────────────────────────────────────
void CutsceneCamera::pitch() {   // FUN_8006D654
  int32_t g30  = (int32_t)r32(G + 0x30);
  int16_t g17e = (int16_t)r16(G + 0x17e);
  int sign = (g17e & 0x8000) != 0;

  int32_t r5 = g30; int viaC8 = 0;
  uint8_t idx = r8(G + 0x164);
  if (idx >= 13) idx = 7;
  switch (idx) {
    case 2: { uint32_t p = r32(G + 0x10); r5 = (int32_t)(r32(p + 0x30) << 16); viaC8 = 1; break; }
    case 3: { uint32_t p = r32(G + 0x10); r5 = (int32_t)(r32(p + 0x30) << 16); break; }
    case 12: {
      int32_t a = (int16_t)r16(0x1F800202u);
      int32_t sum = (a << 16) + g30;
      r5 = (int32_t)((sum + (int32_t)((uint32_t)sum >> 31)) >> 1); viaC8 = 1; break;
    }
    case 7: case 8: r5 = g30; viaC8 = 1; break;
    default: {
      uint8_t m  = r8(0x800BF821u);
      uint8_t gm = r8(G + 0x145);
      if (m == 1) {
        r5 = g30 - (200 << 16);
      } else if (m != 0) {
        if (gm == 2) r5 = g30 + (sign ? -(240 << 16) : (40 << 16));
        else         r5 = g30 + (sign ? -(310 << 16) : -(240 << 16));
      } else {
        if (gm == 2) r5 = sign ? g30 : (g30 + (200 << 16));
        else         r5 = sign ? (g30 - (70 << 16)) : g30;
      }
      break;
    }
  }
  if (viaC8) r5 += (200 << 16);
  int32_t target = r5 - (600 << 16);

  int32_t delta = target - (int32_t)camR32(0x0c);
  int32_t r7 = (g17e < 0) ? (20 << 16)  : (140 << 16);
  int32_t r4 = (g17e < 0) ? (580 << 16) : (460 << 16);

  int dir; int32_t r3;
  int32_t t = delta + r4;
  if (t >= 0) { dir = 0; r3 = t; }
  else {
    int32_t t2 = t + r7;
    if (t2 < 0) { dir = 1; r3 = t2; }
    else { camW32(0x18, 0); return; }
  }

  if (dir == 0) {
    if (!(0x80000 < r3)) { camW32(0x0c, (uint32_t)(r5 + r4 - (600 << 16))); return; }
    int32_t cv = (int32_t)camR32(0x18);
    if (!(r3 < cv)) {
      if (cv < 0) camW32(0x18, 0);
      camW32(0x18, (uint32_t)((int32_t)camR32(0x18) + (16 << 16)));
    } else if (0x4FFFFF < r3) {
      camW32(0x18, (uint32_t)(80 << 16));
    } else {
      camW32(0x18, (uint32_t)r3);
    }
  } else {
    uint8_t g145 = r8(G + 0x145);
    if (g145 != 0) {
      if (!(r3 < -(256 << 16))) return;
      r3 += (256 << 16);
      if (-(96 << 16) < r3) camW32(0x18, (uint32_t)r3);
      else                  camW32(0x18, (uint32_t)-(96 << 16));
    } else {
      if (!(r3 < -(22 << 16))) {
        camW32(0x18, (uint32_t)r3);
      } else {
        int32_t cv = (int32_t)camR32(0x18);
        if (!(cv < r3)) {
          if (cv > 0) camW32(0x18, 0);
          camW32(0x18, (uint32_t)((int32_t)camR32(0x18) + (int32_t)0xFFFEA000u));
        } else {
          camW32(0x18, (uint32_t)-(22 << 16));
        }
      }
    }
  }
  camW32(0x0c, (uint32_t)((int32_t)camR32(0x0c) + (int32_t)camR32(0x18)));
}

// ── heading (heading tracker) ────────────────────────────────────────────────────────────────────
void CutsceneCamera::heading() {   // FUN_8006DCF4
  if (camR8(0x74) & 4) { camW8(0x66, camR8(0x66) | 2); return; }

  int32_t off = 0;
  uint8_t g61 = r8(G + 0x61);
  int useTable;
  if (g61 == 0)            useTable = 1;
  else if ((g61 & 1) == 0) { off = 768; useTable = 0; }
  else                     useTable = 1;

  if (useTable) {
    uint8_t idx = r8(G + 0x164);
    if (idx >= 13) return;
    switch (idx) {
      case 0: case 4: {
        if (r8(G + 0x145) == 0) { off = 320; break; }
        int32_t g4a_s = (int16_t)r16(G + 0x4a);
        uint32_t r3v  = (uint16_t)r16(G + 0x4a);
        if (g4a_s < 0) r3v = (uint32_t)(0 - r3v);
        r3v += (r8(G + 0x165) == 0) ? (uint32_t)-14976 : (uint32_t)-16384;
        int32_t s = (int32_t)((uint32_t)r3v << 16) >> 21;
        off = 320 - s;
        break;
      }
      case 1: case 11: {
        uint32_t sub = r32(G + 0x158);
        off = (r8(sub + 0xc) == 4 && r8(sub + 0x2) != 0) ? 1100 : 700;
        break;
      }
      case 9:  off = -320; break;
      case 3:  off = 700;  break;
      case 12: {
        uint32_t diff = (uint32_t)(uint16_t)r16(G + 0x32) - (uint32_t)(uint16_t)r16(0x1F800202u);
        off = ((int16_t)diff < 501) ? (int32_t)diff : 500;
        break;
      }
      default: return;
    }
  }

  int32_t s06 = (uint16_t)r16(S + 6);
  int32_t c26 = (uint16_t)camR16(0x26);
  int32_t s12 = (uint16_t)r16(S + 0x12);
  int32_t r5  = s06 + off - c26 - s12;
  if ((uint16_t)(r5 + 10) < 21) {
    w16(S + 6, (uint16_t)(c26 + (s12 - off)));
    camW8(0x66, camR8(0x66) | 2);
  } else {
    int32_t step = (int32_t)((uint32_t)r5 << 16) >> 3;
    w32(S + 4, (uint32_t)((int32_t)r32(S + 4) - step));
  }

  uint8_t f = camR8(0x74);
  int cond = 0, active = 1;
  if (f & 2)      cond = ((int16_t)r16(S + 6) < (int16_t)camR16(0x4a));
  else if (f & 8) cond = ((int16_t)camR16(0x4a) < (int16_t)r16(S + 6));
  else            active = 0;
  if (active && !cond) {
    w16(S + 6, (uint16_t)camR16(0x4a));
    camW8(0x66, camR8(0x66) | 2);
  }
}

// ── lookAt (camera basis / view-matrix builder) ──────────────────────────────────────────────────
static inline int32_t cam_idiv(Core* c, int32_t num, int32_t den) {
  cpu_div(c, (uint32_t)num, (uint32_t)den);
  return (int32_t)c->lo;
}
void CutsceneCamera::lookAt() {   // FUN_8006D02C
  int32_t dX = (int16_t)r16(S + 14) - (int16_t)r16(S + 2);
  int32_t dZ = (int16_t)r16(S + 22) - (int16_t)r16(S + 10);
  int32_t dY = (int16_t)r16(S + 18) - (int16_t)r16(S + 6);
  int32_t xz = dX * dX + dZ * dZ;
  int32_t s18 = call(LA_ISQRT, xz + dY * dY) & 0xffff;
  int32_t s19 = call(LA_ISQRT, xz) & 0xffff;
  camW32(0x5c, (uint32_t)s18);
  camW32(0x60, (uint32_t)s19);

  if (s18 != 0) {
    mtxOf(c).identity(S + 40);
    int32_t sinp = cam_idiv(c, dY  << 12, s18);
    int32_t cosp = cam_idiv(c, s19 << 12, s18);
    int32_t pitch = (int16_t)trigOf(c).ratan2(sinp, cosp);
    w16(S + 32, (uint16_t)pitch);
    w16(S + 48, (uint16_t)cosp);
    w16(S + 56, (uint16_t)cosp);
    w16(S + 50, (uint16_t)(-sinp));
    w16(S + 54, (uint16_t)sinp);

    if (s19 != 0) {
      int32_t a = cam_idiv(c, (-dX) << 12, s19);
      int32_t b = cam_idiv(c, dZ    << 12, s19);
      int32_t yaw = (int16_t)trigOf(c).ratan2(a, b);
      w16(S + 34, (uint16_t)yaw);
      mtxOf(c).identity(0x1F800000u);
      w16(0x1F800000u, (uint16_t)b);
      w16(0x1F800004u, (uint16_t)a);
      w16(0x1F800010u, (uint16_t)b);
      w16(0x1F80000Cu, (uint16_t)(-a));
      call(LA_MULMAT, (int32_t)(S + 40), 0x1F800000);
    }
  }

  uint32_t M = S + 40;
  uint16_t r104 = r16(S + 52);
  uint16_t r106 = r16(S + 54);
  uint16_t r108 = r16(S + 56);
  w16(S + 24, r104);
  w16(S + 26, r106);
  w16(S + 28, r108);
  w16(0x1F8000C0u, (uint16_t)(0u - (uint32_t)r16(S + 2)));
  w16(0x1F8000C2u, (uint16_t)(0u - (uint32_t)r16(S + 6)));
  w16(0x1F8000C4u, (uint16_t)(0u - (uint32_t)r16(S + 10)));
  mathOf(c).applyMatrixLV((uint32_t)M, 0x1F8000C0u, 0x1F80010Cu);   // FUN_80084470 (native)
  call(LA_COPYMAT, (int32_t)M, (int32_t)(S + 72));
}

// ── orchestrators (per-frame camera modes) ───────────────────────────────────────────────────────
void CutsceneCamera::snapAccXZ(uint32_t target) {    // FUN_8006D934
  w32(S + 0x0C, r32(target + 0));      // snap X accumulator
  w32(S + 0x14, r32(target + 8));      // snap Z accumulator
}
void CutsceneCamera::snapAccY(uint32_t target) {     // FUN_8006D950
  w32(S + 0x10, r32(target + 4));      // snap Y accumulator
}
void CutsceneCamera::snapFollow(uint32_t target) {   // FUN_8006E3B0
  int s_v = c->game->verify.on("camverify");
  uint32_t cam0[64], sp0[256], rsave[32];
  if (s_v) {
    for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam_ + i * 4);
    for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1F800000u + i * 4);
    memcpy(rsave, c->r, sizeof rsave);
  }

  snapAccXZ(target);                   // snap the follow accumulators to target (no smoothing)
  snapAccY(target);
  lookAt();

  if (!s_v) return;
  // `camverify` A/B: capture the native result, roll back, super-call the recomp oracle at
  // 0x8006E3B0, diff the resulting cam+scratchpad state.
  uint32_t camM[64], spM[256];
  for (int i = 0; i < 64;  i++) camM[i] = c->mem_r32(cam_ + i * 4);
  for (int i = 0; i < 256; i++) spM[i]  = c->mem_r32(0x1F800000u + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam_ + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1F800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  c->r[4] = cam_; c->r[5] = target;
  rec_interp(c, 0x8006E3B0u);
  uint32_t camO[64], spO[256];
  for (int i = 0; i < 64;  i++) camO[i] = c->mem_r32(cam_ + i * 4);
  for (int i = 0; i < 256; i++) spO[i]  = c->mem_r32(0x1F800000u + i * 4);
  VerifyHarness::Check& chk = c->game->verify.check("camverify");
  long &nbad = chk.nMismatch, &ngood = chk.nMatch;
  int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[camverify] snapFollow cam+0x%02x mine=%08x oracle=%08x\n", i*4, camM[i], camO[i]); }
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[camverify] snapFollow sp+0x%03x mine=%08x oracle=%08x\n", i*4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[camverify] snapFollow match #%d\n", (int)ngood);
}
void CutsceneCamera::snapFollowA(uint32_t target) {  // FUN_8006E294 (driver mode 2 + init post-check)
  snapAccXZ(target);
  snapAccY(target);
  if (camR8(0x76) == 0) { posBuildA(); headBuildA(1); }   // scripted look-build A
  lookAt();
}
void CutsceneCamera::pitchFollow(uint32_t target) {  // FUN_8006E360 (driver mode 3)
  pitch();
  snapAccXZ(target);
  snapAccY(target);
  lookAt();
}
void CutsceneCamera::snapFollowB(uint32_t target) {  // FUN_8006E2FC (driver mode 4)
  snapAccXZ(target);
  snapAccY(target);
  if (camR8(0x76) == 0) { posBuildB(); headBuildB(); }              // scripted look-build B
  lookAt();
}
void CutsceneCamera::mainFollow() {   // FUN_8006E0F0
  distSolve();
  trackXZ(cam_ + 8);
  pitch();
  trackY(cam_ + 8);
  yFloor();
  if (camR8(0x76) == 0 && r8(G + 0x17a) == 0) heading();
  lookAt();
  if (camR8(0x77) == 0) angleStep();
  int32_t acc = (int32_t)camR32(0x28) + (int32_t)camR32(0x34);
  w32(S + 0x44, r32(S + 0x44) - (uint32_t)acc);
}
void CutsceneCamera::simpleFollow(uint32_t target) {   // FUN_8006E3F4
  if (trackXZ(target)) camW8(0x66, camR8(0x66) | 1);
  if (trackY(target))  camW8(0x66, camR8(0x66) | 2);
  lookAt();
}
void CutsceneCamera::trackFollow(uint32_t target) {   // FUN_8006E228
  trackXZ(target);
  trackY(target);
  camW16(0x0e, r16(0x1F8000E2u));
  if (camR8(0x76) == 0) { posBuildB(); headBuildB(); }   // scripted look-build B (see snapFollowB — same pattern)
  lookAt();
}

// ── post-mode TAIL (0x8006C988) — the camera SHAKE state machine ───────────────────────────────────
// cam[0x76] is the shake state, driven by external code (0 = idle, no-op). Two families:
//   * 3-axis free-running shake: 1 (capture anchor, ->2) -> 2 (jitter X/Y/Z around the anchor every frame,
//     fx id 129, stays at 2) -> 3 (external code sets this to stop: restore the exact anchor, ->0).
//   * Y-only shake, three variants sharing the same shape (capture-then-jitter):
//       4->5: free-running (like 1->2, but Y-only, fx id 241, stays at 5 until externally reset).
//       6->7, 8->9: ONE-SHOT pulses (states 6/8 fall straight into 7/9's jitter in the SAME frame — that's
//       the recompiled control flow, not a bug); 7/9 abort (->0, no jitter) if cam[0x64] is busy, else
//       jitter once (±32 for 7, ±16 for 9) and always end at state 0.
void CutsceneCamera::shakeTail() {   // FUN_8006C988
  uint8_t state = camR8(0x76);
  if (state >= 10) return;
  switch (state) {
    case 1:
      camW16(0x86, r16(S + 0x02));
      camW16(0x88, r16(S + 0x06));
      camW8(0x76, 2);
      camW16(0x8a, r16(S + 0x0a));
      break;
    case 2: {
      int32_t rx = rngOf(c).next() & 0x1f;
      w16(S + 0x02, (uint16_t)((int32_t)camR16(0x86) - 16 + rx));
      int32_t rz = rngOf(c).next() & 0x1f;
      w16(S + 0x0a, (uint16_t)((int32_t)camR16(0x8a) - 16 + rz));
      int32_t ry = rngOf(c).next() & 0xf;
      w16(S + 0x06, (uint16_t)((int32_t)camR16(0x88) - 8 + ry));
      call(SHAKE_FX, 0, 0, 129, 2);
      break;
    }
    case 3:
      w16(S + 0x02, camR16(0x86));
      w16(S + 0x06, camR16(0x88));
      w16(S + 0x0a, camR16(0x8a));
      camW8(0x76, 0);
      break;
    case 4:
      camW16(0x88, r16(S + 0x06));
      camW8(0x76, 5);
      break;
    case 5: {
      int32_t r = rngOf(c).next() & 0x3f;
      w16(S + 0x06, (uint16_t)((int32_t)camR16(0x88) - 32 + r));
      call(SHAKE_FX, 0, 0, 241, 2);
      break;
    }
    case 6:
      camW16(0x88, r16(S + 0x06));
      camW8(0x76, 7);
      [[fallthrough]];
    case 7:
      if (camR8(0x64) != 0) { camW8(0x76, 0); break; }
      {
        int32_t r = rngOf(c).next() & 0x3f;
        w16(S + 0x06, (uint16_t)((int32_t)camR16(0x88) - 32 + r));
        call(SHAKE_FX, 0, 0, 129, 2);
      }
      camW8(0x76, 0);
      break;
    case 8:
      camW16(0x88, r16(S + 0x06));
      camW8(0x76, 9);
      [[fallthrough]];
    case 9:
      if (camR8(0x64) != 0) { camW8(0x76, 0); break; }
      {
        int32_t r = rngOf(c).next() & 0x1f;
        w16(S + 0x06, (uint16_t)((int32_t)camR16(0x88) - 16 + r));
        call(SHAKE_FX, 0, 0, 129, 2);
      }
      camW8(0x76, 0);
      break;
    default: break;
  }
}

// ── driver + init (the camera dispatcher) ─────────────────────────────────────────────────────────
// Still-unowned resident camera LEAVES reached by the mode dispatch / init (a1-taking follow variants and
// the init sub-fns). Kept substrate until they're rebuilt as methods — contiguous top-down ownership owns
// the CALLER (update/init) first, its unowned children run via the substrate (0-diff, same as trackFollow).
// FIELD OVERLAY handlers reached by some modes / render sub-modes: they live in loaded \BIN\*.BIN overlays,
// not resident MAIN, so they dispatch through the overlay router. (In the oracle unit test no overlay is
// loaded, so these modes MISS and are skipped — same class as the yFloor recompiler gap.)
static constexpr uint32_t OV_RENDER_RM2_M0 = 0x80115F58u, OV_RENDER_RM7_M0 = 0x80112DECu, OV_RENDER_RM20_M0 = 0x8010AD0Cu;
static constexpr uint32_t OV_RENDER_RM2_M1 = 0x80116918u, OV_RENDER_RM7_M1 = 0x80113660u, OV_RENDER_RM20_M1 = 0x8010B2F0u;
static constexpr uint32_t OV_MODE9  = 0x8018B924u;   // mode 9  field-overlay camera handler
static constexpr uint32_t OV_A00_CAM= 0x8010D89Cu;   // mode 10 A00 scripted-camera state machine
static constexpr uint32_t OV_MODE17 = 0x80111AB4u;   // mode 17 field-overlay handler
static constexpr uint32_t RENDER_FP_TABLE = 0x800A4AA0u;   // mode 0 render-mode → fn-pointer table (resident data)

void CutsceneCamera::dispatchMode(uint8_t mode) {
  switch (mode) {
    case 0: {   // main follow, gated, then a render-mode-keyed handler
      uint8_t rm = r8(0x800BF870u);
      if      (rm == 7)  { sub(OV_RENDER_RM7_M0);  break; }
      else if (rm == 2)  { sub(OV_RENDER_RM2_M0);  break; }
      else if (rm == 20) { sub(OV_RENDER_RM20_M0); break; }
      if (!(camR8(0x64) & 0x80)) { mainFollow(); rotBuild(); }
      sub(r32(RENDER_FP_TABLE + (uint32_t)rm * 4));   // indirect render fn (field overlay)
      break;
    }
    case 1: {   // track follow, with the same render-mode-keyed overlay prologue
      uint8_t rm = r8(0x800BF870u);
      if      (rm == 7)  { sub(OV_RENDER_RM7_M1);  break; }
      else if (rm == 2)  { sub(OV_RENDER_RM2_M1);  break; }
      else if (rm == 20) { sub(OV_RENDER_RM20_M1); break; }
      trackFollow(cam_ + 0x38);
      break;
    }
    case 2:  snapFollowA(cam_ + 0x38); break;
    case 3:  pitchFollow(cam_ + 0x38); break;
    case 4:  snapFollowB(cam_ + 0x38); break;
    case 5:  snapFollow(G + 0x2c);       break;   // snap to MASTER position
    case 6:  camW8(0x64, 0); camW32(0x0c, r32(G + 0x30)); break;   // freeze: cam[0x0c] = master Y
    case 7:
    case 14: snapFollow(cam_ + 0x38);    break;
    case 8:  simpleFollow(cam_ + 0x38);  break;
    case 9:  sub(OV_MODE9);              break;
    case 10: sub(OV_A00_CAM);            break;
    case 11:
    case 12: camW8(0x64, 0); camW8(2, 0); camW8(3, 0); break;   // reset
    case 13: camW8(0x64, 6);            break;
    case 15: simpleFollow(G + 0x2c);     break;
    case 16: /* tail only */            break;
    case 17: sub(OV_MODE17);            break;
    default: break;
  }
}

void CutsceneCamera::initPlace() {   // FUN_8006E918 (init: place the camera X/Z base from the heading)
  int32_t g140 = (int16_t)r16(G + 0x140);
  uint16_t cam56 = camR16(0x56);
  // s0: cam[0x56], negated unless the scene heading G+0x140 already equals the target heading G+0x56.
  int32_t s0 = (g140 == (int16_t)r16(G + 0x56)) ? (int16_t)cam56
                                                : (int16_t)(uint16_t)(0u - (uint32_t)cam56);
  int32_t s1 = (int16_t)(uint16_t)(0u - (uint32_t)r16(0x1F8000EEu));   // -(radius) as int16
  int32_t angle = g140 + (int16_t)r16(cam_ + 0x52) + s0;
  int32_t cx = (int32_t)(uint16_t)r16(G + 0x2e) + (mlo(trigOf(c).rcos(angle), s1) >> 12);
  w16(S + 0x02, (uint16_t)cx);
  int32_t cz = (int32_t)(uint16_t)r16(G + 0x36) - (mlo(trigOf(c).rsin(angle), s1) >> 12);
  w16(S + 0x0a, (uint16_t)cz);
}
void CutsceneCamera::initSeedGrp(uint32_t src) {   // FUN_8006CBA8 (writes the FIXED driver cam @0x800E8008)
  w16(CAM_OBJ + 0x3a, r16(src + 2));
  w16(CAM_OBJ + 0x3e, r16(src + 6));
  w16(CAM_OBJ + 0x42, r16(src + 10));
}

// ── Wiring pass (2026-07-08 frontier follow-up) ─────────────────────────────────────────────────
// Ghidra decomp: scratch/decomp_local/region_8006.c (import "main_ram", va 0x80060000-0x80070000) +
// gen_func_8006E8F8 (generated/shard_5.c, Ghidra missed this one — read the recompiled body directly,
// which is instruction-exact ground truth per CLAUDE.md).
//
// REAL BUG found wiring this one (was drafted assuming camera-only semantics): the gen body reads
// its object base from a0 (c->r[4]), NOT a hardcoded CAM_OBJ constant like pushMode/restoreMode/
// snapToMasterOffsetY200/orbitTick below. Confirmed by its cross-module callers (generated/
// ov_a00_shard_0.c..ov_a0k_shard_0.c etc.) — every one of them calls FUN_8006E8F8 with a0 = that
// overlay's OWN actor-object pointer (e.g. `c->r[16]`, an A00-area actor base), not the camera
// object at 0x800E8008. So this leaf is a generic "reset follow accumulator" applied to whatever
// object shares this field shape (0x24/0x28/0x56) — the camera is just ONE caller (of many). The
// method itself is unaffected (it already takes its base from the instance's `cam_`, which reads
// as "cam" only by naming convention); the fix lives entirely in the override-registry wiring
// below, which constructs the instance from the LIVE a0 rather than hardcoding CAM_OBJ.
void CutsceneCamera::resetFollowAccum() {   // FUN_8006E8F8
  camW32(0x24, 0);
  camW32(0x28, 0);
  w16(S + 0x1e, (uint16_t)(int16_t)-1750);
  camW16(0x56, 256);
}
void CutsceneCamera::pushMode(uint8_t mode) {   // FUN_8006E1C0
  camW8(0x67, camR8(0x64));   // stash the current mode
  camW8(0x64, mode);
  camW8(4, 0); camW8(5, 0); camW8(6, 0);
}
void CutsceneCamera::restoreMode() {   // FUN_8006E1E4
  if (r8(G + 2) == 1) {
    camW8(0x64, 0);
    camW32(0x0c, r32(MASTER_Y));
    return;
  }
  camW32(0x0c, r32(MASTER_Y));
  camW8(0x64, camR8(0x67));   // restore the mode pushMode() stashed
}
// FUN_8006EA00 pushes a real 32-byte guest frame (r29-=32, s0/s1/ra spilled at +16/+20/+24,
// restored symmetrically before every return) — confirmed against generated/shard_7.c
// gen_func_8006EA00. Since this leaf is wired GLOBALLY (any rec_dispatch(c, 0x8006EA00u) caller,
// substrate context included, per the "MIRROR THE GUEST STACK" directive), the frame is mirrored
// relative to the CALLER's live c->r[29] (not a fixed offset): spill the live s0/s1/ra so their
// bytes land in guest RAM exactly where the substrate would leave them, run the body (which never
// itself needs s0/s1 as registers — CAM_OBJ is a compile-time constant here, matching the gen's
// own hardcoded-constant s0/s1 load), then restore before returning (a nested call, e.g. lookAt's
// LA_ISQRT rec_dispatch, can clobber the shared Core::r[] register file, so the restore is a real
// requirement, not a formality).
void CutsceneCamera::snapToMasterOffsetY200() {   // FUN_8006EA00
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 20, c->r[17]);
  c->mem_w32(sp + 24, c->r[31]);
  // cam[8]/[0xc]/[0x10] are a 32-bit (X,Y,Z) staging triple (same shape trackXZ/trackY/snapAccXZ/snapAccY
  // read as `target`); only the HIGH (integer) half is written here — the low half keeps whatever was
  // already there, exactly like the guest (never "cleaned up").
  w16(CAM_OBJ + 0x0e, (uint16_t)((int16_t)r16(MASTER_Y + 2) - 200));   // cam[0xc].hi = MASTER_Y.hi - 200
  w16(CAM_OBJ + 0x0a, r16(MASTER_X + 2));                              // cam[8].hi   = MASTER_X.hi
  w16(CAM_OBJ + 0x12, r16(MASTER_Z + 2));                              // cam[0x10].hi= MASTER_Z.hi
  snapAccXZ(CAM_OBJ + 8);
  snapAccY(CAM_OBJ + 8);
  initPlace();
  lookAt();
  c->r[31] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] = sp + 32;
}
// FUN_8006EF38 pushes the same shape of 32-byte frame (r29-=32, s0/s1/ra spilled at +16/+20/+24)
// UNCONDITIONALLY — even on the early-return path (the gen's branch-delay-slot spill runs before
// the {3,4}-window check, and the early-return target is the same restore tail as the normal
// exit) — confirmed against generated/shard_2.c gen_func_8006EF38. Mirrored the same way as
// snapToMasterOffsetY200 above; see that method's comment for the rationale.
void CutsceneCamera::orbitTick() {   // FUN_8006EF38
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 20, c->r[17]);
  c->mem_w32(sp + 24, c->r[31]);
  if ((uint8_t)(r8(0x1F800236u) - 3) < 2) {   // only during render-timing window {3,4}
    int32_t angle = (int16_t)camR16(0x70);
    int32_t rc = trigOf(c).rcos(angle), rs = trigOf(c).rsin(angle);
    w16(S + 0x02, (uint16_t)((int16_t)camR16(0x3a) + (int16_t)(mlo(rc, 500) >> 12)));
    w16(S + 0x0a, (uint16_t)((int16_t)camR16(0x42) + (int16_t)(mlo(rs, 500) >> 12)));
    camW16(0x70, (uint16_t)(angle + 8));
    snapFollow(CAM_OBJ + 0x38);   // snap the camera's own position accumulators to the fixed orbit center
  }
  c->r[31] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] = sp + 32;
}

void CutsceneCamera::update() {   // FUN_8006EC44 (resident per-frame camera driver; cam obj @0x800E8008)
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x8006EC44u, updateFaithful()); return; }   // faithful: gen mirror
  uint8_t outer = camR8(0);                     // cam[0] = outer state
  if (outer == 0) { camW8(0, 1); init(); return; }   // first frame: init (no post-mode tail)
  if (outer != 1) return;                       // idle
  uint8_t ss = camR8(1);                        // cam[1] = sub-state
  if (ss == 0) { camW8(1, 1); camW8(2, 0); camW8(3, 0); }   // fall through into the run state
  else if (ss != 1) return;
  uint8_t mode = (uint8_t)(camR8(0x64) & 0x3f);
  camW8(0x66, 0);
  if (mode < 18) dispatchMode(mode);
  shakeTail();                                  // post-mode tail (always, after any mode)
}

// pc_faithful mirror of gen_func_8006EC44 (generated/shard_1.c:13176-13351). Guest frame: r29-=24,
// s0(r16)@sp+16 spilled with the CALLER's live value (Engine::fieldFrameFaithful leaves r16=0x1F800000
// there before calling), ra(r31)@sp+20 spilled with the caller's jal-site (0x80108B90u), s0 reassigned to
// CAM_OBJ (hardcoded in the gen, independent of any cam_ this instance was constructed with) for the body,
// both restored and the frame deallocated on every exit path (including the two early-return/idle paths,
// which skip shakeTail exactly like the gen's direct goto to the restore tail). Every callee gets r31 set
// to the exact gen jal-site constant first, matching the reference-mirror style (Engine::fieldFrameFaithful
// / Sop::fieldModeFaithful). init/mainFollow/rotBuild/trackFollow/snapFollow*/pitchFollow/simpleFollow/
// shakeTail are dispatched via rec_dispatch(c, addr) to their guest address — since no override-registry
// entry exists for any of them, this falls straight through to the substrate gen_func body (same code the
// oracle runs), NOT a call to the native sibling *methods* on this class (those exist for the pc_skip
// path only). Calling convention for the two-arg follow leaves (trackFollow/snapFollowA/pitchFollow/
// snapFollowB/snapFollow/simpleFollow) is a0(r4)=cam, a1(r5)=cam+56 (or G+0x2C for the "snap-to-master"
// variants of snapFollow/simpleFollow) — verified against the gen bodies AND the real mode-dispatch jump
// table at 0x80016A44 (18 uint32 entries, read from scratch/bin/tomba2/MAIN.EXE @ file offset 0x7244).
void CutsceneCamera::updateFaithful() {   // FUN_8006EC44
  uint8_t outer = camR8(0);
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);          // spill caller's live s0
  c->r[16] = CAM_OBJ;                     // s0 = CAM_OBJ (0x800E8008, hardcoded in the gen)
  c->mem_w32(sp + 20, c->r[31]);          // spill caller's live ra (jal-site set by the caller)

  if (outer == 0) {
    c->mem_w8(c->r[16] + 0, 1);
    c->r[31] = 0x8006EC84u;
    c->r[4] = c->r[16];
    rec_dispatch(c, 0x8006EA7Cu);   // init FUN_8006EA7C(cam) — substrate until its faithful conversion
    goto epilogue;
  }
  if (outer != 1) goto epilogue;
  {
    uint8_t ss = c->mem_r8(c->r[16] + 1);
    if (ss == 0) {
      c->mem_w8(c->r[16] + 1, 1);
      c->mem_w8(c->r[16] + 2, 0);
      c->mem_w8(c->r[16] + 3, 0);
    } else if (ss != 1) {
      goto epilogue;
    }
    uint8_t mode = (uint8_t)(c->mem_r8(c->r[16] + 100) & 63u);
    c->mem_w8(c->r[16] + 102, 0);
    if (mode < 18) dispatchModeFaithful(mode);
    c->r[31] = 0x8006EF28u;
    c->r[4] = c->r[16];
    rec_dispatch(c, 0x8006C988u);   // shakeTail FUN_8006C988(cam)
  }
epilogue:
  c->r[31] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] = sp + 24;
}

void CutsceneCamera::dispatchModeFaithful(uint8_t mode) {
  switch (mode) {
    case 0: {
      uint8_t rm = c->mem_r8(0x800BF870u);
      if (rm == 7)  { c->r[31] = 0x8006ED48u; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM7_M0);  return; }
      if (rm == 20) { c->r[31] = 0x8006ED58u; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM20_M0); return; }
      if (rm == 2)  { c->r[31] = 0x8006ED38u; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM2_M0);  return; }
      if (!(c->mem_r8(c->r[16] + 100) & 0x80)) {
        c->r[31] = 0x8006ED7Cu; c->r[4] = c->r[16]; rec_dispatch(c, 0x8006E0F0u);   // mainFollow(cam)
        c->r[31] = 0x8006ED84u; c->r[4] = c->r[16]; rec_dispatch(c, 0x8006E464u);   // rotBuild(cam)
      }
      rm = c->mem_r8(0x800BF870u);
      uint32_t fn = c->mem_r32(RENDER_FP_TABLE + (uint32_t)rm * 4);
      c->r[31] = 0x8006EDACu; c->r[4] = c->r[16];
      rec_dispatch(c, fn);
      return;
    }
    case 1: {
      uint8_t rm = c->mem_r8(0x800BF870u);
      if (rm == 7)  { c->r[31] = 0x8006EDFCu; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM7_M1);  return; }
      if (rm == 20) { c->r[31] = 0x8006EE1Cu; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM20_M1); return; }
      if (rm == 2)  { c->r[31] = 0x8006EE0Cu; c->r[4] = c->r[16]; rec_dispatch(c, OV_RENDER_RM2_M1);  return; }
      c->r[31] = 0x8006EE2Cu; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E228u);   // trackFollow(cam, cam+56)
      return;
    }
    case 2:  c->r[31] = 0x8006EE40u; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E294u); return;   // snapFollowA(cam, cam+56)
    case 3:  c->r[31] = 0x8006EE54u; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E360u); return;   // pitchFollow(cam, cam+56)
    case 4:  c->r[31] = 0x8006EE68u; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E2FCu); return;   // snapFollowB(cam, cam+56)
    case 5:  c->r[31] = 0x8006EE80u; c->r[4] = c->r[16]; c->r[5] = G + 0x2c; rec_dispatch(c, 0x8006E3B0u); return;   // snapFollow(cam, MASTER_X)
    case 6:
      c->mem_w8(c->r[16] + 100, 0);
      c->mem_w32(c->r[16] + 12, c->mem_r32(G + 0x30));
      return;
    case 7:
    case 14: c->r[31] = 0x8006EEF8u; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E3B0u); return;   // snapFollow(cam, cam+56)
    case 8:  c->r[31] = 0x8006EEA8u; c->r[4] = c->r[16]; c->r[5] = c->r[16] + 56; rec_dispatch(c, 0x8006E3F4u); return;   // simpleFollow(cam, cam+56)
    case 9:  c->r[31] = 0x8006EEB8u; c->r[4] = c->r[16]; rec_dispatch(c, OV_MODE9);   return;
    case 10: c->r[31] = 0x8006EEC8u; c->r[4] = c->r[16]; rec_dispatch(c, OV_A00_CAM); return;
    case 11:
    case 12:
      c->mem_w8(c->r[16] + 100, 0);
      c->mem_w8(c->r[16] + 2, 0);
      c->mem_w8(c->r[16] + 3, 0);
      return;
    case 13: c->mem_w8(c->r[16] + 100, 6); return;
    case 15: c->r[31] = 0x8006EF10u; c->r[4] = c->r[16]; c->r[5] = G + 0x2c; rec_dispatch(c, 0x8006E3F4u); return;   // simpleFollow(cam, MASTER_X)
    case 16: return;   // tail only (falls through to shakeTail in updateFaithful, same as the gen's L_8006EF20 fallthrough)
    case 17: c->r[31] = 0x8006EF20u; c->r[4] = c->r[16]; rec_dispatch(c, OV_MODE17); return;
    default: return;   // unreachable: mode<18 and the 18-entry table's values are all enumerated above
  }
}

void CutsceneCamera::init() {   // FUN_8006EA7C (first-frame field reset + render-mode-keyed mode selector)
  camW16(0x56, 256); camW8(0x72, 0); camW8(0x76, 0); camW8(0x77, 0); camW8(0x74, 0);
  camW16(6, 0);
  camW32(0x0c, r32(0x1F8000E0u));
  camW16(0x52, 1024); camW16(0x22, 240); camW16(0x8c, 0);

  uint8_t rm = r8(0x800BF870u);
  bool mainPath = false;   // whether we take the mainFollow(+E918) branch after selecting the mode
  // Render-mode jump table @0x800169EC: rm 0 -> special; {2,7,20} -> mode 0-cleared; 3 -> mode 14;
  // {16..19} -> mode 128; everything else (incl. rm>=21) -> default (mode 0-cleared + mainFollow path).
  if (rm == 0) {
    if (r8(0x800BF89Cu) == 2) {                 // pre-scripted camera fixup
      camW8(0x64, 7);
      camW16(0x3a, 2746); camW16(0x3e, (uint16_t)(int16_t)-800); camW16(0x42, 3808);
      w16(0x1F8000D2u, 3387); w16(0x1F8000D6u, (uint16_t)(int16_t)-2691); w16(0x1F8000DAu, 3506);
    } else if (r8(0x800BF816u) != 0) {
      /* skip mode-clear + mainFollow, go straight to the post-check */
    } else {
      camW8(0x64, 0); mainPath = true;
    }
  } else if (rm == 2 || rm == 7 || rm == 20) {
    camW8(0x64, 0);
  } else if (rm == 3) {
    camW8(0x64, 14);
  } else if (rm >= 16 && rm <= 19) {
    camW8(0x64, 128);
  } else {
    camW8(0x64, 0); mainPath = true;            // default label (incl. rm>=21)
  }

  if (mainPath) { mainFollow(); initPlace(); }

  // post-check (0x8006EBA8): when render-timing byte 0x1F800236 is 5 or 6, seed a scripted follow.
  if ((uint8_t)(r8(0x1F800236u) - 5) < 2) {
    initSeedGrp(G + 0x2c);
    uint16_t d3e = camR16(0x3e);
    camW16(0x3e, (uint16_t)(d3e + 1000));
    if (r8(G + 2) != 0) return;
    camW16(0x3e, (uint16_t)(d3e + 860));
    camW16(0x0e, (uint16_t)(d3e + 860));
    camW8(0x64, 15);
    camW16(0x6c, 1400); camW16(0x6e, 64);
    camW16(0x70, (uint16_t)(r16(G + 0x140) + 1024));
    snapFollowA(cam_ + 0x38);
    snapFollow(G + 0x2c);
  }
}

// ── Override-registry wiring (2026-07-08 frontier follow-up) ────────────────────────────────────
// resetFollowAccum/pushMode/restoreMode/snapToMasterOffsetY200/orbitTick reach the substrate
// EXCLUSIVELY via rec_dispatch(c, addr) from cross-module (overlay) call sites — confirmed by
// grepping every generated/*.c reference to these 5 addresses: none appear as a direct same-module
// `func_<addr>(c)` call EXCEPT pushMode (0x8006E1C0), which generated/shard_3.c, shard_4.c and
// shard_6.c also call directly (MAIN calling its own resident code — the recompiler emits that as
// a plain C call, bypassing rec_dispatch entirely and consulting the recompiler's OWN g_override[]
// table instead). So pushMode needs the dual-wire (shard_set_override setter passed to install(),
// same pattern as PcScheduler's primitives in game/core/pc_scheduler.cpp); the other 4 only need
// the rec_dispatch-only registration (setter omitted).
extern void gen_func_8006E1C0(Core*);

static void eov_resetFollowAccum(Core* c) {
  // a0 (c->r[4]) IS the target object base here — NOT hardcoded CAM_OBJ (see the RE note above
  // resetFollowAccum's definition). Construct the instance from the live a0.
  CutsceneCamera(c, c->r[4]).resetFollowAccum();
}
static void eov_pushMode(Core* c) {
  CutsceneCamera(c, CutsceneCamera::CAM_OBJ).pushMode((uint8_t)c->r[4]);
}
static void eov_restoreMode(Core* c) {
  CutsceneCamera(c, CutsceneCamera::CAM_OBJ).restoreMode();
}
static void eov_snapToMasterOffsetY200(Core* c) {
  CutsceneCamera(c, CutsceneCamera::CAM_OBJ).snapToMasterOffsetY200();
}
static void eov_orbitTick(Core* c) {
  CutsceneCamera(c, CutsceneCamera::CAM_OBJ).orbitTick();
}

extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_8006E8F8(Core*);
extern void gen_func_8006E1E4(Core*);
extern void gen_func_8006EA00(Core*);
extern void gen_func_8006EF38(Core*);

void CutsceneCamera::registerOverrides(Game* /*game*/) {
  using overrides::install;
  // pushMode (0x8006E1C0) has direct same-module callers -> shard_set_override installs the thunk;
  // the other four are rec_dispatch-only (setter omitted).
  install(0x8006E1C0u, "CutsceneCamera::pushMode",               eov_pushMode,               gen_func_8006E1C0, shard_set_override);
  install(0x8006E8F8u, "CutsceneCamera::resetFollowAccum",       eov_resetFollowAccum,       gen_func_8006E8F8);
  install(0x8006E1E4u, "CutsceneCamera::restoreMode",            eov_restoreMode,            gen_func_8006E1E4);
  install(0x8006EA00u, "CutsceneCamera::snapToMasterOffsetY200", eov_snapToMasterOffsetY200, gen_func_8006EA00);
  install(0x8006EF38u, "CutsceneCamera::orbitTick",              eov_orbitTick,              gen_func_8006EF38);
}

