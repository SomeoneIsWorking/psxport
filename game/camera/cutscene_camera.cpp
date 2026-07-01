// class CutsceneCamera — the general FIELD / FOLLOW engine camera, PC-native (see cutscene_camera.h). It is
// the free-roam camera as well as the SOP/cutscene one (RESOLVED later-293 — docs/findings/camera.md).
// The arithmetic is the RE'd engine behaviour (docs/engine_re.md "CutsceneCamera"; later-173..176); this file
// restructures it into methods over named state (no guest register convention). It is verified per-call
// against the recomp oracle on the live SOP scene via `cam_snap_follow` (PSXPORT_DEBUG=camverify) and by the
// oracle UNIT TEST over every method incl. the driver (game/camera/cutscene_camera_test.cpp).
#include "cutscene_camera.h"
#include "cfg.h"
#include "mtx.h"
#include "trig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Retained libgte helpers (a math library — kept substrate, not PSX hardware).
// rsin (0x80083E80) and rcos (0x80083F50) are owned by class Trig (game/math/trig.h) — the class
// methods Trig::rsin / Trig::rcos have superseded the T2_RSIN / T2_RCOS `call(...)` shims here.
static constexpr uint32_t T2_ISQRT  = 0x80084080u;   // isqrt(x)      -> floor(sqrt)
static constexpr uint32_t T2_RATAN2 = 0x80085690u;   // ratan2(y,x)   -> angle12
// angleCmp (0x80077768) is owned by class Trig (game/math/trig.h) — Trig::angleCmp superseded T2_ANGCMP.
// lookAt's matrix helpers (NB its isqrt is a DIFFERENT entry from rotBuild's).
static constexpr uint32_t LA_ISQRT  = 0x80077FB0u;
static constexpr uint32_t LA_RATAN2 = 0x80085690u;
static constexpr uint32_t LA_MRINIT = 0x80051794u;   // MR_init (identity)
static constexpr uint32_t LA_MULMAT = 0x80084250u;   // MulMatrix0(a0,a1)
static constexpr uint32_t LA_APPLYLV= 0x80084470u;   // ApplyMatrixLV(m,v,out)
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

// ── teleport hook (REPL) ─────────────────────────────────────────────────────────────────────────
static int g_tp_pending = 0; static int32_t g_tp_x, g_tp_y, g_tp_z;
void cam_teleport(int x, int y, int z) { g_tp_x = x; g_tp_y = y; g_tp_z = z; g_tp_pending = 1; }
void cam_teleport_off(void) { g_tp_pending = 0; }

// ── follow accumulators ──────────────────────────────────────────────────────────────────────────
bool CutsceneCamera::trackXZ(uint32_t target) {   // FUN_8006D960
  if (g_tp_pending) {                                   // one-shot debug teleport of Tomba's master pos
    g_tp_pending = 0;
    w32(MASTER_X, (uint32_t)g_tp_x << 16);
    w32(MASTER_Y, (uint32_t)g_tp_y << 16);
    w32(MASTER_Z, (uint32_t)g_tp_z << 16);
    w32(G + 0x44, 0);                                   // master speed
    fprintf(stderr, "[tp] Tomba -> (%d,%d,%d)\n", g_tp_x, g_tp_y, g_tp_z);
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
  int32_t yaw  = (int16_t)call(T2_RATAN2, -dz, dx);
  int32_t dist = (int16_t)call(T2_ISQRT, dx * dx + dz * dz);
  int32_t rc2  = Trig::rcos(c, yaw);
  w32(S + 0x00, r32(S + 0x00) + ((rc2 * dist) >> 1));
  int32_t rs2  = Trig::rsin(c, yaw);
  w32(S + 0x08, r32(S + 0x08) - ((rs2 * dist) >> 1));
  if (dist < 401) camW8(0x66, camR8(0x66) | 1);
}
void CutsceneCamera::lookatTail(int32_t theta, int32_t radius) {
  int32_t rc    = Trig::rcos(c, theta);
  int32_t lookX = (int32_t)r16(0x1F800160u) + ((rc * radius) >> 12);
  int32_t rs    = Trig::rsin(c, theta);
  int32_t lookZ = (int32_t)r16(0x1F800164u) - ((rs * radius) >> 12);
  int32_t camZ = (int32_t)r16(S + 0x0A);
  int32_t camX = (int32_t)r16(S + 0x02);
  int32_t dz   = (int16_t)(lookZ - camZ);
  int32_t dx   = (int16_t)(lookX - camX);
  yawDistAccumulate(dx, dz);
}

// ── scripted-camera look-angle builders (0x8006DC38/DAD8/DF88/DEF0 — used by snapFollowA/B) ─────────
void CutsceneCamera::posBuildA() {   // FUN_8006DC38 — overwrite the X/Z look accumulators
  int32_t P  = mlo(Trig::rcos(c, (int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
  int32_t x  = (int32_t)(int16_t)r16(S + 0x0e) + (mlo(Trig::rcos(c, (int16_t)camR16(0x70)), P) >> 12);
  int32_t cz = mlo(Trig::rsin(c, (int16_t)camR16(0x70)), P) >> 12;
  w32(S + 0x00, (uint32_t)(x << 16));
  int32_t z  = (int32_t)(int16_t)r16(S + 0x16) - cz;
  w32(S + 0x08, (uint32_t)(z << 16));
  camW8(0x66, camR8(0x66) | 1);
}
void CutsceneCamera::posBuildB() {   // FUN_8006DAD8 — place a look point then yaw/dist accumulate
  int32_t P  = (int16_t)(mlo(Trig::rcos(c, (int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12);
  int32_t x  = (int32_t)(uint16_t)r16(S + 0x0e) + (mlo(Trig::rcos(c, (int16_t)camR16(0x70)), P) >> 12);
  int32_t cz = mlo(Trig::rsin(c, (int16_t)camR16(0x70)), P) >> 12;
  int32_t dz = (int16_t)((int32_t)(uint16_t)r16(S + 0x16) - cz - (int32_t)(uint16_t)r16(S + 0x0a));
  int32_t dx = (int16_t)(x - (int32_t)(uint16_t)r16(S + 0x02));
  yawDistAccumulate(dx, dz);
}
void CutsceneCamera::headBuildA(uint32_t nonzero) {   // FUN_8006DF88
  if (nonzero == 0) {
    int32_t off = (int32_t)(int16_t)camR16(0x26) - 320;
    w16(S + 6, (uint16_t)((int32_t)r16(S + 0x12) + off));
  } else {
    int32_t step = mlo(Trig::rsin(c, (int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
    w16(S + 6, (uint16_t)((int32_t)r16(S + 0x12) - step));
  }
  camW8(0x66, camR8(0x66) | 2);
}
void CutsceneCamera::headBuildB() {   // FUN_8006DEF0
  int32_t step = mlo(Trig::rsin(c, (int16_t)camR16(0x6e)), (int16_t)camR16(0x6c)) >> 12;
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
  int32_t g140 = (int16_t)c->mem_r16(G + 0x140), g56 = (int16_t)c->mem_r16(G + 0x56);
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
  int32_t s2 = tX + (mlo(Trig::rcos(c, s0a), cam22) << 4);
  int32_t s1 = tZ - (mlo(Trig::rsin(c, s0a), cam22) << 4);

  int32_t g140s = (int16_t)r16(G + 0x140);
  int32_t cam58 = (int32_t)camR32(0x58);
  int32_t coordX = tX + (mlo(Trig::rcos(c, g140s), cam58) >> 4);
  int32_t coordZ = tZ - (mlo(Trig::rsin(c, g140s), cam58) >> 4);
  int32_t s2q = (s2 - coordX) >> 8;
  int32_t s1q = (s1 - coordZ) >> 8;
  int32_t s0d = call(T2_ISQRT, mlo(s2q, s2q) + mlo(s1q, s1q)) << 8;
  int32_t ang = call(T2_RATAN2, -s1q, s2q);
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
  camW32(0x08, (uint32_t)(tX + (mlo(Trig::rcos(c, g140s), cam58n) >> 4)));
  camW32(0x10, (uint32_t)(tZ - (mlo(Trig::rsin(c, g140s), cam58n) >> 4)));
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
    Mtx::identity(c, S + 40);
    int32_t sinp = cam_idiv(c, dY  << 12, s18);
    int32_t cosp = cam_idiv(c, s19 << 12, s18);
    int32_t pitch = (int16_t)call(LA_RATAN2, sinp, cosp);
    w16(S + 32, (uint16_t)pitch);
    w16(S + 48, (uint16_t)cosp);
    w16(S + 56, (uint16_t)cosp);
    w16(S + 50, (uint16_t)(-sinp));
    w16(S + 54, (uint16_t)sinp);

    if (s19 != 0) {
      int32_t a = cam_idiv(c, (-dX) << 12, s19);
      int32_t b = cam_idiv(c, dZ    << 12, s19);
      int32_t yaw = (int16_t)call(LA_RATAN2, a, b);
      w16(S + 34, (uint16_t)yaw);
      Mtx::identity(c, 0x1F800000u);
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
  call(LA_APPLYLV, (int32_t)M, 0x1F8000C0, 0x1F80010C);
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
  snapAccXZ(target);                   // snap the follow accumulators to target (no smoothing)
  snapAccY(target);
  lookAt();
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
  if (camR8(0x76) == 0) {
    c->r[4] = cam_; rec_dispatch(c, 0x8006DAD8u);      // still-substrate sub-fns (read a0 only)
    c->r[4] = cam_; rec_dispatch(c, 0x8006DEF0u);
  }
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
      int32_t rx = c->rng.next() & 0x1f;
      w16(S + 0x02, (uint16_t)((int32_t)camR16(0x86) - 16 + rx));
      int32_t rz = c->rng.next() & 0x1f;
      w16(S + 0x0a, (uint16_t)((int32_t)camR16(0x8a) - 16 + rz));
      int32_t ry = c->rng.next() & 0xf;
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
      int32_t r = c->rng.next() & 0x3f;
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
        int32_t r = c->rng.next() & 0x3f;
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
        int32_t r = c->rng.next() & 0x1f;
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
  int32_t cx = (int32_t)(uint16_t)r16(G + 0x2e) + (mlo(Trig::rcos(c, angle), s1) >> 12);
  w16(S + 0x02, (uint16_t)cx);
  int32_t cz = (int32_t)(uint16_t)r16(G + 0x36) - (mlo(Trig::rsin(c, angle), s1) >> 12);
  w16(S + 0x0a, (uint16_t)cz);
}
void CutsceneCamera::initSeedGrp(uint32_t src) {   // FUN_8006CBA8 (writes the FIXED driver cam @0x800E8008)
  w16(CAM_OBJ + 0x3a, r16(src + 2));
  w16(CAM_OBJ + 0x3e, r16(src + 6));
  w16(CAM_OBJ + 0x42, r16(src + 10));
}

void CutsceneCamera::update() {   // FUN_8006EC44 (resident per-frame camera driver; cam obj @0x800E8008)
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

// ── live-spine entry points (static; class member interface) ─────────────────────────────────────
void CutsceneCamera::runFieldUpdate(Core* c) {
  CutsceneCamera(c, CAM_OBJ).update();
}

void CutsceneCamera::runSnapFollow(Core* c, uint32_t cam, uint32_t target) {
  if (!cfg_dbg("camverify")) { CutsceneCamera(c, cam).snapFollow(target); return; }

  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1F800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);

  CutsceneCamera(c, cam).snapFollow(target);
  uint32_t camM[64], spM[256];
  for (int i = 0; i < 64;  i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spM[i]  = c->mem_r32(0x1F800000u + i * 4);

  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1F800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  c->r[4] = cam; c->r[5] = target;
  rec_interp(c, 0x8006E3B0u);
  uint32_t camO[64], spO[256];
  for (int i = 0; i < 64;  i++) camO[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spO[i]  = c->mem_r32(0x1F800000u + i * 4);

  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[camverify] snapFollow cam+0x%02x mine=%08x oracle=%08x\n", i*4, camM[i], camO[i]); }
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[camverify] snapFollow sp+0x%03x mine=%08x oracle=%08x\n", i*4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[camverify] snapFollow match #%d\n", ngood);
}
