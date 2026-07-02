// game/math/engine_math.h — the hot per-frame GTE-transform cluster (rotation-matrix compose + apply),
// PC-native + GTE-exact verified (see engine_math.cpp `mathverify`/`rotXverify`/etc.). Exposed so other
// render-transform code (game/render/engine_submit.cpp's node-transform propagation) can call these
// directly instead of round-tripping through rec_dispatch to the same guest address.
#ifndef GAME_MATH_ENGINE_MATH_H
#define GAME_MATH_ENGINE_MATH_H
struct Core;
void ov_mat_mul(Core* c);       // FUN_80084110 — 3x3 matrix multiply P = R x M (MVMVA, sf=1, rot, lm=0)
void ov_apply_matlv(Core* c);   // FUN_80084220 — MVMVA the CR0-4 rotation matrix by a vector -> IR1-3
void ov_rot_x(Core* c);         // FUN_80084D10 — compose an X-axis rotation onto a 3x3 matrix
void ov_rot_y(Core* c);         // FUN_80084EB0 — compose a  Y-axis rotation onto a 3x3 matrix
void ov_rot_z(Core* c);         // FUN_80085050 — compose a  Z-axis rotation onto a 3x3 matrix
void ov_rotmat(Core* c);        // FUN_80085480 — libgte RotMatrix: 3-euler SVECTOR* @a0 -> 3x3 MATRIX* @a1
#endif
