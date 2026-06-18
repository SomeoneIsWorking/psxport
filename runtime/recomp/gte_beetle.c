// GTE (COP2) coprocessor, lifted from the Beetle GPL-2 fork (mednafen/psx/gte.c, compiled
// as-is). All the game's geometry — RTPS/RTPT projection, NCLIP, matrix ops, color/depth —
// flows through here; our previous stub was a no-op, so any 3D was inert. This adapts the
// recomp GTE interface (r3000.h) to Beetle's GTE_* API and provides faithful-first stubs for
// the few externs gte.c references (PGXP off, widescreen off, savestate unused) so the math
// matches the oracle exactly. The widescreen GTE-scale hack stays OFF here (fps60 tier later).
#include "r3000.h"
#include "cfg.h"
#include <stdint.h>
#include <stdbool.h>

// Beetle GTE API (mednafen/psx/gte.h), declared locally to avoid pulling Beetle headers.
void     GTE_Init(void);
void     GTE_Power(void);
int32_t  GTE_Instruction(uint32_t instr);
void     GTE_WriteCR(unsigned which, uint32_t value);
void     GTE_WriteDR(unsigned which, uint32_t value);
uint32_t GTE_ReadCR(unsigned which);
uint32_t GTE_ReadDR(unsigned which);

// Externs gte.c references — faithful-first values.
bool     psx_gte_overclock = false;
uint8_t  widescreen_hack = 0;                   // GTE widescreen-scale hack OFF (faithful)
uint8_t  widescreen_hack_aspect_ratio_setting = 0;
uint32_t gMode = 0;                             // PGXP mode 0 = off

// --- PGXP subpixel cache (vertex-smoothing / PSX-wobble fix) -------------------------------------
// PSX projected vertices to INTEGER screen coords, so 3D geometry jitters ("wobbles") as the
// camera/object moves — the classic PS1 look. Beetle's GTE already computes the SUBPIXEL-precise
// projected x/y/z (gte.c TransformXY: precise_x/precise_y/precise_z) and hands them to this hook,
// keyed `v` = the packed integer SXY the game then copies verbatim into its GP0 vertex packets.
// We cache (precise_x,y,z) keyed by that packed int; the GPU tee (gpu_native.c) looks the precise
// coords back up by each vertex's integer (sx,sy) and rasterizes with FLOAT positions instead of
// the integer-snapped ones. This is value-keyed "PGXP-lite": on a key collision we simply fall
// back to the integer coords (a miss), so a wrong match can only ever cost smoothing, not correctness.
// The cache is reset each presented frame (pgxp_frame_reset) so a stale precise value from a prior
// frame can't be re-applied to a freshly-placed integer vertex (kills cross-frame wobble artifacts).
#define PGXP_BITS 15
#define PGXP_SIZE (1 << PGXP_BITS)
#define PGXP_MASK (PGXP_SIZE - 1)
// vx/vy/vz = the GTE's view-space vertex (IR1/IR2/IR3 = rotation*vertex + translation), captured at
// projection time. The renderer reconstructs per-face normals from these (cross product) for native
// lighting — Tomba2 has no GTE lighting of its own (later-96), so this is the only normal source.
typedef struct { uint32_t key; float x, y, z; float vx, vy, vz; uint8_t valid; } PgxpEnt;
static PgxpEnt s_pgxp[PGXP_SIZE];

static inline uint32_t pgxp_key(int sx, int sy) {
  // canonical 22-bit key = 11-bit signed sx | sy, matching the GP0 vertex word's usable range
  return ((uint32_t)(sy & 0x7FF) << 11) | (uint32_t)(sx & 0x7FF);
}
static inline uint32_t pgxp_slot(uint32_t key) {
  uint32_t h = key * 2654435761u;        // Knuth multiplicative hash
  return (h >> (32 - PGXP_BITS)) & PGXP_MASK;
}

void PGXP_pushSXYZ2f(float x, float y, float z, uint32_t v) {
  int sx = (int16_t)(v & 0xFFFF), sy = (int16_t)(v >> 16);
  uint32_t key = pgxp_key(sx, sy);
  PgxpEnt* e = &s_pgxp[pgxp_slot(key)];
  e->key = key; e->x = x; e->y = y; e->z = z;
  // IR1/IR2/IR3 (data regs 9/10/11) still hold this vertex in view space at push time (TransformXY
  // doesn't touch them) — capture for renderer-side normal reconstruction.
  e->vx = (float)(int16_t)GTE_ReadDR(9);
  e->vy = (float)(int16_t)GTE_ReadDR(10);
  e->vz = (float)(int16_t)GTE_ReadDR(11);
  e->valid = 1;
}

// Renderer hook: fetch the subpixel coords for an integer vertex (sx,sy). Returns 1 on a cache hit.
int pgxp_lookup(int sx, int sy, float* px, float* py, float* pz) {
  uint32_t key = pgxp_key(sx, sy);
  PgxpEnt* e = &s_pgxp[pgxp_slot(key)];
  if (e->valid && e->key == key) {
    if (px) *px = e->x; if (py) *py = e->y; if (pz) *pz = e->z;
    return 1;
  }
  return 0;
}

// Renderer hook: fetch the view-space vertex (for native lighting). Returns 1 on a cache hit.
int pgxp_lookup_view(int sx, int sy, float* vx, float* vy, float* vz) {
  uint32_t key = pgxp_key(sx, sy);
  PgxpEnt* e = &s_pgxp[pgxp_slot(key)];
  if (e->valid && e->key == key) {
    if (vx) *vx = e->vx; if (vy) *vy = e->vy; if (vz) *vz = e->vz;
    return 1;
  }
  return 0;
}

void pgxp_frame_reset(void) {
  for (uint32_t i = 0; i < PGXP_SIZE; i++) s_pgxp[i].valid = 0;
}

int   PGXP_NCLIP_valid(uint32_t a, uint32_t b, uint32_t c)   { (void)a;(void)b;(void)c; return 0; }
float PGXP_NCLIP(void)                                        { return 0.0f; }
int   MDFNSS_StateAction(void* st, int load, int data_only, void* sf, const char* name) {
  (void)st; (void)load; (void)data_only; (void)sf; (void)name; return 1;  // savestate unused
}

// Recomp GTE interface (r3000.h) -> Beetle GTE. mfc2/cfc2/mtc2/ctc2/lwc2/swc2 map to the
// data/control register ports; the COP2 ops map to GTE_Instruction.
uint32_t gte_read_data (uint32_t reg)            { return GTE_ReadDR(reg); }
void     gte_write_data(uint32_t reg, uint32_t v){ GTE_WriteDR(reg, v); }
uint32_t gte_read_ctrl (uint32_t reg)            { return GTE_ReadCR(reg); }
void     gte_write_ctrl(uint32_t reg, uint32_t v){ GTE_WriteCR(reg, v); }
// --- Widescreen RE tool (journal later-55): histogram the projected screen-X the GTE produces, to
// learn whether world geometry is projected beyond the 320 display window. Result: ~14% of verts
// land outside [0,320) (near bands [-64,0)≈3%, [320,384)≈11%) — the GTE DOES project wide geometry,
// but VRAM packing (textures abut the FB) blocks drawing it. Gated on PSXPORT_WS_SXHIST; accumulates
// and gpu_present dumps it every 500 frames. Reads SXY-FIFO (DR12/13/14) after RTPS/RTPT.
#include <stdio.h>
#include <stdlib.h>
static long s_sx_hist[16];   // buckets of 64px from -256..+704 (display is [0,320))
static long s_sx_n, s_sx_oob_lo, s_sx_oob_hi;
static int  s_sxhist_on = -1;
static void ws_sx_record(void) {
  if (s_sxhist_on < 0) s_sxhist_on = cfg_dbg("sxhist") ? 1 : 0;
  if (!s_sxhist_on) return;
  for (unsigned r = 12; r <= 14; r++) {
    int16_t sx = (int16_t)(GTE_ReadDR(r) & 0xFFFF);
    s_sx_n++;
    if (sx < 0)   s_sx_oob_lo++;
    if (sx >= 320) s_sx_oob_hi++;
    int b = (sx + 256) / 64; if (b < 0) b = 0; if (b > 15) b = 15;
    s_sx_hist[b]++;
  }
}
void ws_sx_dump(const char* tag) {
  if (s_sxhist_on != 1 || s_sx_n == 0) return;
  fprintf(stderr, "[ws_sxhist] %s n=%ld  below0=%ld(%.1f%%)  atOrAbove320=%ld(%.1f%%)\n",
          tag, s_sx_n, s_sx_oob_lo, 100.0*s_sx_oob_lo/s_sx_n, s_sx_oob_hi, 100.0*s_sx_oob_hi/s_sx_n);
  for (int b = 0; b < 16; b++)
    fprintf(stderr, "  [%5d..%5d) %ld\n", b*64-256, b*64-256+64, s_sx_hist[b]);
  for (int b = 0; b < 16; b++) s_sx_hist[b] = 0;
  s_sx_n = s_sx_oob_lo = s_sx_oob_hi = 0;
}

extern int g_fps60_on;                            // fps60.c: gates the capture taps
void fps60_rtp(uint32_t op);                      // fps60.c: fold fingerprint + tag SXY->object

// --- GTE/lighting RE probe (PSXPORT_GTEPROBE=N) -------------------------------------------------
// Logs which GTE commands ACTUALLY execute (static call-site counts in generated/ aren't run-weighted)
// and snapshots the lighting/fog control registers, to pin down Tomba2's shading model. Finding so far:
// NO NC*/CC/CDP ops => no GTE dynamic per-vertex lighting; colors are baked + DPCS/DPCT depth-cue FOG.
static long s_gte_hist[64];
static int  s_gteprobe = -1, s_gteprobe_done = 0;
static const char* gte_name(unsigned fn) {
  switch (fn) {
    case 0x01:return"RTPS"; case 0x06:return"NCLIP"; case 0x0C:return"OP"; case 0x10:return"DPCS";
    case 0x11:return"INTPL"; case 0x12:return"MVMVA"; case 0x13:return"NCDS"; case 0x14:return"CDP";
    case 0x16:return"NCDT"; case 0x1B:return"NCCS"; case 0x1C:return"CC"; case 0x1E:return"NCS";
    case 0x20:return"NCT"; case 0x28:return"SQR"; case 0x29:return"DCPL"; case 0x2A:return"DPCT";
    case 0x2D:return"AVSZ3"; case 0x2E:return"AVSZ4"; case 0x30:return"RTPT"; case 0x3D:return"GPF";
    case 0x3E:return"GPL"; case 0x3F:return"NCCT"; default:return"?";
  }
}
void gte_probe_dump(const char* tag) {
  if (s_gteprobe <= 0) return;
  fprintf(stderr, "[gteprobe %s] executed GTE ops:\n", tag);
  for (unsigned fn = 0; fn < 64; fn++) if (s_gte_hist[fn])
    fprintf(stderr, "    %-6s(0x%02X) = %ld\n", gte_name(fn), fn, s_gte_hist[fn]);
  // lighting/fog control-register snapshot (cop2 CR numbering)
  fprintf(stderr, "  LightMatrix(LLM cr8-12): %08X %08X %08X %08X %08X\n",
          GTE_ReadCR(8),GTE_ReadCR(9),GTE_ReadCR(10),GTE_ReadCR(11),GTE_ReadCR(12));
  fprintf(stderr, "  BackColor(RBK/GBK/BBK cr13-15): %d %d %d\n",
          (int32_t)GTE_ReadCR(13),(int32_t)GTE_ReadCR(14),(int32_t)GTE_ReadCR(15));
  fprintf(stderr, "  LightColorMtx(LCM cr16-20): %08X %08X %08X %08X %08X\n",
          GTE_ReadCR(16),GTE_ReadCR(17),GTE_ReadCR(18),GTE_ReadCR(19),GTE_ReadCR(20));
  fprintf(stderr, "  FarColor(RFC/GFC/BFC cr21-23): %d %d %d   [fog target]\n",
          (int32_t)GTE_ReadCR(21),(int32_t)GTE_ReadCR(22),(int32_t)GTE_ReadCR(23));
  fprintf(stderr, "  DepthCue DQA(cr27)=%d DQB(cr28)=%d   [IR0 = DQB + DQA*h/sz -> fog factor]\n",
          (int16_t)GTE_ReadCR(27),(int32_t)GTE_ReadCR(28));
}
// --- Native projection (Phase 1): reimplement RTPS/RTPT in native C, oracle-gate it ----------------
// The engine projects every vertex through the GTE (RTPS/RTPT) and then copies the integer screen
// coords into GP0 packets. To OWN the projection (plan atomic-riding-sparkle, Phase 1) we recompute it
// here in native C from the loaded matrix + input vertex, producing both the integer outputs (to prove
// 0-diff vs Beetle's GTE — i.e. our math IS the engine's projection, so gameplay stays untouched) and
// the FLOAT view-space pos / screen / depth that the renderer needs and that the GP0 packet drops.
// This replaces the value-keyed PGXP-lite cache as the source of subpixel/depth (which gets attached at
// submission in a later step); here it is a read-only verifier. The integer half exactly mirrors
// mednafen/psx/gte.c (MultiplyMatrixByVector_PT + Divide/UNR + TransformXY), so a 0-diff result proves
// the reimplementation is faithful. Gated on PSXPORT_PROJPROBE (read-only, no effect on output).
#include <compat/intrinsics.h>   // compat_clz_u16, matching Beetle's Divide() shift-bias

typedef struct { int ir1, ir2, ir3, sz, sx, sy; float px, py, pz, vx, vy, vz; } ProjVtx;

static uint8_t  s_divtab[0x101];
static int      s_divtab_init = 0;
static uint16_t s_proj_H = 0;          // last projection-plane H (CR26); used by proj_pz_to_ord
static float    s_proj_cx = 160.0f, s_proj_cy = 120.0f;   // screen projection center (OFX/OFY >> 16); for PSXPORT_LIGHT normal reconstruction

// Phase 2: map a native view-space depth `pz` (= max(H/2, sz), so pz in [H/2, 65535]) into a
// normalized [0,1] depth value for the renderer's D32 buffer. The value is AFFINE in 1/pz, so it
// interpolates linearly in screen space (gl_Position.w==1 in the VK vertex shader -> no perspective
// divide on z); nearer (smaller pz) -> larger value, matching the renderer's GREATER_OR_EQUAL compare
// + 0.0 clear (nearer wins). Not OT submission order: this is true per-vertex perspective depth.
float proj_pz_to_ord(float pz) {
  float nearp = (s_proj_H ? (float)s_proj_H * 0.5f : 1.0f);  // near plane = H/2 (the proj pz clamp floor)
  if (nearp < 1.0f) nearp = 1.0f;
  if (pz < nearp) pz = nearp;
  float inv_near = 1.0f / nearp, inv_far = 1.0f / 65535.0f;
  float ord = (1.0f / pz - inv_far) / (inv_near - inv_far);
  return ord < 0.0f ? 0.0f : (ord > 1.0f ? 1.0f : ord);
}
static void proj_divtab_init(void) {                       // mirrors GTE_Init's DivTable build
  for (uint32_t d = 0x8000; d < 0x10000; d += 0x80) {
    uint32_t xa = 512;
    for (int i = 1; i < 5; i++) xa = (xa * (1024 * 512 - ((d >> 7) * xa))) >> 18;
    s_divtab[(d >> 7) & 0xFF] = ((xa + 1) >> 1) - 0x101;
  }
  s_divtab[0x100] = s_divtab[0xFF];
  s_divtab_init = 1;
}
static int32_t proj_recip(uint16_t divisor) {              // mirrors CalcRecip
  int32_t x   = 0x101 + s_divtab[(((divisor & 0x7FFF) + 0x40) >> 7)];
  int32_t t   = (((int32_t)divisor * -x) + 0x80) >> 8;
  return ((x * (131072 + t)) + 0x80) >> 8;
}
static uint32_t proj_divide(uint32_t dividend, uint32_t divisor) {  // mirrors Divide (UNR)
  if ((divisor * 2) > dividend) {
    unsigned s = compat_clz_u16((uint16_t)divisor);
    dividend <<= s; divisor <<= s;
    uint32_t r = (uint32_t)(((uint64_t)dividend * proj_recip((uint16_t)(divisor | 0x8000)) + 32768) >> 16);
    return r > 0x1FFFF ? 0x1FFFF : r;
  }
  return 0x1FFFF;                                          // Z <= H/2 -> clip
}
static inline int64_t proj_a_mv(int64_t v) {               // A_MV: 44-bit signed wrap (no flags here)
  return (int64_t)((uint64_t)v << 20) >> 20;
}
static inline int32_t proj_clampi(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Recompute one vertex's projection from a snapshot of the GTE control/data regs (read post-instruction;
// neither matrix CR0-7 nor the input V regs DR0-5 are touched by RTP, so reading after is exact).
static void proj_native_vertex(unsigned vidx, uint32_t insn, ProjVtx* out) {
  const uint32_t sf = (insn & (1 << 19)) ? 12 : 0;
  const int      lm = (insn >> 10) & 1;
  // rotation matrix RT (CR0-4) and translation TR (CR5-7), exactly as MultiplyMatrixByVector_PT packs.
  const uint32_t c0 = gte_read_ctrl(0), c1 = gte_read_ctrl(1), c2 = gte_read_ctrl(2),
                 c3 = gte_read_ctrl(3), c4 = gte_read_ctrl(4);
  const int32_t RT[3][3] = {
    { (int16_t)c0,        (int16_t)(c0 >> 16), (int16_t)c1 },
    { (int16_t)(c1 >> 16), (int16_t)c2,        (int16_t)(c2 >> 16) },
    { (int16_t)c3,        (int16_t)(c3 >> 16), (int16_t)c4 } };
  const int64_t TR[3] = { (int32_t)gte_read_ctrl(5), (int32_t)gte_read_ctrl(6), (int32_t)gte_read_ctrl(7) };
  // input vertex V[vidx] from data regs (Vectors macro): DR[2v]=lo:VX hi:VY, DR[2v+1] lo:VZ
  const uint32_t dlo = gte_read_data(2 * vidx), dhi = gte_read_data(2 * vidx + 1);
  const int32_t  V[3] = { (int16_t)dlo, (int16_t)(dlo >> 16), (int16_t)dhi };

  int32_t mac[3]; int64_t tmp2_unshifted = 0;
  for (int i = 0; i < 3; i++) {
    int64_t t = TR[i] << 12;
    t = proj_a_mv(t + (int64_t)RT[i][0] * V[0]);
    t = proj_a_mv(t + (int64_t)RT[i][1] * V[1]);
    t = proj_a_mv(t + (int64_t)RT[i][2] * V[2]);
    if (i == 2) tmp2_unshifted = t;                        // for IR3 PTZ / SZ (uses tmp>>12)
    mac[i] = (int32_t)(t >> sf);
  }
  const int32_t lo_b = -32768 + (lm << 15);
  out->ir1 = proj_clampi(mac[0], lo_b, 32767);             // Lm_B
  out->ir2 = proj_clampi(mac[1], lo_b, 32767);
  out->ir3 = proj_clampi(mac[2], lo_b, 32767);             // Lm_B_PTZ (clamp identical; ftv only flags)
  out->sz  = proj_clampi((int32_t)(tmp2_unshifted >> 12), 0, 65535);  // Lm_D unchained
  out->vx  = (float)out->ir1; out->vy = (float)out->ir2; out->vz = (float)out->ir3;  // view-space (IR)

  const int32_t OFX = (int32_t)gte_read_ctrl(24), OFY = (int32_t)gte_read_ctrl(25);
  const uint16_t H = (uint16_t)gte_read_ctrl(26);
  s_proj_H = H;                                             // remember the projection plane for depth-normalize
  int64_t h_div_sz = proj_divide(H, (uint32_t)out->sz);    // integer UNR division (matches gameplay)
  out->sx = proj_clampi((int32_t)(((int64_t)OFX + out->ir1 * h_div_sz) >> 16), -1024, 1023);  // Lm_G
  out->sy = proj_clampi((int32_t)(((int64_t)OFY + out->ir2 * h_div_sz) >> 16), -1024, 1023);
  // float (subpixel) screen + depth — the data we keep that the integer GP0 packet throws away.
  float pz = (float)H * 0.5f; if ((float)out->sz > pz) pz = (float)out->sz;
  float ph = (float)H / pz;
  float fofx = (float)OFX / 65536.0f, fofy = (float)OFY / 65536.0f;
  s_proj_cx = fofx; s_proj_cy = fofy;                      // remember the projection center for PSXPORT_LIGHT
  out->px = fofx + (float)out->ir1 * ph;
  out->py = fofy + (float)out->ir2 * ph;
  if (out->px < -1024.f) out->px = -1024.f; if (out->px > 1023.f) out->px = 1023.f;
  if (out->py < -1024.f) out->py = -1024.f; if (out->py > 1023.f) out->py = 1023.f;
  out->pz = pz;
}

// Verification accumulators (PSXPORT_PROJPROBE).
static int  s_projprobe = -1;
static long s_pp_verts, s_pp_bad_ir, s_pp_bad_sz, s_pp_bad_sx, s_pp_bad_sy;
static int  s_pp_maxd_ir, s_pp_maxd_sz, s_pp_maxd_sx, s_pp_maxd_sy;
static long s_pp_subpix_le1;   // how often round(precise) is within 1px of the integer screen coord
static void proj_probe_one(unsigned vidx, uint32_t insn, int b_ir1, int b_ir2, int b_ir3, int b_sz,
                           int b_sx, int b_sy) {
  ProjVtx p; proj_native_vertex(vidx, insn, &p);
  s_pp_verts++;
  int dir = 0;
  if (p.ir1 != b_ir1) dir = abs(p.ir1 - b_ir1) > dir ? abs(p.ir1 - b_ir1) : dir;
  if (p.ir2 != b_ir2) { int d = abs(p.ir2 - b_ir2); if (d > dir) dir = d; }
  if (p.ir3 != b_ir3) { int d = abs(p.ir3 - b_ir3); if (d > dir) dir = d; }
  if (dir) { s_pp_bad_ir++; if (dir > s_pp_maxd_ir) s_pp_maxd_ir = dir; }
  int dsz = abs(p.sz - b_sz); if (dsz) { s_pp_bad_sz++; if (dsz > s_pp_maxd_sz) s_pp_maxd_sz = dsz; }
  int dsx = abs(p.sx - b_sx); if (dsx) { s_pp_bad_sx++; if (dsx > s_pp_maxd_sx) s_pp_maxd_sx = dsx; }
  int dsy = abs(p.sy - b_sy); if (dsy) { s_pp_bad_sy++; if (dsy > s_pp_maxd_sy) s_pp_maxd_sy = dsy; }
  // sanity: our subpixel float, rounded, should land within 1px of the integer projection
  int rx = (int)(p.px < 0 ? p.px - 0.5f : p.px + 0.5f), ry = (int)(p.py < 0 ? p.py - 0.5f : p.py + 0.5f);
  if (abs(rx - b_sx) <= 1 && abs(ry - b_sy) <= 1) s_pp_subpix_le1++;
}
void proj_probe_dump(const char* tag) {
  if (s_projprobe <= 0 || s_pp_verts == 0) return;
  fprintf(stderr, "[projprobe %s] verts=%ld  IR diff=%ld(max%d) SZ diff=%ld(max%d) "
          "SX diff=%ld(max%d) SY diff=%ld(max%d)  subpix<=1px=%ld(%.2f%%)\n",
          tag, s_pp_verts, s_pp_bad_ir, s_pp_maxd_ir, s_pp_bad_sz, s_pp_maxd_sz,
          s_pp_bad_sx, s_pp_maxd_sx, s_pp_bad_sy, s_pp_maxd_sy,
          s_pp_subpix_le1, 100.0 * s_pp_subpix_le1 / s_pp_verts);
  s_pp_verts = s_pp_bad_ir = s_pp_bad_sz = s_pp_bad_sx = s_pp_bad_sy = s_pp_subpix_le1 = 0;
  s_pp_maxd_ir = s_pp_maxd_sz = s_pp_maxd_sx = s_pp_maxd_sy = 0;
}

// PSXPORT_RTPCALLER: histogram the return address (RA=r[31]) at each RTPS/RTPT — pins the submit/handler
// call sites that build POLY packets from the projection, the targets for attach-at-submission.
static struct { uint32_t ra; long n; } s_rtpcaller[64];
static int s_rtpcaller_on = -1;
static void rtpcaller_record(uint32_t ra) {
  if (s_rtpcaller_on < 0) s_rtpcaller_on = cfg_dbg("rtpcaller") ? 1 : 0;
  if (!s_rtpcaller_on) return;
  for (int i = 0; i < 64; i++) { if (s_rtpcaller[i].n == 0) { s_rtpcaller[i].ra = ra; s_rtpcaller[i].n = 1; return; }
                                 if (s_rtpcaller[i].ra == ra) { s_rtpcaller[i].n++; return; } }
}
void rtpcaller_dump(const char* tag) {
  if (s_rtpcaller_on != 1) return;
  uint32_t mem_r32(uint32_t);
  fprintf(stderr, "[rtpcaller %s] RA histogram (caller return site -> jal target = projection fn):\n", tag);
  for (int i = 0; i < 64 && s_rtpcaller[i].n; i++) {
    // the jal that set RA is at RA-8 (RA = jal_addr + 8, MIPS branch-delay). Decode its target.
    uint32_t jal = mem_r32(s_rtpcaller[i].ra - 8);
    uint32_t tgt = ((jal >> 26) == 3) ? (((s_rtpcaller[i].ra - 8) & 0xF0000000u) | ((jal & 0x03FFFFFFu) << 2))
                                      : 0;  // 0 = not a jal (jalr / inlined)
    fprintf(stderr, "    RA=0x%08X  %8ld   jal[%08X]->fn=0x%08X\n",
            s_rtpcaller[i].ra, s_rtpcaller[i].n, jal, tgt);
  }
}
void rtpcaller_reset(void) { for (int i = 0; i < 64; i++) { s_rtpcaller[i].ra = 0; s_rtpcaller[i].n = 0; } }

// --- Native per-vertex depth, recorded BY the owned submit path (engine/engine_submit.c) ------------
// The engine that builds each GPU packet knows the real view-space Z of every vertex it projects, so it
// records that depth keyed by the vertex word's guest ADDRESS in the packet (pkt+8/+20/+32[/+44] for a
// POLY_GT3/GT4). The renderer (gpu_native gp0_exec) looks it up by each packet vertex word's read
// address (s_fifo_addr). Exact and deterministic by construction. This REPLACED the value-keyed "attach"
// ring (capture-at-gte_op + value-match-at-store), which could only CORRELATE projected SXY back to a
// depth and was unreliable (same-pixel verts ambiguous; whole-frame staleness) — a measurement hack.
#define PP_MAX  65536
#define PP_HASH 16384
typedef struct { uint32_t addr; float pz; int next; } PpEnt;
static PpEnt s_pp[PP_MAX];
static int   s_pp_head[PP_HASH];
static int   s_pp_n = 0, s_pp_inited = 0, s_pp_overflow = 0;
static inline uint32_t pp_hash(uint32_t addr) { return ((addr >> 2) * 2654435761u) >> 18 & (PP_HASH - 1); }
void projprim_reset(void) {           // per-frame: drop last frame's depths so none are read stale
  s_pp_n = 0; s_pp_overflow = 0;
  for (int i = 0; i < PP_HASH; i++) s_pp_head[i] = -1;
  s_pp_inited = 1;
}
extern long g_pp_set;
void projprim_set_pz(uint32_t addr, float pz) {   // engine_submit records a vertex's view-Z at its addr
  g_pp_set++;
  if (!s_pp_inited) projprim_reset();
  addr &= 0x1FFFFC;
  uint32_t h = pp_hash(addr);
  for (int i = s_pp_head[h]; i >= 0; i = s_pp[i].next) if (s_pp[i].addr == addr) { s_pp[i].pz = pz; return; }
  if (s_pp_n >= PP_MAX) { s_pp_overflow = 1; return; }
  PpEnt* e = &s_pp[s_pp_n];
  e->addr = addr; e->pz = pz; e->next = s_pp_head[h]; s_pp_head[h] = s_pp_n++;
}
long g_pp_set, g_pp_hit, g_pp_miss;   // ndepth diag: depth records made / lookups hit / lookups missed
int projprim_lookup_pz(uint32_t addr, float* pz) {   // renderer: depth for the packet vertex word at addr
  if (!s_pp_inited) return 0;
  addr &= 0x1FFFFC;
  for (int i = s_pp_head[pp_hash(addr)]; i >= 0; i = s_pp[i].next) if (s_pp[i].addr == addr) {
    if (pz) *pz = s_pp[i].pz; g_pp_hit++; return 1; }
  g_pp_miss++;
  return 0;
}
int  projprim_overflowed(void) { return s_pp_overflow; }
int  projprim_count(void)      { return s_pp_n; }

// The native-depth path is active (NATIVE_DEPTH renderer or the SBS A/B view) — gates the engine's depth
// recording + the per-frame reset. (PSXPORT_ATTACH and its value-keyed ring are retired.)
static int s_attach = -1;
int attach_enabled(void) { if (s_attach < 0) s_attach = (cfg_on("PSXPORT_NATIVE_DEPTH") || cfg_on("PSXPORT_SBS") || cfg_on("PSXPORT_SSAO") || cfg_on("PSXPORT_LIGHT")) ? 1 : 0;
                           return s_attach > 0; }
// engine_submit sets the projection-plane H (read from CR26) so proj_pz_to_ord normalizes depth correctly.
void proj_set_H(uint16_t h) { s_proj_H = h; }
// Near-plane view-Z used by proj_pz_to_ord (= H/2, clamped >=1). SSAO needs it to invert the banded
// depth back to a linear view-space Z (1/pz is affine in the stored depth — see proj_pz_to_ord).
float proj_near_pz(void) { float n = s_proj_H ? (float)s_proj_H * 0.5f : 1.0f; return n < 1.0f ? 1.0f : n; }
// Projection plane distance H (CR26) and screen center (OFX/OFY) — PSXPORT_LIGHT reconstructs view-space
// position from a depth pixel as P = ((sx-cx)*pz/H, (sy-cy)*pz/H, pz), then the surface normal from it.
float proj_plane_h(void) { return s_proj_H ? (float)s_proj_H : 1.0f; }
void  proj_screen_center(float* cx, float* cy) { if (cx) *cx = s_proj_cx; if (cy) *cy = s_proj_cy; }

void     gte_op(R3000* c, uint32_t insn)         { GTE_Instruction(insn);
                                                   unsigned op = insn & 0x3F;
                                                   if (s_gteprobe < 0) { const char* e = cfg_str("PSXPORT_GTEPROBE"); s_gteprobe = e ? atoi(e) : 0; }
                                                   if (s_gteprobe > 0) s_gte_hist[op]++;
                                                   if (op == 0x01 || op == 0x30) {
                                                     ws_sx_record();          // self-gated (PSXPORT_WS_SXHIST)
                                                     rtpcaller_record(c->r[31]);   // self-gated (PSXPORT_RTPCALLER)
                                                     if (g_fps60_on) fps60_rtp(op);
                                                     if (s_projprobe < 0) { s_projprobe = cfg_on("PSXPORT_PROJPROBE") ? 1 : 0;
                                                                            if (s_projprobe && !s_divtab_init) proj_divtab_init(); }
                                                     if (s_projprobe > 0) {
                                                       // Compare native projection to Beetle's outputs. After RTPS the single vertex
                                                       // is in XY_FIFO(3)=DR15 / Z_FIFO(3)=DR19 / IR=DR9-11. After RTPT the 3 verts
                                                       // land in XY DR12,DR13,DR14 (DR15==DR14, see push quirk below) and Z DR17-19;
                                                       // IR holds only the last vertex.
                                                       if (op == 0x01) {
                                                         uint32_t xy = gte_read_data(15), z = gte_read_data(19);
                                                         proj_probe_one(0, insn, (int16_t)gte_read_data(9), (int16_t)gte_read_data(10),
                                                                        (int16_t)gte_read_data(11), (uint16_t)z, (int16_t)xy, (int16_t)(xy >> 16));
                                                       } else {
                                                         // XY_FIFO push duplicates the new value into slots 2 & 3 (gte.c TransformXY:
                                                         // XY_FIFO(2)=XY_FIFO(3) runs after (3)=new), so after 3 pushes v0,v1,v2 land in
                                                         // DR12,DR13,DR14 (DR15==DR14). Z_FIFO shifts cleanly: v0,v1,v2 -> DR17,DR18,DR19.
                                                         for (unsigned i = 0; i < 3; i++) {
                                                           uint32_t xy = gte_read_data(12 + i), z = gte_read_data(17 + i);
                                                           // IR1-3 only valid for the last vertex; pass native's own for the others so
                                                           // the IR check is meaningful only on vertex 2 (others compare native vs native).
                                                           int bir1, bir2, bir3;
                                                           if (i == 2) { bir1 = (int16_t)gte_read_data(9); bir2 = (int16_t)gte_read_data(10); bir3 = (int16_t)gte_read_data(11); }
                                                           else { ProjVtx t; proj_native_vertex(i, insn, &t); bir1 = t.ir1; bir2 = t.ir2; bir3 = t.ir3; }
                                                           proj_probe_one(i, insn, bir1, bir2, bir3, (uint16_t)z, (int16_t)xy, (int16_t)(xy >> 16));
                                                         }
                                                       }
                                                     }
                                                   } }
void     gte_init(void)                          { GTE_Init(); GTE_Power();
  // PSXPORT_WIDE is PC-native widescreen now: the GTE keeps its NATIVE projection (NO squish) and the
  // renderer (gpu_vk) re-centers the geometry into a wider scratch framebuffer at a true wider FOV.
  // The old emulator squish-X + display-stretch hack (widescreen_hack=1) is intentionally NOT used —
  // it loses horizontal resolution and stretches the 2D HUD (user rejected it). Keep the hack OFF. }
  widescreen_hack = 0; }
