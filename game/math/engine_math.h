// game/math/engine_math.h — the hot per-frame GTE-transform CLUSTER (rotation-matrix compose + apply).
//
// STATELESS class-namespace — pure operations over Core*, all methods `static`, no instance and no
// embedded member on Engine. Callers pass Core* explicitly: `Math::rotmat(c, a, b)`. Grouping is
// the point (Math is the boundary for the 6 hot GTE-transform entries the engine calls every frame,
// even though it holds no fields). See CLAUDE.md "REAL C++ CLASSES" — instance-with-back-pointer
// would be fake state; the extra Core* arg is not a real cost.
//
// SCOPE: 3x3 matrix multiply (FUN_80084110), MVMVA apply (FUN_80084220), 3-Euler RotMatrix
// (FUN_80085480), and the 3 axis-rotation composers RotMatrixX/Y/Z (FUN_80084D10 / EB0 / 5050).
// All PC-native + GTE-bit-exact (verified via the `mathverify` gate in engine_math.cpp); each writes
// the GTE data-reg leftovers a downstream still-PSX `gte_op` reader could consume, so the coupling
// is preserved.
//
// Was the free functions ov_mat_mul / ov_apply_matlv / ov_rotmat / ov_rot_x/y/z, each taking its
// arguments via MIPS taxi parameters (c->r[4/5/6]). Now real static methods with typed pointer/angle
// arguments — no taxi marshal at any layer, the body reads directly from the args.
#ifndef GAME_MATH_ENGINE_MATH_H
#define GAME_MATH_ENGINE_MATH_H
#include <stdint.h>
class Core;

class Math {
public:
  // matMul(c, rPtr, mPtr, outPtr): FUN_80084110 — 3x3 matrix multiply P = R x M written to outPtr
  //   in CR-packed layout (5 words). Also CTC2-loads R into GTE CR0-4 so a following MVMVA
  //   (applyMatlv) sees this matrix. Returns outPtr via v0 (c->r[2]).
  static uint32_t matMul(Core* c, uint32_t rPtr, uint32_t mPtr, uint32_t outPtr);

  // applyMatlv(c, inPtr, outPtr): FUN_80084220 — MVMVA the rotation matrix already in CR0-4 by the
  //   vector at inPtr; writes 3 sign-extended s32 words at outPtr. Returns outPtr via v0.
  static uint32_t applyMatlv(Core* c, uint32_t inPtr, uint32_t outPtr);

  // rotmat(c, anglesPtr, outPtr): FUN_80085480 — libgte RotMatrix. Build a 3x3 rotation matrix from
  //   3 Euler angles at anglesPtr (SVECTOR: vx/vy/vz s16); output written to outPtr in CR-packed
  //   layout (5 words). Also loads GTE leftovers a downstream reader consumes.
  static uint32_t rotmat(Core* c, uint32_t anglesPtr, uint32_t outPtr);

  // rotX/Y/Z(c, angle, matPtr): FUN_80084D10 / 80084EB0 / 80085050 — compose an axis rotation onto
  //   the matrix at matPtr (in-place). PURE — no GTE ops. Returns matPtr via v0.
  static uint32_t rotX(Core* c, int16_t angle, uint32_t matPtr);
  static uint32_t rotY(Core* c, int16_t angle, uint32_t matPtr);
  static uint32_t rotZ(Core* c, int16_t angle, uint32_t matPtr);
};

#endif
