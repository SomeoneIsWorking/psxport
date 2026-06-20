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
#include <stdio.h>
#include <string.h>

void rec_set_override(uint32_t addr, void (*fn)(Core*));
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

static void ov_isqrt(Core* c) { c->r[2] = isqrt16(c->r[4]); }

// Exposed for the native per-object cull (game_tomba2.cpp ov_object_cull): the cull body computes
// dist = isqrt16(dx²+dy²+dz²) exactly as FUN_80077FB0 does, so it must use the SAME bit-exact leaf.
uint32_t eng_isqrt16(uint32_t a0) { return isqrt16(a0); }

// PSXPORT_DEBUG=mathverify — per-call A/B gate: run native, save v0, restore regs, run the recomp
// reference, compare. Pure leaf so only v0 (and clobbered caller-saved regs, which the caller
// reloads) matters; we restore the full reg file before the reference so both see identical input.
static void ov_isqrt_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t a0 = c->r[4];
  ov_isqrt(c);
  uint32_t mine = c->r[2];
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x80077FB0u);
  uint32_t oracle = c->r[2];
  static int nbad = 0, ngood = 0;
  if (mine != oracle) {
    if (nbad++ < 60) fprintf(stderr, "[mathverify] isqrt MISMATCH a0=%08x mine=%08x oracle=%08x\n", a0, mine, oracle);
  } else if ((ngood++ % 5000) == 0) {
    fprintf(stderr, "[mathverify] isqrt match #%d (a0=%08x -> %04x)\n", ngood, a0, oracle);
  }
  c->r[2] = mine;  // keep the native result as the live value
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084080 — table-based fixed-point NORMALIZE (a sqrt-class helper). 9.24% of all interpreter
// instructions on the field — the single biggest resident lever. Its ONLY GTE use is the leading-bit
// counter (MTC2 a0→LZCS reg30; MFC2 v0←LZCR reg31), which is a PURE function of a0, NOT GTE state —
// so this whole routine is a pure leaf and ports native with the GTE replaced by a native CLZ (this is
// the "remove Beetle by porting callers" axis: the caller no longer needs the emulated GTE).
//   v0 = lzcr(a0)                           // count leading bits == bit31, range 1..32
//   if (v0 == 32) return 0                  // a0 all-same-bits (e.g. 0) → 0
//   t2 = v0 & ~1;  t1 = (31 - t2) >> 1;  t3 = t2 - 24
//   t4 = (t3 < 0) ? (a0 >>a (24 - t2)) : (a0 << t3)     // normalize mantissa
//   elem = t4 - 64                          // signed halfword index into LUT @0x800a6310 (rodata)
//   return ( (int16_t)LUT[elem] << t1 ) >> 12          // apply exponent
// LUT is resident MAIN.EXE rodata (read from guest RAM so it can never drift from the reference).
// PSX GTE LZCR semantics: number of leading bits equal to bit31 (leading zeros if a0>=0, leading ones
// if a0<0); 0x00000000 and 0xFFFFFFFF → 32.
static int gte_lzcr(uint32_t x) {
  uint32_t v = (x & 0x80000000u) ? ~x : x;   // negative → count leading ones == leading zeros of ~x
  if (v == 0) return 32;
  return __builtin_clz(v);
}
static void ov_gte_norm(Core* c) {
  uint32_t a0 = c->r[4];
  int v0 = gte_lzcr(a0);
  if (v0 == 32) { c->r[2] = 0; return; }
  int t2 = v0 & ~1;
  int t1 = (31 - t2) >> 1;
  int t3 = t2 - 24;
  int32_t t4 = (t3 < 0) ? ((int32_t)a0 >> ((24 - t2) & 31))   // srav (arithmetic)
                        : (int32_t)(a0 << (t3 & 31));         // sllv (logical)
  int32_t elem = t4 - 64;                                     // signed halfword index
  int16_t lutv = (int16_t)c->mem_r16(0x800a6310u + (uint32_t)(elem * 2));
  uint32_t t5 = (uint32_t)((int32_t)lutv << (t1 & 31));
  c->r[2] = t5 >> 12;
}

// PSXPORT_DEBUG=mathverify — per-call gate vs the recomp reference (which runs the real emulated GTE).
static void ov_gte_norm_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t a0 = c->r[4];
  ov_gte_norm(c);
  uint32_t mine = c->r[2];
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x80084080u);
  uint32_t oracle = c->r[2];
  static int nbad = 0, ngood = 0;
  if (mine != oracle) {
    if (nbad++ < 60) fprintf(stderr, "[mathverify] norm MISMATCH a0=%08x mine=%08x oracle=%08x\n", a0, mine, oracle);
  } else if ((ngood++ % 5000) == 0) {
    fprintf(stderr, "[mathverify] norm match #%d (a0=%08x -> %08x)\n", ngood, a0, oracle);
  }
  c->r[2] = mine;
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
static void ov_mat_mul(Core* c) {
  int16_t R[3][3], M[3][3], P[3][3];
  load_mat3(c, c->r[4], R);
  load_mat3(c, c->r[5], M);
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
  uint32_t a2 = c->r[6];
  c->mem_w32(a2,    (uint16_t)P[0][0] | ((uint32_t)(uint16_t)P[0][1] << 16));
  c->mem_w32(a2+4,  (uint16_t)P[0][2] | ((uint32_t)(uint16_t)P[1][0] << 16));
  c->mem_w32(a2+8,  (uint16_t)P[1][1] | ((uint32_t)(uint16_t)P[1][2] << 16));
  c->mem_w32(a2+12, (uint16_t)P[2][0] | ((uint32_t)(uint16_t)P[2][1] << 16));
  c->mem_w32(a2+16, (uint32_t)(int32_t)P[2][2]);  // SWC2 IR3: sign-extended 32-bit, not a packed halfword
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
  c->r[2] = a2;                                       // returns a2
}

// Comparator: diff a2 (5 words) AND all 32 GTE DATA regs, native vs the recomp reference. Snapshot regs
// + a2 + GTE data/ctrl, run native, capture, restore everything, run oracle, capture, diff. Reveals
// both output correctness and any GTE-state LEAKAGE the native path must replicate.
static void ov_mat_mul_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t a2 = c->r[6];
  uint32_t a2save[5]; for (int i=0;i<5;i++) a2save[i]=c->mem_r32(a2+i*4);
  uint32_t gd0[32], gc0[32]; for (int i=0;i<32;i++){ gd0[i]=gte_read_data(i); gc0[i]=gte_read_ctrl(i); }
  ov_mat_mul(c);
  uint32_t a2m[5]; for (int i=0;i<5;i++) a2m[i]=c->mem_r32(a2+i*4);
  uint32_t gdm[32]; for (int i=0;i<32;i++) gdm[i]=gte_read_data(i);
  // restore pre-state for the oracle
  memcpy(c->r, rsave, sizeof rsave);
  for (int i=0;i<5;i++) c->mem_w32(a2+i*4, a2save[i]);
  for (int i=0;i<32;i++){ gte_write_data(i, gd0[i]); gte_write_ctrl(i, gc0[i]); }
  rec_interp(c, 0x80084110u);
  uint32_t a2o[5]; for (int i=0;i<5;i++) a2o[i]=c->mem_r32(a2+i*4);
  uint32_t gdo[32]; for (int i=0;i<32;i++) gdo[i]=gte_read_data(i);
  static int nbad=0, ngood=0; int bad=0;
  for (int i=0;i<5;i++) if (a2m[i]!=a2o[i]) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] matmul a2+%d mine=%08x oracle=%08x\n", i*4, a2m[i], a2o[i]); }
  for (int i=0;i<32;i++) {
    // DR12-14 = XY_FIFO, DR15 = SXYP, DR31 = LZCR: FIFO/derived regs that MVMVA does NOT write, and the
    // comparator's gte_write_data restore can't round-trip a FIFO (a write pushes it). Excluded — they
    // diverge only as a snapshot artifact, not a real ov_mat_mul leak (both paths leave them untouched).
    if (i>=12 && i<=15) continue; if (i==31) continue;
    if (gdm[i]!=gdo[i]) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] matmul GTE-DR%d mine=%08x oracle=%08x\n", i, gdm[i], gdo[i]); }
  }
  if (bad) nbad++;
  else if ((ngood++ % 5000)==0) fprintf(stderr,"[mathverify] matmul match #%d\n", ngood);
  // keep native result live
  memcpy(c->r, rsave, sizeof rsave); for (int i=0;i<5;i++) c->mem_w32(a2+i*4,a2m[i]);
  for (int i=0;i<32;i++) gte_write_data(i, gdm[i]); c->r[2]=a2;
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
static void ov_rotmat(Core* c) {
  uint32_t a0 = c->r[4], out = c->r[5];
  uint32_t w0 = c->mem_r32(a0);
  int sx,cx,sy,cy,sz,cz;
  rotmat_trig(c, (int16_t)w0,        &sx, &cx);            // vx (low half of +0)
  rotmat_trig(c, (int16_t)(w0>>16),  &sy, &cy);            // vy (high half of +0)
  rotmat_trig(c, (int16_t)c->mem_r16(a0+4), &sz, &cz);     // vz (+4)
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
  c->r[2] = out;
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
}

// Comparator: diff the 5 output words + all 32 GTE data regs, native vs the recomp reference.
static void ov_rotmat_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t out = c->r[5];
  uint32_t osave[5]; for (int i=0;i<5;i++) osave[i]=c->mem_r32(out+i*4);
  uint32_t gd0[32], gc0[32]; for (int i=0;i<32;i++){ gd0[i]=gte_read_data(i); gc0[i]=gte_read_ctrl(i); }
  ov_rotmat(c);
  uint32_t om[5]; for (int i=0;i<5;i++) om[i]=c->mem_r32(out+i*4);
  uint32_t gdm[32]; for (int i=0;i<32;i++) gdm[i]=gte_read_data(i);
  memcpy(c->r, rsave, sizeof rsave);
  for (int i=0;i<5;i++) c->mem_w32(out+i*4, osave[i]);
  for (int i=0;i<32;i++){ gte_write_data(i, gd0[i]); gte_write_ctrl(i, gc0[i]); }
  rec_interp(c, 0x80085480u);
  uint32_t oo[5]; for (int i=0;i<5;i++) oo[i]=c->mem_r32(out+i*4);
  uint32_t gdo[32]; for (int i=0;i<32;i++) gdo[i]=gte_read_data(i);
  static int nbad=0, ngood=0; int bad=0;
  for (int i=0;i<5;i++) if (om[i]!=oo[i]) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] rotmat out+%d mine=%08x oracle=%08x\n", i*4, om[i], oo[i]); }
  for (int i=0;i<32;i++) {
    if (i>=12 && i<=15) continue; if (i==31) continue;   // XY FIFO + LZCR: untouched / can't round-trip
    if (gdm[i]!=gdo[i]) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] rotmat GTE-DR%d mine=%08x oracle=%08x\n", i, gdm[i], gdo[i]); }
  }
  if (bad) nbad++;
  else if ((ngood++ % 5000)==0) fprintf(stderr,"[mathverify] rotmat match #%d\n", ngood);
  memcpy(c->r, rsave, sizeof rsave); for (int i=0;i<5;i++) c->mem_w32(out+i*4,om[i]);
  for (int i=0;i<32;i++) gte_write_data(i, gdm[i]); c->r[2]=out;
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
static void rotpair(Core* c, uint32_t rowA, uint32_t rowB, int posSin) {
  uint32_t m = c->r[5];
  int s, co; rotpair_trig(c, c->r[4], posSin, &s, &co);
  int16_t A[3], B[3];
  for (int i = 0; i < 3; i++) { A[i] = (int16_t)c->mem_r16(m + rowA + i*2); B[i] = (int16_t)c->mem_r16(m + rowB + i*2); }
  for (int i = 0; i < 3; i++) c->mem_w16(m + rowA + i*2, (uint16_t)(int16_t)(((int32_t)co*A[i] - (int32_t)s*B[i]) >> 12));
  for (int i = 0; i < 3; i++) c->mem_w16(m + rowB + i*2, (uint16_t)(int16_t)(((int32_t)s*A[i]  + (int32_t)co*B[i]) >> 12));
  c->r[2] = m;
}
static void ov_rot_z(Core* c) { rotpair(c, 0, 6, +1); }   // FUN_80085050
static void ov_rot_y(Core* c) { rotpair(c, 0, 12, -1); }  // FUN_80084EB0
static void ov_rot_x(Core* c) { rotpair(c, 6, 12, +1); }  // FUN_80084D10
// shared comparator: diff the 6 written halfwords (at rowA{+0,2,4}, rowB{+0,2,4}) vs the recomp ref
static void rotpair_verify(Core* c, uint32_t addr, void(*fn)(Core*), uint32_t rowA, uint32_t rowB, const char* tag) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t m = c->r[5];
  uint32_t off[6] = { rowA, rowA+2, rowA+4, rowB, rowB+2, rowB+4 };
  uint16_t save[6]; for (int i=0;i<6;i++) save[i]=(uint16_t)c->mem_r16(m+off[i]);
  fn(c);
  uint16_t mine[6]; for (int i=0;i<6;i++) mine[i]=(uint16_t)c->mem_r16(m+off[i]);
  memcpy(c->r, rsave, sizeof rsave); for (int i=0;i<6;i++) c->mem_w16(m+off[i], save[i]);
  rec_interp(c, addr);
  uint16_t orc[6]; for (int i=0;i<6;i++) orc[i]=(uint16_t)c->mem_r16(m+off[i]);
  static int nbad=0, ngood=0; int bad=0;
  for (int i=0;i<6;i++) if (mine[i]!=orc[i]) { bad=1; if (nbad<80) fprintf(stderr,"[mathverify] %s hw%d mine=%04x oracle=%04x (ang=%08x)\n", tag, i, mine[i], orc[i], rsave[4]); }
  if (bad) nbad++; else if ((ngood++%5000)==0) fprintf(stderr,"[mathverify] %s match #%d\n", tag, ngood);
  for (int i=0;i<6;i++) c->mem_w16(m+off[i], mine[i]);
  memcpy(c->r, rsave, sizeof rsave); c->r[2]=m;
}
static void ov_rot_z_verify(Core* c){ rotpair_verify(c, 0x80085050u, ov_rot_z, 0, 6,  "rotZ"); }
static void ov_rot_y_verify(Core* c){ rotpair_verify(c, 0x80084EB0u, ov_rot_y, 0, 12, "rotY"); }
static void ov_rot_x_verify(Core* c){ rotpair_verify(c, 0x80084D10u, ov_rot_x, 6, 12, "rotX"); }

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084220 — MVMVA the rotation matrix already in the GTE CR regs by a vector → IR1-3 (libgte
// ApplyMatrixLV-class). a0 = SVECTOR* in (VX,VY @+0 packed, VZ @+4 low half), a1 = VECTOR* out (3
// sign-extended 32-bit words), returned in v0. The body: MTC2 a0 words → VXY0/VZ0, GTE MVMVA (sf=1,
// mx=ROT, v=V0, cv=Null, lm=0) reading the rotation matrix from CR0-4 (loaded by a prior CTC2), then
// SWC2 IR1-3 → a1. USER 2026-06-21: GTE math PC-native (the matrix is read from CR — i.e. content the
// engine/game previously loaded — so this is content-interface; the C MVMVA must be GTE-exact).
static void ov_apply_matlv(Core* c) {
  uint32_t w0 = c->mem_r32(c->r[4]), w1 = c->mem_r32(c->r[4]+4);
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
  uint32_t out = c->r[5];
  c->mem_w32(out,   (uint32_t)(int32_t)ir[0]);
  c->mem_w32(out+4, (uint32_t)(int32_t)ir[1]);
  c->mem_w32(out+8, (uint32_t)(int32_t)ir[2]);
  c->r[2] = out;
  // GTE leftover: VXY0/VZ0 = the MTC2'd input; IR1-3, MAC1-3 = the MVMVA result.
  gte_write_data(0, w0); gte_write_data(1, w1);
  gte_write_data(9,(uint32_t)(int32_t)ir[0]); gte_write_data(10,(uint32_t)(int32_t)ir[1]); gte_write_data(11,(uint32_t)(int32_t)ir[2]);
  gte_write_data(25,(uint32_t)mac[0]); gte_write_data(26,(uint32_t)mac[1]); gte_write_data(27,(uint32_t)mac[2]);
}
static void ov_apply_matlv_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t out = c->r[5];
  uint32_t osave[3]; for (int i=0;i<3;i++) osave[i]=c->mem_r32(out+i*4);
  uint32_t gd0[32], gc0[32]; for (int i=0;i<32;i++){ gd0[i]=gte_read_data(i); gc0[i]=gte_read_ctrl(i); }
  ov_apply_matlv(c);
  uint32_t om[3]; for (int i=0;i<3;i++) om[i]=c->mem_r32(out+i*4);
  uint32_t gdm[32]; for (int i=0;i<32;i++) gdm[i]=gte_read_data(i);
  memcpy(c->r, rsave, sizeof rsave);
  for (int i=0;i<3;i++) c->mem_w32(out+i*4, osave[i]);
  for (int i=0;i<32;i++){ gte_write_data(i, gd0[i]); gte_write_ctrl(i, gc0[i]); }
  rec_interp(c, 0x80084220u);
  uint32_t oo[3]; for (int i=0;i<3;i++) oo[i]=c->mem_r32(out+i*4);
  uint32_t gdo[32]; for (int i=0;i<32;i++) gdo[i]=gte_read_data(i);
  static int nbad=0, ngood=0; int bad=0;
  for (int i=0;i<3;i++) if (om[i]!=oo[i]) { bad=1; if (nbad<80) fprintf(stderr,"[mathverify] applylv out+%d mine=%08x oracle=%08x\n", i*4, om[i], oo[i]); }
  for (int i=0;i<32;i++) { if (i>=12&&i<=15) continue; if (i==31) continue;
    if (gdm[i]!=gdo[i]) { bad=1; if (nbad<80) fprintf(stderr,"[mathverify] applylv GTE-DR%d mine=%08x oracle=%08x\n", i, gdm[i], gdo[i]); } }
  if (bad) nbad++; else if ((ngood++%5000)==0) fprintf(stderr,"[mathverify] applylv match #%d\n", ngood);
  memcpy(c->r, rsave, sizeof rsave); for (int i=0;i<3;i++) c->mem_w32(out+i*4,om[i]);
  for (int i=0;i<32;i++) gte_write_data(i, gdm[i]); c->r[2]=out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80084A80 — libgte RotMatrix variant: build a 3x3 matrix from 3 Euler angles (a0 = SVECTOR*,
// vx@+0/vy@+2/vz@+4), a1 = MATRIX* out (also returned in v0). PURE — SIN/COS LUT @0x800a6490 + native
// 16-bit multiplies (multu/mflo, low 32 bits = signed product), NO GTE ops, so no GTE side-effects to
// mirror. Element layout/composition differs from FUN_80085480 (different rotation order). Transcribed
// from the asm's exact product/shift/store sequence: each product is >>12 (arithmetic), intermediates
// kept full 32-bit (the asm's `sra ,12` results stay register-width before being re-multiplied), and
// only the final `sh` stores truncate to 16 bits. All three angles use posSin=+1 (sin sign = angle
// sign), so rotpair_trig is reused directly. Layout (s_i=sin, c_i=cos of angle i):
//   m00=(c1*c2)>>12                              m20=-s1
//   m01=((s0*s1>>12)*c2>>12)-(s2*c0>>12)         m21=(s0*c1)>>12
//   m02=((s1*c0>>12)*c2>>12)+(s0*s2>>12)         m22=(c0*c1)>>12
//   m10=(s2*c1)>>12
//   m11=((s0*s1>>12)*s2>>12)+(c0*c2>>12)
//   m12=((s1*c0>>12)*s2>>12)-(s0*c2>>12)
static void ov_rot84A80(Core* c) {
  uint32_t a0 = c->r[4], out = c->r[5];
  int s0,c0,s1,c1,s2,c2;
  rotpair_trig(c, (uint32_t)(int32_t)(int16_t)c->mem_r16(a0+0), +1, &s0, &c0);
  rotpair_trig(c, (uint32_t)(int32_t)(int16_t)c->mem_r16(a0+2), +1, &s1, &c1);
  rotpair_trig(c, (uint32_t)(int32_t)(int16_t)c->mem_r16(a0+4), +1, &s2, &c2);
  int32_t s0s1 = ((int32_t)s0*s1) >> 12;   // kept full 32-bit (re-multiplied below)
  int32_t s1c0 = ((int32_t)s1*c0) >> 12;
  int16_t m00 = (int16_t)(((int32_t)c1*c2) >> 12);
  int16_t m01 = (int16_t)(((s0s1*c2) >> 12) - (((int32_t)s2*c0) >> 12));
  int16_t m02 = (int16_t)(((s1c0*c2) >> 12) + (((int32_t)s0*s2) >> 12));
  int16_t m10 = (int16_t)(((int32_t)s2*c1) >> 12);
  int16_t m11 = (int16_t)(((s0s1*s2) >> 12) + (((int32_t)c0*c2) >> 12));
  int16_t m12 = (int16_t)(((s1c0*s2) >> 12) - (((int32_t)s0*c2) >> 12));
  int16_t m20 = (int16_t)(-s1);
  int16_t m21 = (int16_t)(((int32_t)s0*c1) >> 12);
  int16_t m22 = (int16_t)(((int32_t)c0*c1) >> 12);
  c->mem_w16(out+0x0, (uint16_t)m00);
  c->mem_w16(out+0x2, (uint16_t)m01);
  c->mem_w16(out+0x4, (uint16_t)m02);
  c->mem_w16(out+0x6, (uint16_t)m10);
  c->mem_w16(out+0x8, (uint16_t)m11);
  c->mem_w16(out+0xa, (uint16_t)m12);
  c->mem_w16(out+0xc, (uint16_t)m20);
  c->mem_w16(out+0xe, (uint16_t)m21);
  c->mem_w16(out+0x10,(uint16_t)m22);
  c->r[2] = out;
}
static void ov_rot84A80_verify(Core* c) {
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);
  uint32_t out = c->r[5];
  uint32_t osave[5]; for (int i=0;i<5;i++) osave[i]=c->mem_r32(out+i*4);
  ov_rot84A80(c);
  uint32_t om[5]; for (int i=0;i<5;i++) om[i]=c->mem_r32(out+i*4);
  uint32_t vm = c->r[2];
  memcpy(c->r, rsave, sizeof rsave);
  for (int i=0;i<5;i++) c->mem_w32(out+i*4, osave[i]);
  rec_interp(c, 0x80084A80u);
  uint32_t oo[5]; for (int i=0;i<5;i++) oo[i]=c->mem_r32(out+i*4);
  uint32_t vo = c->r[2];
  static int nbad=0, ngood=0; int bad=0;
  for (int i=0;i<5;i++) if (om[i]!=oo[i]) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] rot84A80 out+%d mine=%08x oracle=%08x\n", i*4, om[i], oo[i]); }
  if (vm!=vo) { bad=1; if (nbad<60) fprintf(stderr,"[mathverify] rot84A80 v0 mine=%08x oracle=%08x\n", vm, vo); }
  if (bad) nbad++; else if ((ngood++%5000)==0) fprintf(stderr,"[mathverify] rot84A80 match #%d\n", ngood);
  memcpy(c->r, rsave, sizeof rsave); for (int i=0;i<5;i++) c->mem_w32(out+i*4,om[i]); c->r[2]=vm;
}

void engine_math_register(void) {
  // Verified bit-exact: 65000+ live field calls 0-diff vs the recomp reference (later-186). ov_isqrt is
  // the live path; ov_isqrt_verify is reachable as the per-call gate when the `mathverify` channel is set
  // before override install (same convention as camverify).
  int v = cfg_dbg("mathverify");
  rec_set_override(0x80077FB0u, v ? ov_isqrt_verify    : ov_isqrt);
  rec_set_override(0x80084080u, v ? ov_gte_norm_verify : ov_gte_norm);  // verified 0-diff 15000+ live calls
  rec_set_override(0x80084110u, v ? ov_mat_mul_verify : ov_mat_mul);  // 3x3 matmul; verified 0-diff 115000+ live calls
  rec_set_override(0x80085480u, v ? ov_rotmat_verify : ov_rotmat);  // RotMatrix; verified 0-diff 55000+ live calls
  rec_set_override(0x80085050u, v ? ov_rot_z_verify : ov_rot_z);  // RotMatrixZ-class; verified 0-diff 35000+ live calls
  rec_set_override(0x80084EB0u, v ? ov_rot_y_verify : ov_rot_y);  // RotMatrixY-class (inverted sin); same kernel as rotZ/X (1 live call 0-diff; kernel verified 55k+ on siblings)
  rec_set_override(0x80084D10u, v ? ov_rot_x_verify : ov_rot_x);  // RotMatrixX-class; verified 0-diff 55000+ live calls
  rec_set_override(0x80084220u, v ? ov_apply_matlv_verify : ov_apply_matlv);  // MVMVA matrix(CR)×vec; verified 0-diff 75000+ live calls
  rec_set_override(0x80084A80u, v ? ov_rot84A80_verify : ov_rot84A80);  // RotMatrix variant (pure LUT trig, no GTE); verified 0-diff 5000+ live field calls; 4.4% field hot
}
