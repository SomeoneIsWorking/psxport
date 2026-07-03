// engine_math.cpp — PC-native reimplementations of the hot libgte-style MATH helpers the engine
// calls every frame. The port is interpreter-only, so each of these pure leaf routines runs as
// interpreted MIPS on the hot path; owning them native is the #1-priority lever (perf + 100%-PC-
// native — they align). These are deterministic pure-integer/math leaves with NO PSX intricacy, so
// the PC-native form is just the same math in C; bit-exactness with the recomp reference IS the
// correctness gate (the result feeds cull distance / camera / content), proven by the per-call
// `mathverify` comparator below and then registered unconditionally.
//
// Profiled hot-list (field, later-186, docs/port-progress.md §F): FUN_80077FB0 isqrt = 8.41% of all
// interpreter instructions (and a frequency leader). First port here.
#include "core.h"
#include "cfg.h"
#include "engine_math.h"   // class Math — static entry surface + ov_* free-fn decls for internal reuse
#include <stdio.h>
#include <string.h>

void rec_interp(Core* c, uint32_t pc);

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80077FB0 — 16-bit ROUNDING integer square root. a0 = unsigned value, returns v0 = nearest
// integer r in [0, 0xffff] with r ≈ sqrt(a0). Algorithm (transcribed from the asm, exactly):
//   - high-bit seed: if a0 > 0x3fffffff the top result bit (0x8000) is pre-set via hi=0xffff8000.
//   - binary search bits 0x4000..0x1: accept candidate c when a0 >= (c & 0xffff)^2 (low-32 mult).
//   - final round-up: with r = result & 0xffff (unless r==0xffff), add 1 iff r^2 + r < a0
//     (i.e. a0 > r·(r+1) ⇒ a0 is closer to (r+1)^2). Return r & 0xffff.
// Pure leaf, no memory/GTE — bit-exact by construction; the comparator confirms on live calls.
static uint32_t isqrt16(uint32_t a0) {
  uint32_t hi = (a0 > 0x3fffffffu) ? 0xffff8000u : 0u;   // v0 seed (top bit)
  uint32_t a1 = hi;                                       // result accumulator
  // first step: bit 0x4000, candidate masked with 0xc000 (lower bits are 0 here)
  uint32_t a2 = hi + 0x4000u;
  {
    uint32_t v1 = a2 & 0xc000u;
    uint32_t a3 = (v1 * v1) & 0xffffffffu;
    if (!(a0 < a3)) a1 = a2;                              // accept when a0 >= candidate^2
  }
  // generic steps: bits 0x2000 .. 0x1, candidate masked with 0xffff
  for (uint32_t bit = 0x2000u; bit != 0u; bit >>= 1) {
    uint32_t v1 = a1 + bit;
    uint32_t v0 = v1 & 0xffffu;
    uint32_t a3 = (v0 * v0) & 0xffffffffu;
    if (!(a0 < a3)) a1 = v1;                              // accept
  }
  // final rounding
  uint32_t r = a1 & 0xffffu;
  if (r != 0xffffu) {
    uint32_t a3 = (r * r) & 0xffffffffu;
    uint32_t s  = (a3 + r) & 0xffffffffu;
    if (s < a0) a1 += 1;                                  // round up toward (r+1)
  }
  return a1 & 0xffffu;
}

// Exposed for the native per-object cull (game/render/cull.cpp): the cull body computes
// dist = isqrt16(dx²+dy²+dz²) exactly as FUN_80077FB0 does, so it must use the SAME bit-exact leaf.
uint32_t eng_isqrt16(uint32_t a0) { return isqrt16(a0); }

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084110 — 3x3 MATRIX MULTIPLY P = R × M via the GTE. a0 = matrix R, a1 = matrix M, a2 = out P
// (all 3x3 int16, GTE row-major CR layout: 5 words = [11|12, 13|21, 22|23, 31|32, 33]). 16.2% of hot
// interpreter time + the top frequency leader (55k calls) — the biggest single perf lever. The recomp
// body CTC2-loads R into the rotation-matrix regs, then for each COLUMN j of M runs MVMVA (sf=1→>>12,
// mx=ROT, v=Vj, cv=Null, lm=0) and packs the 3 IR outputs into P. USER DIRECTIVE 2026-06-21: port the
// GTE math NATIVE (perf) — must stay GTE-EXACT (output feeds object position → content). Exact MVMVA
// from vendor gte.c: MAC(1+i) = (Σ_k R[i][k]·Mcol_j[k]) >> 12 with 44-bit running accumulation (A_MV);
// IR(1+i) = clamp(MAC, lm=0 → signed [-32768,32767]). P[i][j] = IR(1+i) for column j → P = R·M.
static inline int64_t sign44(int64_t v) {                 // A_MV: sign-extend bit43 (overflow flags unread)
  v &= (INT64_C(1) << 44) - 1;
  if (v & (INT64_C(1) << 43)) v |= ~((INT64_C(1) << 44) - 1);
  return v;
}
static inline int16_t clamp16s(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int16_t)v); }
static void load_mat3(Core* c, uint32_t p, int16_t m[3][3]) {
  uint32_t w0 = c->mem_r32(p), w1 = c->mem_r32(p+4), w2 = c->mem_r32(p+8), w3 = c->mem_r32(p+12), w4 = c->mem_r32(p+16);
  m[0][0]=(int16_t)w0;       m[0][1]=(int16_t)(w0>>16); m[0][2]=(int16_t)w1;
  m[1][0]=(int16_t)(w1>>16); m[1][1]=(int16_t)w2;       m[1][2]=(int16_t)(w2>>16);
  m[2][0]=(int16_t)w3;       m[2][1]=(int16_t)(w3>>16); m[2][2]=(int16_t)w4;
}
uint32_t Math::matMul(uint32_t rPtr, uint32_t mPtr, uint32_t outPtr) { Core* c = this->core;
  int16_t R[3][3], M[3][3], P[3][3];
  load_mat3(c, rPtr, R);
  load_mat3(c, mPtr, M);
  // Faithful CTC2: the real body loads R into the GTE rotation-matrix CR0-4 and leaves it there, so a
  // following MVMVA (FUN_80084220 Math::applyMatlv reads CR0-4) sees this matrix. Replicate it so the CR
  // coupling is robust regardless of call order (was previously relying on a prior 80084470's CR write).
  for (int i=0;i<5;i++) gte_write_ctrl(i, c->mem_r32(rPtr + i*4));
  int32_t mac1=0, mac2=0, mac3=0;  // trailing MAC1-3 = last column's pre-clamp results (GTE leftover)
  for (int j = 0; j < 3; j++) {              // column j of M = MVMVA vector Vj
    for (int i = 0; i < 3; i++) {            // matrix row i
      int64_t tmp = 0;
      tmp = sign44(tmp + (int32_t)R[i][0] * M[0][j]);
      tmp = sign44(tmp + (int32_t)R[i][1] * M[1][j]);
      tmp = sign44(tmp + (int32_t)R[i][2] * M[2][j]);
      int32_t mac = (int32_t)(tmp >> 12);
      P[i][j] = clamp16s(mac);
      if (j == 2) { if (i==0) mac1=mac; else if (i==1) mac2=mac; else mac3=mac; }
    }
  }
  c->mem_w32(outPtr,    (uint16_t)P[0][0] | ((uint32_t)(uint16_t)P[0][1] << 16));
  c->mem_w32(outPtr+4,  (uint16_t)P[0][2] | ((uint32_t)(uint16_t)P[1][0] << 16));
  c->mem_w32(outPtr+8,  (uint16_t)P[1][1] | ((uint32_t)(uint16_t)P[1][2] << 16));
  c->mem_w32(outPtr+12, (uint16_t)P[2][0] | ((uint32_t)(uint16_t)P[2][1] << 16));
  c->mem_w32(outPtr+16, (uint32_t)(int32_t)P[2][2]);  // SWC2 IR3: sign-extended 32-bit, not a packed halfword
  // GTE leftover state a downstream gte_op reader could consume (the recomp body leaves the LAST
  // column's input vector in DR0/DR1 (VXY0/VZ0) + that column's MVMVA result in IR1-3/MAC1-3).
  gte_write_data(0, (uint32_t)(uint16_t)M[0][2] | ((uint32_t)(uint16_t)M[1][2] << 16));  // VXY0 = col2 (VX,VY)
  gte_write_data(1, (uint32_t)(uint16_t)M[2][2]);    // VZ0 = col2 (VZ)
  gte_write_data(9,  (uint32_t)(uint16_t)P[0][2]);   // IR1
  gte_write_data(10, (uint32_t)(uint16_t)P[1][2]);   // IR2
  gte_write_data(11, (uint32_t)(uint16_t)P[2][2]);   // IR3
  gte_write_data(25, (uint32_t)mac1);                // MAC1
  gte_write_data(26, (uint32_t)mac2);                // MAC2
  gte_write_data(27, (uint32_t)mac3);                // MAC3
  return outPtr;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084470 — libgte ApplyMatrixLV. Sibling of applyMatlv (FUN_80084220) with two differences:
//   (a) the 3x3 rotation matrix is loaded from a0 (mPtr) into GTE CR0-4 via CTC2 up front — not
//       assumed to already be in the control regs;
//   (b) the SWC2 output stores GTE data regs 25/26/27 (MAC1-3) — the UNCLAMPED s32 accumulator —
//       into a2, not the clamped IR1-3 that applyMatlv writes.
// The MVMVA opcode itself is identical to applyMatlv (sf=1, mx=ROT, v=V0, cv=Null, lm=0).
// Same 44-bit accumulator + >>12 as matMul. Returns outPtr in v0.
uint32_t Math::applyMatrixLV(uint32_t mPtr, uint32_t inPtr, uint32_t out) { Core* c = this->core;
  // Load matrix into CR0-4 (faithful CTC2). Also read it as R[3][3] for the MVMVA math.
  for (int i=0;i<5;i++) gte_write_ctrl(i, c->mem_r32(mPtr + i*4));
  int16_t R[3][3]; load_mat3(c, mPtr, R);
  uint32_t w0 = c->mem_r32(inPtr), w1 = c->mem_r32(inPtr+4);
  int16_t v[3] = { (int16_t)w0, (int16_t)(w0>>16), (int16_t)w1 };
  int32_t mac[3]; int16_t ir[3];
  for (int i=0;i<3;i++) {
    int64_t t=0;
    t = sign44(t + (int32_t)R[i][0]*v[0]);
    t = sign44(t + (int32_t)R[i][1]*v[1]);
    t = sign44(t + (int32_t)R[i][2]*v[2]);
    mac[i] = (int32_t)(t >> 12); ir[i] = clamp16s(mac[i]);
  }
  // Store MAC (unclamped s32), not IR — that's the ApplyMatrixLV signature.
  c->mem_w32(out,   (uint32_t)mac[0]);
  c->mem_w32(out+4, (uint32_t)mac[1]);
  c->mem_w32(out+8, (uint32_t)mac[2]);
  // GTE leftovers: VXY0/VZ0 = the MTC2'd input; IR1-3, MAC1-3 = the MVMVA result.
  gte_write_data(0, w0); gte_write_data(1, w1);
  gte_write_data(9,(uint32_t)(int32_t)ir[0]); gte_write_data(10,(uint32_t)(int32_t)ir[1]); gte_write_data(11,(uint32_t)(int32_t)ir[2]);
  gte_write_data(25,(uint32_t)mac[0]); gte_write_data(26,(uint32_t)mac[1]); gte_write_data(27,(uint32_t)mac[2]);
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80085480 — libgte RotMatrix: build a 3x3 rotation matrix from 3 Euler angles. a0 = SVECTOR* of
// angles (vx@+0, vy@+2, vz@+4, 16-bit fixed: 4096 = full turn), a1 = MATRIX* out (also returned in v0).
// ~17% of hot interpreter time — the hottest fn after ov_mat_mul. USER 2026-06-21: GTE is PSX hardware
// → port the MATH PC-native plain C (NO gte_op/Beetle); output is the rotation matrix the retained PSX
// content reads, so it must be GTE-EXACT (content-interface gate). Verified per-call by ov_rotmat_verify.
//
// The recomp body: (1) for each angle, abs+&0xfff indexes the SIN/COS LUT @0x800a6490 (word = sin in low
// half, cos in high half), then re-applies the angle's sign to sin (sin is odd, cos even). (2) Composes
// the matrix R = Rz·Ry·Rx via the GTE GPF op (cmd 0x3d, sf=1: MAC_i=(IR0*IR_i)>>12, IR_i=clamp16(MAC_i))
// used as a clamped scalar×vector multiplier, interleaved with two native 16-bit mults (cy·cx, cy·sx)
// that are >>12 then truncated (NOT clamped). Element order packed into out[+0..+16] exactly as the asm.
static inline void rotmat_trig(Core* c, int32_t angle, int* s, int* co) {
  int32_t sign = angle >> 31;                              // 0 or 0xffffffff (arithmetic)
  uint32_t absa = (uint32_t)((angle + sign) ^ sign);       // abs(angle)
  uint32_t idx = (absa << 2) & 0x3ffcu;                    // (abs & 0xfff) * 4
  uint32_t word = c->mem_r32(0x800a6490u + idx);
  *co = (int)((int32_t)word >> 16);                        // cos = high half (signed), sign-independent
  uint32_t at = (word << 16);                              // sin = low half, then re-apply angle sign:
  at = at + (uint32_t)sign; at = at ^ (uint32_t)sign; at = at >> 16;
  *s = (int)(int16_t)at;
}
static inline int16_t gpf1(int ir0, int ir) { return clamp16s(((int32_t)ir0 * ir) >> 12); }  // MAC=(IR0*IR)>>12, clamp16
static inline uint8_t lmC(int32_t v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }  // GTE Lm_C
uint32_t Math::rotmat(uint32_t anglesPtr, uint32_t out) { Core* c = this->core;
  uint32_t w0 = c->mem_r32(anglesPtr);
  int sx,cx,sy,cy,sz,cz;
  rotmat_trig(c, (int16_t)w0,        &sx, &cx);            // vx (low half of +0)
  rotmat_trig(c, (int16_t)(w0>>16),  &sy, &cy);            // vy (high half of +0)
  rotmat_trig(c, c->mem_r16s(anglesPtr+4), &sz, &cz);   // vz (+4)
  // GPF rounds (clamped scalar×vector products), in the asm's order:
  int16_t cxsy = gpf1(cx, sy), cxsz = gpf1(cx, sz), cxcz = gpf1(cx, cz);            // R1: IR0=cx
  int16_t sxsy = gpf1(sx, sy), sxsz = gpf1(sx, sz), sxcz = gpf1(sx, cz);            // R2: IR0=sx
  int16_t czcy = gpf1(cz, cy), cz_sxsy = gpf1(cz, sxsy), cz_cxsy = gpf1(cz, cxsy); // R3: IR0=cz
  int16_t szcy = gpf1(sz, cy), sz_sxsy = gpf1(sz, sxsy), sz_cxsy = gpf1(sz, cxsy); // R4: IR0=sz
  // native 16-bit mults: >>12 then truncated to 16 (NOT clamped)
  int16_t cycx = (int16_t)(((int32_t)cy * cx) >> 12);
  int16_t cysx = (int16_t)(((int32_t)cy * sx) >> 12);
  // matrix elements (truncating sums/negations to 16 bits as the sh/sw stores do)
  int16_t m00=czcy, m01=(int16_t)-szcy, m02=(int16_t)sy;
  int16_t m10=(int16_t)(cz_sxsy + cxsz), m11=(int16_t)(cxcz - sz_sxsy), m12=(int16_t)-cysx;
  int16_t m20=(int16_t)(sxsz - cz_cxsy), m21=(int16_t)(sxcz + sz_cxsy), m22=cycx;
  c->mem_w32(out+0,  (uint16_t)m00 | ((uint32_t)(uint16_t)m01 << 16));
  c->mem_w32(out+4,  (uint16_t)m02 | ((uint32_t)(uint16_t)m10 << 16));
  c->mem_w32(out+8,  (uint16_t)m11 | ((uint32_t)(uint16_t)m12 << 16));
  c->mem_w32(out+12, (uint16_t)m20 | ((uint32_t)(uint16_t)m21 << 16));
  c->mem_w16(out+16, (uint16_t)m22);
  // GTE leftover state a still-PSX gte_op reader could consume. The body runs 4 GPFs; the last leaves
  // IR0=sz, IR1-3=clamp16(R4 MACs), MAC1-3=raw R4 products, and the RGB FIFO holds R2/R3/R4 colors.
  int32_t r4m1=((int32_t)sz*cy)>>12, r4m2=((int32_t)sz*sxsy)>>12, r4m3=((int32_t)sz*cxsy)>>12;
  gte_write_data(8,  (uint32_t)(int32_t)(int16_t)sz);
  gte_write_data(9,  (uint32_t)(int32_t)szcy);
  gte_write_data(10, (uint32_t)(int32_t)sz_sxsy);
  gte_write_data(11, (uint32_t)(int32_t)sz_cxsy);
  gte_write_data(25, (uint32_t)r4m1);
  gte_write_data(26, (uint32_t)r4m2);
  gte_write_data(27, (uint32_t)r4m3);
  // RGB FIFO push: each GPF does MAC_to_RGB_FIFO (shift + new color = Lm_C(MAC_i>>4) | RGB_CD<<24).
  // After 4 pushes the FIFO holds R2,R3,R4 colors. RGB_CD = current RGBC reg high byte (unchanged).
  uint32_t cd = (gte_read_data(6) >> 24) & 0xff;
  int32_t r2m1=((int32_t)sx*sy)>>12, r2m2=((int32_t)sx*sz)>>12, r2m3=((int32_t)sx*cz)>>12;
  int32_t r3m1=((int32_t)cz*cy)>>12, r3m2=((int32_t)cz*sxsy)>>12, r3m3=((int32_t)cz*cxsy)>>12;
  #define MKCOL(a,b,d) ((uint32_t)lmC((a)>>4) | ((uint32_t)lmC((b)>>4)<<8) | ((uint32_t)lmC((d)>>4)<<16) | (cd<<24))
  gte_write_data(20, MKCOL(r2m1,r2m2,r2m3));
  gte_write_data(21, MKCOL(r3m1,r3m2,r3m3));
  gte_write_data(22, MKCOL(r4m1,r4m2,r4m3));
  #undef MKCOL
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80085050 / FUN_80084EB0 / FUN_80084D10 — compose an AXIS rotation onto a matrix (libgte
// RotMatrixZ/Y/X-class). a0 = angle (16-bit fixed), a1 = MATRIX* (in/out, also returned in v0). PURE —
// NO GTE ops: SIN/COS LUT @0x800a6490 + native multiplies only. All three are ONE kernel that rotates
// two ROWS (rowA, rowB) of the 3x3 matrix in the plane: rowA' = (cos·rowA − sin·rowB)>>12,
// rowB' = (sin·rowA + cos·rowB)>>12 (element-wise over the 3 columns). They differ only in (rowA,rowB)
// byte offsets and the sin sign for positive angles (the Y-axis variant 80084EB0 negates it):
//   80085050: rows +0/+6,  posSin=+1   80084EB0: rows +0/+12, posSin=−1   80084D10: rows +6/+12, posSin=+1
// The asm uses `multu` but only reads MFLO (low 32 bits) = the signed product for 16-bit operands → plain
// signed int math is bit-exact. cos = LUT word high half; sin = low half, sign by (angle<0)^(posSin<0).
static inline void rotpair_trig(Core* c, uint32_t a0, int posSin, int* s, int* co) {
  int32_t a = (int32_t)a0;
  uint32_t absidx = (a >= 0) ? ((uint32_t)a & 0xfffu) : ((0u - (uint32_t)a) & 0xfffu);
  uint32_t w = c->mem_r32(0x800a6490u + (absidx << 2));
  *co = (int)((int32_t)w >> 16);
  int sinv = (int)(int16_t)w;
  if (a < 0) sinv = -sinv;          // sin is odd → negate for negative angle
  if (posSin < 0) sinv = -sinv;     // Y-axis variant flips the sign
  *s = sinv;
}
static uint32_t rotpair(Core* c, int16_t angle, uint32_t matPtr, uint32_t rowA, uint32_t rowB, int posSin) {
  int s, co; rotpair_trig(c, (uint32_t)(int32_t)angle, posSin, &s, &co);
  int16_t A[3], B[3];
  for (int i = 0; i < 3; i++) { A[i] = c->mem_r16s(matPtr + rowA + i*2); B[i] = c->mem_r16s(matPtr + rowB + i*2); }
  for (int i = 0; i < 3; i++) c->mem_w16(matPtr + rowA + i*2, (uint16_t)(int16_t)(((int32_t)co*A[i] - (int32_t)s*B[i]) >> 12));
  for (int i = 0; i < 3; i++) c->mem_w16(matPtr + rowB + i*2, (uint16_t)(int16_t)(((int32_t)s*A[i]  + (int32_t)co*B[i]) >> 12));
  return matPtr;
}
uint32_t Math::rotZ(int16_t angle, uint32_t matPtr) { Core* c = this->core; return rotpair(c, angle, matPtr, 0,  6, +1); }   // FUN_80085050
uint32_t Math::rotY(int16_t angle, uint32_t matPtr) { Core* c = this->core; return rotpair(c, angle, matPtr, 0, 12, -1); }   // FUN_80084EB0
uint32_t Math::rotX(int16_t angle, uint32_t matPtr) { Core* c = this->core; return rotpair(c, angle, matPtr, 6, 12, +1); }   // FUN_80084D10

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084220 — MVMVA the rotation matrix already in the GTE CR regs by a vector → IR1-3 (libgte
// ApplyMatrixLV-class). a0 = SVECTOR* in (VX,VY @+0 packed, VZ @+4 low half), a1 = VECTOR* out (3
// sign-extended 32-bit words), returned in v0. The body: MTC2 a0 words → VXY0/VZ0, GTE MVMVA (sf=1,
// mx=ROT, v=V0, cv=Null, lm=0) reading the rotation matrix from CR0-4 (loaded by a prior CTC2), then
// SWC2 IR1-3 → a1. USER 2026-06-21: GTE math PC-native (the matrix is read from CR — i.e. content the
// engine/game previously loaded — so this is content-interface; the C MVMVA must be GTE-exact).
uint32_t Math::applyMatlv(uint32_t inPtr, uint32_t out) { Core* c = this->core;
  uint32_t w0 = c->mem_r32(inPtr), w1 = c->mem_r32(inPtr+4);
  int16_t v[3] = { (int16_t)w0, (int16_t)(w0>>16), (int16_t)w1 };
  // rotation matrix R from GTE CONTROL regs CR0..CR4 (same packing as load_mat3 but from ctrl)
  uint32_t cr0=gte_read_ctrl(0),cr1=gte_read_ctrl(1),cr2=gte_read_ctrl(2),cr3=gte_read_ctrl(3),cr4=gte_read_ctrl(4);
  int16_t R[3][3] = {
    {(int16_t)cr0,(int16_t)(cr0>>16),(int16_t)cr1},
    {(int16_t)(cr1>>16),(int16_t)cr2,(int16_t)(cr2>>16)},
    {(int16_t)cr3,(int16_t)(cr3>>16),(int16_t)cr4} };
  int32_t mac[3]; int16_t ir[3];
  for (int i=0;i<3;i++) {
    int64_t t=0;
    t = sign44(t + (int32_t)R[i][0]*v[0]);
    t = sign44(t + (int32_t)R[i][1]*v[1]);
    t = sign44(t + (int32_t)R[i][2]*v[2]);
    mac[i] = (int32_t)(t >> 12); ir[i] = clamp16s(mac[i]);
  }
  c->mem_w32(out,   (uint32_t)(int32_t)ir[0]);
  c->mem_w32(out+4, (uint32_t)(int32_t)ir[1]);
  c->mem_w32(out+8, (uint32_t)(int32_t)ir[2]);
  // GTE leftover: VXY0/VZ0 = the MTC2'd input; IR1-3, MAC1-3 = the MVMVA result.
  gte_write_data(0, w0); gte_write_data(1, w1);
  gte_write_data(9,(uint32_t)(int32_t)ir[0]); gte_write_data(10,(uint32_t)(int32_t)ir[1]); gte_write_data(11,(uint32_t)(int32_t)ir[2]);
  gte_write_data(25,(uint32_t)mac[0]); gte_write_data(26,(uint32_t)mac[1]); gte_write_data(27,(uint32_t)mac[2]);
  return out;
}

