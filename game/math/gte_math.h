// game/math/gte_math.h — the hot per-frame GTE-transform CLUSTER (rotation-matrix compose + apply).
//
// class Math — instance subsystem owned by Core (`Core::math`), back-pointer wired in Core::Core().
// Callers reach it as `mathOf(c).matMul(rPtr, mPtr, outPtr)` — no Core* on the method surface. Was
// previously the `A::B(core, …)` static-with-Core shape; promoted to instance-with-back-pointer to
// match the standard OOP shape (same as Rng / Trig on Core, Collision / Bit / Spawn on Engine).
//
// SCOPE: 3x3 matrix multiply (FUN_80084110), MVMVA apply (FUN_80084220), 3-Euler RotMatrix
// (FUN_80085480), and the 3 axis-rotation composers RotMatrixX/Y/Z (FUN_80084D10 / EB0 / 5050).
// All PC-native + GTE-bit-exact (verified via the `mathverify` gate in gte_math.cpp); each writes
// the GTE data-reg leftovers a downstream still-PSX `gte_op` reader could consume, so the coupling
// is preserved.
//
// Was originally the free functions ov_mat_mul / ov_apply_matlv / ov_rotmat / ov_rot_x/y/z, each
// taking its arguments via MIPS taxi (c->r[4/5/6]). Now real instance methods with typed pointer/
// angle arguments — no taxi marshal at any layer.
#ifndef GAME_MATH_ENGINE_MATH_H
#define GAME_MATH_ENGINE_MATH_H
#include <stdint.h>
class Core;

class Math {
public:
  Core* core = nullptr;

  // matMul(rPtr, mPtr, outPtr): FUN_80084110 — 3x3 matrix multiply P = R x M written to outPtr
  //   in CR-packed layout (5 words). Also CTC2-loads R into GTE CR0-4 so a following MVMVA
  //   (applyMatlv) sees this matrix. Returns outPtr via v0 (c->r[2]).
  uint32_t matMul(uint32_t rPtr, uint32_t mPtr, uint32_t outPtr);

  // applyMatlv(inPtr, outPtr): FUN_80084220 — MVMVA the rotation matrix already in CR0-4 by the
  //   vector at inPtr; writes 3 sign-extended s32 words at outPtr. Returns outPtr via v0.
  uint32_t applyMatlv(uint32_t inPtr, uint32_t outPtr);

  // applyMatrixLV(mPtr, inPtr, outPtr): FUN_80084470 — libgte ApplyMatrixLV. Load the 3x3 rotation
  //   matrix from mPtr into GTE CR0-4, then MVMVA the vector at inPtr by it and write the 3 MAC
  //   words (unclamped s32) at outPtr. Differs from applyMatlv in that (a) the matrix is loaded
  //   here (not assumed already in CR), and (b) the output is MAC1-3, not IR1-3.
  uint32_t applyMatrixLV(uint32_t mPtr, uint32_t inPtr, uint32_t outPtr);

  // rotmat(anglesPtr, outPtr): FUN_80085480 — libgte RotMatrix. Build a 3x3 rotation matrix from
  //   3 Euler angles at anglesPtr (SVECTOR: vx/vy/vz s16); output written to outPtr in CR-packed
  //   layout (5 words). Also loads GTE leftovers a downstream reader consumes.
  uint32_t rotmat(uint32_t anglesPtr, uint32_t outPtr);

  // rotX/Y/Z(angle, matPtr): FUN_80084D10 / 80084EB0 / 80085050 — compose an axis rotation onto
  //   the matrix at matPtr (in-place). PURE — no GTE ops. Returns matPtr via v0.
  uint32_t rotX(int16_t angle, uint32_t matPtr);
  uint32_t rotY(int16_t angle, uint32_t matPtr);
  uint32_t rotZ(int16_t angle, uint32_t matPtr);

  // sqrtLzc(v): FUN_80084080 — GTE-LZC fixed-point square root (leading-sign-bit normalize + a
  //   16-bit ROM recip-sqrt table lookup @0x800a6310). Returns the isqrt-like result via v0.
  uint32_t sqrtLzc(uint32_t v);

  // matLoadLV(rPtr, vPtr): FUN_80084360 — in-place 3x3 matrix multiply P = R x M (R at rPtr,
  //   M at vPtr, result written back to vPtr in CR-packed layout). Same math as matMul. Returns vPtr.
  uint32_t matLoadLV(uint32_t rPtr, uint32_t vPtr);

  // matColScale(dstPtr, facPtr): FUN_80084520 — per-column fixed-point scale of the CR-packed 3x3
  //   matrix at dstPtr by the 3 column factors at facPtr (element * f[col] >> 12). Returns dstPtr.
  uint32_t matColScale(uint32_t dstPtr, uint32_t facPtr);

  // Wire all 7 addresses above into the global override registry (guest-ABI trampolines: args in
  // r4..r6, return in r2) so EVERY caller reaching them via rec_dispatch or a direct shard call —
  // substrate included, not just the direct mathOf(c).* call sites in node_xform.cpp/
  // cutscene_camera.cpp/graphics_bind.cpp — runs this native bit-exact math instead of
  // interpreting GTE ops. Called once from boot.cpp.
  void registerOverrides();
};

#endif
