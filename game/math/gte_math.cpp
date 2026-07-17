// gte_math.cpp — PC-native reimplementations of the hot libgte-style MATH helpers the engine
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
#include "game_ctx.h"
#include "cfg.h"
#include "gte_math.h"   // class Math — static entry surface + ov_* free-fn decls for internal reuse
#include "game.h"
#include "override_registry.h"  // overrides::install — the one native-override registry
#include <stdio.h>
#include <string.h>

// The recompiler's own PROCESS-GLOBAL call table (generated/shard_disp.c: g_override[]/
// shard_set_override) — the substrate's OWN func_<addr>(c) call sites (e.g. `func_80084110(c);`
// inline in shard_0.c/shard_6.c/…, 55k+/frame for matMul alone) check THIS table first, not the
// registry's rec_dispatch path (which only fires on an explicit rec_dispatch(c, addr) call — a
// small minority of call sites for this cluster). Same dual-wiring pattern as
// ActorReward::registerOverrides — both setters passed to the same `install()` call.
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80084110(Core*);
extern void gen_func_80084220(Core*);
extern void gen_func_80084470(Core*);
extern void gen_func_80085480(Core*);
extern void gen_func_80084D10(Core*);
extern void gen_func_80084EB0(Core*);
extern void gen_func_80085050(Core*);
extern void gen_func_80077FB0(Core*);
extern void gen_func_80078240(Core*);
extern void gen_func_80084080(Core*);
extern void gen_func_80084360(Core*);
extern void gen_func_80084520(Core*);

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

// FUN_80078240 — fast 3-value MAGNITUDE APPROXIMATION (a cheaper substitute for a real sqrt
// magnitude, matching the classic PSX-era "max - max/16 + (sum of the other two)*3/8" estimator).
// Takes abs(a0,a1,a2), finds the MAX of the three (via two pairwise max/carry-the-loser compares —
// transcribed exactly, not a full 3-way sort), and returns max*(15/16) + (other two)*(3/8). Pure
// integer leaf, no memory/GTE. RE'd via Ghidra headless (scratch/decomp/cluster1.c: FUN_80078240):
//   iVar1 = max(a,b); the LOSER of that compare becomes the new `b`
//   iVar2 = max(iVar1,c); the LOSER of that compare becomes the new `c`
//   return (iVar2 - iVar2/16) + (b+c)/4 + (b+c)/8
uint32_t eng_approxDist3(int32_t a, int32_t b, int32_t c) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  if (c < 0) c = -c;
  int32_t iVar1 = a;
  if (a < b) { iVar1 = b; b = a; }
  int32_t iVar2 = iVar1;
  if (iVar1 < c) { iVar2 = c; c = iVar1; }
  uint32_t sum = (uint32_t)(b + c);
  return (uint32_t)(iVar2 - (iVar2 >> 4)) + (sum >> 2) + (sum >> 3);
}

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
uint32_t Math::matMul(uint32_t rPtr, uint32_t mPtr, uint32_t outPtr) {   // FUN_80084110
  Core* c = this->core;
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
uint32_t Math::applyMatrixLV(uint32_t mPtr, uint32_t inPtr, uint32_t out) {   // FUN_80084470
  Core* c = this->core;
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
uint32_t Math::rotmat(uint32_t anglesPtr, uint32_t out) {   // FUN_80085480
  Core* c = this->core;
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
uint32_t Math::applyMatlv(uint32_t inPtr, uint32_t out) {   // FUN_80084220
  Core* c = this->core;
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

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084080 — GTE-LZC fixed-point square root. a0 = value; returns an isqrt-like result via a
// leading-sign-bit normalize + a 16-bit ROM reciprocal-sqrt table lookup (@0x800a6310), scaled back
// by half of the removed exponent. The recomp body drives the GTE LZCS/LZCR unit (data regs 30/31)
// to count leading sign bits; we compute that count in C (perf — no Beetle GTE round-trip) and then
// replicate the two LZC data-reg leftovers so a downstream still-PSX reader sees identical GTE state.
// ORACLE: gen_func_80084080
uint32_t Math::sqrtLzc(uint32_t v) {
  Core* c = this->core;
  uint32_t sign = (v >> 31) & 1u;                 // LZCS sign bit
  uint32_t lzcr = 1;                              // count of leading bits equal to the sign bit (1..32)
  for (int b = 30; b >= 0; --b) { if (((v >> b) & 1u) != sign) break; lzcr++; }
  gte_write_data(30, v);                          // LZCS leftover (= a0)
  gte_write_data(31, lzcr);                       // LZCR leftover (= the count)
  if (lzcr == 32) return 0;                       // v == 0 or v == 0xffffffff → result 0
  uint32_t evenLz  = lzcr & ~1u;                  // round the leading-bit count down to even
  int32_t  normSh  = (int32_t)evenLz - 24;        // shift that lands the mantissa in the table window
  uint32_t mant    = normSh >= 0 ? (v << (uint32_t)(normSh & 31))
                                 : (uint32_t)((int32_t)v >> (uint32_t)((-normSh) & 31));
  uint32_t outSh   = (uint32_t)((31 - (int32_t)evenLz) >> 1);   // half of the removed exponent
  int16_t  tab     = (int16_t)c->mem_r16(0x800a6310u + (((mant - 64u) << 1) & 0xffffffffu));
  uint32_t acc     = (uint32_t)(int32_t)tab << (outSh & 31);
  return acc >> 12;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084360 — in-place 3x3 matrix multiply P = R × M. a0 = R (CR-packed 5 words), a1 = M (same
// packing, ALSO the destination + returned in v0). Identical math to matMul (FUN_80084110) — CTC2
// R into the GTE rotation regs, then one MVMVA (sf=1→>>12, mx=ROT, v=Vj, cv=Null, lm=0) per column
// of M, packing the clamped IR outputs back into M's storage. All reads happen before any store so
// the in-place overwrite is safe. Leaves the same GTE data-reg leftovers a downstream reader consumes.
// ORACLE: gen_func_80084360
uint32_t Math::matLoadLV(uint32_t rPtr, uint32_t vPtr) {
  Core* c = this->core;
  int16_t R[3][3], M[3][3], P[3][3];
  load_mat3(c, rPtr, R);
  load_mat3(c, vPtr, M);
  for (int i = 0; i < 5; i++) gte_write_ctrl(i, c->mem_r32(rPtr + i*4));   // faithful CTC2 of R
  uint32_t vw4 = c->mem_r32(vPtr + 16);      // raw M22 word — the GTE VZ0 leftover keeps its full bits
  int32_t mac1 = 0, mac2 = 0, mac3 = 0;      // last column's pre-clamp MACs (GTE leftover)
  for (int j = 0; j < 3; j++) {              // column j of M = MVMVA input vector Vj
    for (int i = 0; i < 3; i++) {            // matrix row i
      int64_t t = 0;
      t = sign44(t + (int32_t)R[i][0]*M[0][j]);
      t = sign44(t + (int32_t)R[i][1]*M[1][j]);
      t = sign44(t + (int32_t)R[i][2]*M[2][j]);
      int32_t mac = (int32_t)(t >> 12);
      P[i][j] = clamp16s(mac);
      if (j == 2) { if (i==0) mac1=mac; else if (i==1) mac2=mac; else mac3=mac; }
    }
  }
  c->mem_w32(vPtr+0,  (uint16_t)P[0][0] | ((uint32_t)(uint16_t)P[0][1] << 16));
  c->mem_w32(vPtr+4,  (uint16_t)P[0][2] | ((uint32_t)(uint16_t)P[1][0] << 16));
  c->mem_w32(vPtr+8,  (uint16_t)P[1][1] | ((uint32_t)(uint16_t)P[1][2] << 16));
  c->mem_w32(vPtr+12, (uint16_t)P[2][0] | ((uint32_t)(uint16_t)P[2][1] << 16));
  c->mem_w32(vPtr+16, (uint32_t)(int32_t)P[2][2]);   // SWC2 IR3: sign-extended s32
  // GTE leftover (pass 3 = column 2): VXY0/VZ0 = the MTC2'd col-2 vector, IR1-3/MAC1-3 = its result.
  gte_write_data(0, (uint32_t)(uint16_t)M[0][2] | ((uint32_t)(uint16_t)M[1][2] << 16));  // VXY0
  gte_write_data(1, vw4);                                                                // VZ0 (full word)
  gte_write_data(9,  (uint32_t)(int32_t)P[0][2]);    // IR1
  gte_write_data(10, (uint32_t)(int32_t)P[1][2]);    // IR2
  gte_write_data(11, (uint32_t)(int32_t)P[2][2]);    // IR3
  gte_write_data(25, (uint32_t)mac1);                // MAC1
  gte_write_data(26, (uint32_t)mac2);                // MAC2
  gte_write_data(27, (uint32_t)mac3);                // MAC3
  return vPtr;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084520 — per-COLUMN fixed-point scale of the CR-packed 3x3 matrix at r4 by the 3 factors at
// r5. Each element M[i][j] (int16 lo/hi halves across 5 words) is multiplied by column factor f[j]
// and shifted right 12 (12.12 fixed-point). The MIPS `multu` is used but only MFLO (low 32) is read,
// which equals the signed product's low 32 for these operands. Pure integer leaf — no GTE. Returns dstPtr.
static inline int32_t colScale(int32_t half, uint32_t fac) {   // (half*fac) low32, then arithmetic >>12
  return (int32_t)((uint32_t)half * fac) >> 12;
}
// ORACLE: gen_func_80084520
uint32_t Math::matColScale(uint32_t dstPtr, uint32_t facPtr) {
  Core* c = this->core;
  uint32_t f0 = c->mem_r32(facPtr + 0);   // column-0 scale
  uint32_t f1 = c->mem_r32(facPtr + 4);   // column-1 scale
  uint32_t f2 = c->mem_r32(facPtr + 8);   // column-2 scale
  uint32_t w0 = c->mem_r32(dstPtr + 0);   // M00 | M01<<16  → cols 0,1
  c->mem_w32(dstPtr + 0,  ((uint32_t)colScale((int16_t)w0, f0) & 0xffffu)
                          | ((uint32_t)colScale((int32_t)w0 >> 16, f1) << 16));
  uint32_t w1 = c->mem_r32(dstPtr + 4);   // M02 | M10<<16  → cols 2,0
  c->mem_w32(dstPtr + 4,  ((uint32_t)colScale((int16_t)w1, f2) & 0xffffu)
                          | ((uint32_t)colScale((int32_t)w1 >> 16, f0) << 16));
  uint32_t w2 = c->mem_r32(dstPtr + 8);   // M11 | M12<<16  → cols 1,2
  c->mem_w32(dstPtr + 8,  ((uint32_t)colScale((int16_t)w2, f1) & 0xffffu)
                          | ((uint32_t)colScale((int32_t)w2 >> 16, f2) << 16));
  uint32_t w3 = c->mem_r32(dstPtr + 12);  // M20 | M21<<16  → cols 0,1
  c->mem_w32(dstPtr + 12, ((uint32_t)colScale((int16_t)w3, f0) & 0xffffu)
                          | ((uint32_t)colScale((int32_t)w3 >> 16, f1) << 16));
  uint32_t w4 = c->mem_r32(dstPtr + 16);  // M22 (lo only)  → col 2, stored as sign-extended s32
  c->mem_w32(dstPtr + 16, (uint32_t)colScale((int16_t)w4, f2));
  return dstPtr;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Wiring — BOTH tables, same dual pattern as ActorReward::registerOverrides (game/object/
// actor_sm_reward.cpp): each `install()` call below registers (1) the registry's own rec_dispatch
// entry, for callers reaching these via an explicit rec_dispatch(c, addr) (Engine::
// objMatrixCompose etc — traced by the `dispatch` debug channel), and (2) shard_set_override for
// the recompiler's OWN g_override[] table, which is what the substrate's inline `func_<addr>(c)`
// call sites actually consult (the 55k+/frame majority for matMul). g_override[] is a single
// PROCESS-GLOBAL table shared by every Core — including SBS's
// two separately-constructed cores — so the gate must live IN the installed function itself:
// core B (psx_fallback, the pure substrate reference) must keep running the exact recompiled
// gen_func_* body, or SBS would just be comparing this port against itself (a fake 0-diff).
static void eov_matMul(Core* c)        { c->r[2] = mathOf(c).matMul(c->r[4], c->r[5], c->r[6]); }
static void eov_applyMatlv(Core* c)    { c->r[2] = mathOf(c).applyMatlv(c->r[4], c->r[5]); }
static void eov_applyMatrixLV(Core* c) { c->r[2] = mathOf(c).applyMatrixLV(c->r[4], c->r[5], c->r[6]); }
static void eov_rotmat(Core* c)        { c->r[2] = mathOf(c).rotmat(c->r[4], c->r[5]); }
static void eov_rotX(Core* c)          { c->r[2] = mathOf(c).rotX((int16_t)c->r[4], c->r[5]); }
static void eov_rotY(Core* c)          { c->r[2] = mathOf(c).rotY((int16_t)c->r[4], c->r[5]); }
static void eov_rotZ(Core* c)          { c->r[2] = mathOf(c).rotZ((int16_t)c->r[4], c->r[5]); }

static void eov_isqrt16(Core* c)       { c->r[2] = eng_isqrt16(c->r[4]); }
static void eov_approxDist3(Core* c)   { c->r[2] = eng_approxDist3((int32_t)c->r[4], (int32_t)c->r[5], (int32_t)c->r[6]); }

static void eov_sqrtLzc(Core* c)       { c->r[2] = mathOf(c).sqrtLzc(c->r[4]); }
static void eov_matLoadLV(Core* c)     { c->r[2] = mathOf(c).matLoadLV(c->r[4], c->r[5]); }
static void eov_matColScale(Core* c)   { c->r[2] = mathOf(c).matColScale(c->r[4], c->r[5]); }

void Math::registerOverrides() {
  using overrides::install;
  install(0x80084110u, "Math::matMul",        eov_matMul,        gen_func_80084110, shard_set_override);
  install(0x80084220u, "Math::applyMatlv",    eov_applyMatlv,    gen_func_80084220, shard_set_override);
  install(0x80084470u, "Math::applyMatrixLV", eov_applyMatrixLV, gen_func_80084470, shard_set_override);
  install(0x80085480u, "Math::rotmat",        eov_rotmat,        gen_func_80085480, shard_set_override);
  install(0x80084D10u, "Math::rotX",          eov_rotX,          gen_func_80084D10, shard_set_override);
  install(0x80084EB0u, "Math::rotY",          eov_rotY,          gen_func_80084EB0, shard_set_override);
  install(0x80085050u, "Math::rotZ",          eov_rotZ,          gen_func_80085050, shard_set_override);
  install(0x80077FB0u, "Math::isqrt16",       eov_isqrt16,       gen_func_80077FB0, shard_set_override);
  install(0x80078240u, "Math::approxDist3",   eov_approxDist3,   gen_func_80078240, shard_set_override);
  install(0x80084080u, "Math::sqrtLzc",       eov_sqrtLzc,       gen_func_80084080, shard_set_override);
  install(0x80084360u, "Math::matLoadLV",     eov_matLoadLV,     gen_func_80084360, shard_set_override);
  install(0x80084520u, "Math::matColScale",   eov_matColScale,   gen_func_80084520, shard_set_override);
}

