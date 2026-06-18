// fps60 — interpolated-60fps tier for the native PC port (design: docs/fps60_recomp_60fps.md).
//
// This file owns the capture buffers, the logic-rate detector, the object→primitive join, and
// (later) the matcher + in-between synthesizer. GATED behind PSXPORT_FPS60 and purely additive:
// when off, the taps in gte_beetle.c / gpu_native.c / games_tomba2.c are no-ops and the faithful
// 4:3/30fps path is byte-identical.
//
// MILESTONE 1 (done): measured Tomba2's logic rate = 30 fps (period 1, quota 2) → one in-between/frame.
// MILESTONE 2 (this commit): the OBJECT→PRIMITIVE JOIN — the matcher's foundation and the design's
//   #1 open risk ("re-validate on the native path"). Tag every RTP-produced SXY with the current
//   object id (from the 0x8007712C cull dispatcher), then at draw time join each GP0 polygon to a
//   captured SXY by vertex coords (±2px). Report the join rate: the fraction of drawn polys that are
//   object-matchable (3D models) vs. unjoinable (CPU-projected terrain / 2D HUD → will snap).
#include <stdint.h>
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

uint8_t  mem_r8(uint32_t);
uint32_t GTE_ReadDR(unsigned which);   // Beetle GTE data regs (SXY-FIFO = DR12/13/14)

int g_fps60_on = 0;          // read by the gte_op tap; set by fps60_init from PSXPORT_FPS60
uint32_t g_current_object = 0;// set by games_tomba2.c ov_object_cull during the cull subtree

// ---- per-frame projected-geometry fingerprint (rate detector input) -----------------
static uint64_t s_frame_hash = 1469598103934665603ull;
static long     s_frame_geom = 0;
static long     s_fence = 0;       // per-logic-frame counter (the synth dumptest keys on it)

static void fold(uint32_t v) {
  uint64_t h = s_frame_hash;
  for (int i = 0; i < 4; i++) { h ^= (v & 0xFF); h *= 1099511628211ull; v >>= 8; }
  s_frame_hash = h; s_frame_geom++;
}

// ---- SXY → object-id grid (the join) -------------------------------------------------
// Last object that projected a vertex to each (sx,sy). Epoch-stamped so it resets per frame with
// no 2 MB memset: a cell is live only when its stamp == the current epoch.
#define GW 1024
#define GH 512
static uint32_t s_obj_grid[GW * GH];
static uint32_t s_obj_stamp[GW * GH];
static uint32_t s_epoch = 0;
static long s_join_hit, s_join_miss;     // accumulated over the report window

static void grid_put(int sx, int sy, uint32_t obj) {
  int x = sx & (GW - 1), y = sy & (GH - 1);
  int i = y * GW + x;
  s_obj_grid[i] = obj; s_obj_stamp[i] = s_epoch;
}
static uint32_t grid_get(int px, int py) {  // ±2px search; returns the source object's node pointer
  for (int dy = -2; dy <= 2; dy++)          // (0 = no object near here / CPU-projected → caller snaps)
    for (int dx = -2; dx <= 2; dx++) {
      int x = (px + dx) & (GW - 1), y = (py + dy) & (GH - 1);
      int i = y * GW + x;
      if (s_obj_stamp[i] == s_epoch && s_obj_grid[i]) return s_obj_grid[i];
    }
  return 0;
}

uint32_t GTE_ReadCR(unsigned which);

// ---- native graphical objects (GTE transform groups) ---------------------------------
// The game's camera + models, ported to native: every run of RTPS/RTPT sharing one GTE transform
// (rotation matrix CR0-4 + translation CR5-7 = the model-view, camera baked in) is ONE object. Its
// identity ACROSS frames is its local-vertex fingerprint (the model mesh is invariant; only the
// transform moves). Interpolating each object's transform between frames and re-projecting its verts
// reproduces camera pan + object motion perspective-correctly. (RE: the 0x8007712C cull dispatcher does
// NOT tag these scenes — the GTE transform is the real object identity.)
#define XOBJ_MAX 1024
#define XV_MAX   80000
typedef struct {
  uint32_t r0, r1, r2, r3, r4;     // rotation matrix, GTE control regs CR0..4 (packed int16 pairs)
  int32_t  trx, try_, trz;         // translation, CR5..7
  uint64_t fp;                     // local-vertex fingerprint = cross-frame identity
  long     nrtps;                  // RTPS/RTPT count (object size)
  int      v0, nv;                 // range [v0,v0+nv) into the per-frame local-vertex pool
} XObj;
static XObj s_xa[XOBJ_MAX], s_xb[XOBJ_MAX];
static XObj* s_xA = s_xa;          // previous frame's objects (transforms; verts not needed)
static XObj* s_xB = s_xb;          // current frame's objects (capturing)
static int   s_nxA = 0, s_nxB = 0;
static int   s_xb_started = 0;     // a group is open in s_xB
// Per-frame local-vertex pool for the CURRENT frame (B): each captured vertex's model-space coords and
// the screen XY the GTE produced for it (the key we remap on). Rebuilt every frame.
static int16_t s_lvx[XV_MAX], s_lvy[XV_MAX], s_lvz[XV_MAX];
static int32_t s_osxy[XV_MAX];
static int     s_nv = 0;
static uint32_t s_rtps_insn = 0x00080001; // a real RTPS instruction word (flags) for re-projection

static void xfold(XObj* o, uint32_t v) {     // fold a local-vertex word into the fingerprint
  o->fp ^= v + 0x9E3779B97F4A7C15ull + (o->fp << 6) + (o->fp >> 2);
}
static void xvert(int16_t vx, int16_t vy, int16_t vz, uint32_t sxy) {
  if (s_nv >= XV_MAX) return;
  s_lvx[s_nv] = vx; s_lvy[s_nv] = vy; s_lvz[s_nv] = vz; s_osxy[s_nv] = (int32_t)sxy; s_nv++;
}

// Called per RTPS(0x01)/RTPT(0x30) from fps60_rtp, with the GTE holding this vertex's transform.
static void xobj_rtp(uint32_t insn) {
  uint32_t op = insn & 0x3F;
  if (op == 0x01) s_rtps_insn = insn;          // remember the game's RTPS flags for re-projection
  uint32_t r0 = GTE_ReadCR(0), r1 = GTE_ReadCR(1), r2 = GTE_ReadCR(2), r3 = GTE_ReadCR(3), r4 = GTE_ReadCR(4);
  int32_t  t5 = (int32_t)GTE_ReadCR(5), t6 = (int32_t)GTE_ReadCR(6), t7 = (int32_t)GTE_ReadCR(7);
  XObj* o = s_xb_started ? &s_xB[s_nxB - 1] : NULL;
  int same = o && o->r0==r0 && o->r1==r1 && o->r2==r2 && o->r3==r3 && o->r4==r4
               && o->trx==t5 && o->try_==t6 && o->trz==t7;
  if (!same) {                                  // transform changed → new object
    if (s_nxB >= XOBJ_MAX) return;
    o = &s_xB[s_nxB++]; s_xb_started = 1;
    o->r0=r0; o->r1=r1; o->r2=r2; o->r3=r3; o->r4=r4; o->trx=t5; o->try_=t6; o->trz=t7;
    o->fp = 1469598103934665603ull; o->nrtps = 0; o->v0 = s_nv; o->nv = 0;
  }
  // Capture LOCAL input verts (model-space, frame-invariant) + each one's screen output, and fold verts
  // into the cross-frame fingerprint. RTPS: 1 vert (DR0/1) → SXY DR14. RTPT: 3 verts (DR0/1,2/3,4/5) →
  // SXY DR12,DR13,DR14 in order.
  uint32_t v0 = GTE_ReadDR(0); int16_t z0 = (int16_t)GTE_ReadDR(1);
  xfold(o, v0); xfold(o, (uint16_t)z0);
  if (op == 0x30) {
    uint32_t v1 = GTE_ReadDR(2); int16_t z1 = (int16_t)GTE_ReadDR(3);
    uint32_t v2 = GTE_ReadDR(4); int16_t z2 = (int16_t)GTE_ReadDR(5);
    xfold(o, v1); xfold(o, (uint16_t)z1); xfold(o, v2); xfold(o, (uint16_t)z2);
    xvert((int16_t)(v0 & 0xFFFF), (int16_t)(v0 >> 16), z0, GTE_ReadDR(12));
    xvert((int16_t)(v1 & 0xFFFF), (int16_t)(v1 >> 16), z1, GTE_ReadDR(13));
    xvert((int16_t)(v2 & 0xFFFF), (int16_t)(v2 >> 16), z2, GTE_ReadDR(14));
    o->nv += 3;
  } else {
    xvert((int16_t)(v0 & 0xFFFF), (int16_t)(v0 >> 16), z0, GTE_ReadDR(14));
    o->nv += 1;
  }
  o->nrtps++;
}

// Match B objects to A by fingerprint; report match rate + transform deltas (interpolation viability).
static void xobj_report(void) {
  long matched = 0, tot = s_nxB, dtr_sum = 0, dtr_max = 0;
  for (int i = 0; i < s_nxB; i++) {
    XObj* B = &s_xB[i];
    for (int j = 0; j < s_nxA; j++) if (s_xA[j].fp == B->fp && s_xA[j].nrtps == B->nrtps) {
      XObj* A = &s_xA[j]; matched++;
      long d = labs(B->trx-A->trx) + labs(B->try_-A->try_) + labs(B->trz-A->trz);
      dtr_sum += d; if (d > dtr_max) dtr_max = d; break;
    }
  }
  fprintf(stderr, "[xobj] f%ld  objects=%ld  matched=%ld (%.1f%%)  TRdelta avg=%ld max=%ld\n",
          s_fence, tot, matched, tot ? 100.0*matched/tot : 0.0,
          matched ? dtr_sum/matched : 0, dtr_max);
}

static void xobj_commit(void) {                 // swap A/B at frame end; reset the per-frame vert pool
  XObj* t = s_xA; s_xA = s_xB; s_xB = t; s_nxA = s_nxB; s_nxB = 0; s_xb_started = 0; s_nv = 0;
}

// ---- transform interpolation + GTE re-projection -------------------------------------
// For each current object (B) matched to a previous one (A) by fingerprint, interpolate the transform
// (rotation matrix + translation at t=0.5) and re-project the object's local verts through the REAL
// Beetle GTE → an old-SXY→new-SXY remap. Rendering the in-between then just remaps B's prim vertices
// through this table (unmapped verts = CPU/2D/unmatched → snap). This is the camera+object motion,
// perspective-correct, with the game's own projection math.
void     GTE_WriteCR(unsigned, uint32_t);
void     GTE_WriteDR(unsigned, uint32_t);
int32_t  GTE_Instruction(uint32_t);

static int s_xmatch[XOBJ_MAX];   // B object i → A object index, or -1
static int s_disp_gate = -1;     // PSXPORT_FPS60_GATE: max screen motion (px) to still interpolate

static void xobj_match(void) {   // by fingerprint + identical vertex count (strong key)
  for (int i = 0; i < s_nxB; i++) {
    s_xmatch[i] = -1;
    XObj* B = &s_xB[i];
    for (int j = 0; j < s_nxA; j++)
      if (s_xA[j].fp == B->fp && s_xA[j].nrtps == B->nrtps) { s_xmatch[i] = j; break; }
  }
}

// average the two packed int16 halves of two GTE matrix control registers
static uint32_t interp_packed(uint32_t a, uint32_t b) {
  int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
  int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
  uint16_t lo = (uint16_t)(((int)al + bl) / 2), hi = (uint16_t)(((int)ah + bh) / 2);
  return lo | ((uint32_t)hi << 16);
}

// old-SXY → new-SXY remap (open-addressing hash; key 0x80000000 reserved as empty marker→never a SXY)
#define REMAP_SZ 131072
static int32_t s_rm_key[REMAP_SZ];
static int32_t s_rm_val[REMAP_SZ];
static int      s_rm_init = 0;
static void remap_reset(void) {
  for (int i = 0; i < REMAP_SZ; i++) s_rm_key[i] = (int32_t)0x80000000;
  s_rm_init = 1;
}
static void remap_put(int32_t key, int32_t val) {
  uint32_t h = (uint32_t)key * 2654435761u;
  for (int n = 0; n < REMAP_SZ; n++) { int s = (int)((h + n) & (REMAP_SZ - 1));
    if (s_rm_key[s] == (int32_t)0x80000000 || s_rm_key[s] == key) { s_rm_key[s] = key; s_rm_val[s] = val; return; } }
}
static int remap_get(int32_t key, int32_t* out) {
  uint32_t h = (uint32_t)key * 2654435761u;
  for (int n = 0; n < REMAP_SZ; n++) { int s = (int)((h + n) & (REMAP_SZ - 1));
    if (s_rm_key[s] == (int32_t)0x80000000) return 0;
    if (s_rm_key[s] == key) { *out = s_rm_val[s]; return 1; } }
  return 0;
}

// Build the remap: interpolate each matched object's transform and re-project its verts through the GTE.
static void fps60_build_remap(void) {
  if (!s_rm_init) remap_reset(); else remap_reset();
  if (s_nxB == 0) return;
  if (s_disp_gate < 0) { const char* g = cfg_str("PSXPORT_FPS60_GATE"); s_disp_gate = g ? atoi(g) : 48; }
  // save the GTE transform registers we overwrite (projection consts CR24-26 are left intact)
  uint32_t save[8]; for (int i = 0; i < 8; i++) save[i] = GTE_ReadCR(i);
  for (int i = 0; i < s_nxB; i++) {
    int j = s_xmatch[i]; if (j < 0) continue;
    XObj* B = &s_xB[i]; XObj* A = &s_xA[j];
    long d = labs(B->trx - A->trx) + labs(B->try_ - A->try_) + labs(B->trz - A->trz);
    if (d > s_disp_gate * 64) continue;          // wrong/teleport match (TR in fixed units) → snap object
    GTE_WriteCR(0, interp_packed(A->r0, B->r0)); GTE_WriteCR(1, interp_packed(A->r1, B->r1));
    GTE_WriteCR(2, interp_packed(A->r2, B->r2)); GTE_WriteCR(3, interp_packed(A->r3, B->r3));
    GTE_WriteCR(4, interp_packed(A->r4, B->r4));
    GTE_WriteCR(5, (uint32_t)((A->trx + B->trx) / 2));
    GTE_WriteCR(6, (uint32_t)((A->try_ + B->try_) / 2));
    GTE_WriteCR(7, (uint32_t)((A->trz + B->trz) / 2));
    for (int k = B->v0; k < B->v0 + B->nv && k < s_nv; k++) {
      GTE_WriteDR(0, ((uint32_t)(uint16_t)s_lvy[k] << 16) | (uint16_t)s_lvx[k]);   // VXY0
      GTE_WriteDR(1, (uint32_t)(uint16_t)s_lvz[k]);                                // VZ0
      GTE_Instruction(s_rtps_insn);
      remap_put(s_osxy[k], (int32_t)GTE_ReadDR(14));
    }
  }
  for (int i = 0; i < 8; i++) GTE_WriteCR(i, save[i]);   // restore the game's transform regs
}

// gte_op RTP tap. op 0x01 = RTPS (one new SXY, DR14); 0x30 = RTPT (three, DR12/13/14).
long s_rtp_calls = 0, s_rtp_with_obj = 0;   // diag: how many RTPS carry an object context
void fps60_rtp(uint32_t op) {
  if (!g_fps60_on) return;
  s_rtp_calls++; if (g_current_object) s_rtp_with_obj++;
  xobj_rtp(op);                 // capture this vertex's GTE transform-group (native object)
  unsigned lo = (op == 0x30) ? 12 : 14, hi = 14;
  for (unsigned r = lo; r <= hi; r++) {
    uint32_t sxy = GTE_ReadDR(r);
    fold(sxy);
    int16_t sx = (int16_t)(sxy & 0xFFFF), sy = (int16_t)(sxy >> 16);
    grid_put(sx, sy, g_current_object);
  }
}

// gp0_exec polygon tap: join the packet's lead vertex to a captured SXY.
void fps60_join_poly(int px, int py) {
  if (!g_fps60_on) return;
  if (grid_get(px, py)) s_join_hit++; else s_join_miss++;
}

// ---- full primitive capture: PrimFrame A (prev) / B (current) ------------------------
// Every completed GP0 draw is teed here so the in-between synthesizer (next milestone) can
// re-rasterize a lerped copy of frame B's display list. Coords are stored in PACKET space
// (pre-E5-offset = buffer-relative); the E5 offset at draw time is kept so the synth can apply
// the CURRENT frame's buffer origin and so we can reason in absolute space when needed. Polys
// carry the joined object id (the matcher's primary key); sprites/lines get obj=0 → they snap.
typedef struct {
  uint8_t  op, nv;
  int16_t  x[4], y[4];
  uint8_t  u[4], v[4];
  uint8_t  r[4], g[4], b[4];
  int16_t  w, h;                 // sprite/rect size (nv==1); unused for polys/lines
  int16_t  off_x, off_y;         // E5 draw offset at draw time (the framebuffer origin)
  int16_t  tp_x, tp_y;           // texpage base (px)
  uint8_t  mode, blend, dither;  // texture color mode / semi-transparency mode / ordered-dither
  int16_t  clut_x, clut_y;       // CLUT base (px)
  uint32_t obj;                  // joined object id (0 = unjoined → snap)
} Prim;

#define PRIM_MAX 8192
static Prim s_prim_a[PRIM_MAX], s_prim_b[PRIM_MAX];
static Prim* s_pA = s_prim_a;   // previous frame's prims
static Prim* s_pB = s_prim_b;   // current frame's prims (being captured)
static int   s_nA = 0, s_nB = 0;
static int   s_overflow = 0;    // set if a frame exceeded PRIM_MAX (report it)

// Full capture for polygons (nv 3/4). xs/ys are packet coords; us/vs/rs/gs/bs per vertex.
void fps60_cap_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                     const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                     int off_x, int off_y, int tp_x, int tp_y, int mode, int blend, int dither,
                     int clut_x, int clut_y) {
  if (!g_fps60_on) return;
  if (s_nB >= PRIM_MAX) { s_overflow = 1; return; }
  Prim* P = &s_pB[s_nB++];
  P->op = (uint8_t)op; P->nv = (uint8_t)nv; P->w = P->h = 0;
  for (int i = 0; i < nv; i++) {
    P->x[i] = (int16_t)xs[i]; P->y[i] = (int16_t)ys[i];
    P->u[i] = (uint8_t)us[i]; P->v[i] = (uint8_t)vs[i];
    P->r[i] = rs[i]; P->g[i] = gs[i]; P->b[i] = bs[i];
  }
  P->off_x = (int16_t)off_x; P->off_y = (int16_t)off_y;
  P->tp_x = (int16_t)tp_x; P->tp_y = (int16_t)tp_y; P->mode = (uint8_t)mode; P->blend = (uint8_t)blend;
  P->dither = (uint8_t)dither;
  P->clut_x = (int16_t)clut_x; P->clut_y = (int16_t)clut_y;
  P->obj = grid_get(xs[0], ys[0]);          // join the lead vertex to its object (0 = snap)
}

// Lighter capture for sprites/rects (nv==1) and lines (nv==2). These are CPU-projected / HUD /
// 2D — they don't pass through the GTE, so obj=0 (snap) by design. Captured for completeness so
// the in-between redraws the WHOLE display list (no holes).
void fps60_cap_sprite(int op, int x, int y, int u, int v, int w, int h,
                       int r, int g, int b, int off_x, int off_y,
                       int tp_x, int tp_y, int mode, int blend, int clut_x, int clut_y) {
  if (!g_fps60_on) return;
  if (s_nB >= PRIM_MAX) { s_overflow = 1; return; }
  Prim* P = &s_pB[s_nB++];
  P->op = (uint8_t)op; P->nv = 1;
  P->x[0] = (int16_t)x; P->y[0] = (int16_t)y; P->u[0] = (uint8_t)u; P->v[0] = (uint8_t)v;
  P->r[0] = (uint8_t)r; P->g[0] = (uint8_t)g; P->b[0] = (uint8_t)b;
  P->w = (int16_t)w; P->h = (int16_t)h;
  P->off_x = (int16_t)off_x; P->off_y = (int16_t)off_y;
  P->tp_x = (int16_t)tp_x; P->tp_y = (int16_t)tp_y; P->mode = (uint8_t)mode; P->blend = (uint8_t)blend;
  P->clut_x = (int16_t)clut_x; P->clut_y = (int16_t)clut_y;
  P->obj = 0;
}

// Line-segment capture (nv==2). Lines are CPU-projected / 2D (obj 0 → snap), captured so the interp
// frame reproduces them (else they flicker on/off every other frame). Color is per-segment flat.
void fps60_cap_line(int op, int x0, int y0, int x1, int y1, int r, int g, int b, int semi) {
  if (!g_fps60_on) return;
  if (s_nB >= PRIM_MAX) { s_overflow = 1; return; }
  Prim* P = &s_pB[s_nB++];
  P->op = (uint8_t)op; P->nv = 2;
  P->x[0] = (int16_t)x0; P->y[0] = (int16_t)y0; P->x[1] = (int16_t)x1; P->y[1] = (int16_t)y1;
  P->r[0] = (uint8_t)r; P->g[0] = (uint8_t)g; P->b[0] = (uint8_t)b;
  P->blend = (uint8_t)semi;           // reuse `blend` to carry the segment's semi-transparency flag
  P->obj = 0;
}

// ---- in-between synthesizer ----------------------------------------------------------
// Re-rasterize frame B's captured display list into the SEPARATE s_interp buffer at the FRONT origin
// (ON TOP of the renderer, VRAM untouched). Each prim is rigidly TRANSLATED by its source object's
// half screen-motion (ocen_delta, keyed by the node pointer Prim.obj); obj==0 / unmatched → snap.
// Whole list redrawn over a cleared region (background + HUD reproduced, no holes); textures from VRAM.
//
// STATUS (2026-06-16): the per-object translation math is correct, but Prim.obj is currently always 0
// in gameplay — the render-time RTPS is a SEPARATE pass with no object context (measured: rtp/frame
// ~3268, rtp_with_obj=0), so the cull/handler-walk tag never reaches the drawn geometry. It therefore
// falls back to all-snap (clean, no artifacts, but no interpolation). UNBLOCK: tag draws by object at
// the real render pass — cleanest via the planned native (VK) renderer, which knows what it draws.
// See docs/journal.md later-86.
void gpu_fps60_begin_interp(int, int, int, int, int, int);
void gpu_fps60_end_interp(void);
void gpu_fps60_draw_poly(int, int, const int*, const int*, const int*, const int*,
                       const unsigned char*, const unsigned char*, const unsigned char*,
                       int, int, int, int, int, int, int);
void gpu_fps60_draw_sprite(int, int, int, int, int, int, int, int, int, int,
                         int, int, int, int, int, int);
void gpu_fps60_draw_line(int, int, int, int, int, int, int, int);

static int fps60_front_off_y(void) { return s_nB > 0 ? s_pB[0].off_y : 0; }

// ---- per-object screen-centroid motion (interpolation key = the node pointer) --------
// Each captured poly is tagged with its source object's pool-slot pointer (Prim.obj, from the
// cull-dispatcher tap g_current_object). An object's screen motion = the delta of its prim-centroid
// between this frame (B) and the previous (A), matched by that POINTER — a stable engine identity
// (no GTE fingerprints, no screen-XY collisions). The in-between translates each matched object's
// prims to the midpoint (A+B)/2. obj==0 (2D/HUD/CPU-projected) and sprites/lines → snap (30fps).
#define OCEN_SZ 8192
typedef struct { uint32_t obj; int32_t sx, sy, n; } ObjCen;
static ObjCen s_oc0[OCEN_SZ], s_oc1[OCEN_SZ];
static ObjCen* s_ocA = s_oc0;          // previous frame's per-object centroids
static ObjCen* s_ocB = s_oc1;          // current frame's
static int s_ocen_gate = -1;           // PSXPORT_FPS60_GATE: max per-object screen motion (px L1)

static ObjCen* ocen_slot(ObjCen* t, uint32_t obj) {   // open-addressing; obj != 0
  uint32_t h = obj * 2654435761u;
  for (int i = 0; i < OCEN_SZ; i++) { ObjCen* s = &t[(h + i) & (OCEN_SZ - 1)];
    if (s->obj == 0 || s->obj == obj) return s; }
  return 0;
}
static void ocen_build(ObjCen* t, Prim* prims, int n) {   // accumulate per-object vertex centroids
  for (int i = 0; i < OCEN_SZ; i++) { t[i].obj = 0; t[i].sx = t[i].sy = t[i].n = 0; }
  for (int i = 0; i < n; i++) { Prim* P = &prims[i];
    if (P->obj == 0 || P->nv < 3) continue;            // only object-tagged polys carry GTE motion
    ObjCen* s = ocen_slot(t, P->obj); if (!s) continue;
    s->obj = P->obj;
    for (int k = 0; k < P->nv; k++) { s->sx += P->x[k]; s->sy += P->y[k]; s->n++; }
  }
}
static int ocen_centroid(ObjCen* t, uint32_t obj, int* cx, int* cy) {
  ObjCen* s = ocen_slot(t, obj);
  if (!s || s->obj != obj || s->n == 0) return 0;
  *cx = s->sx / s->n; *cy = s->sy / s->n; return 1;
}
// This object's B→midpoint translation (dx,dy), or (0,0) if unmatched / over the teleport gate.
static int ocen_delta(uint32_t obj, int* dx, int* dy) {
  *dx = *dy = 0;
  int bx, by, ax, ay;
  if (obj == 0 || !ocen_centroid(s_ocB, obj, &bx, &by) || !ocen_centroid(s_ocA, obj, &ax, &ay))
    return 0;
  int mx = bx - ax, my = by - ay;
  if (s_ocen_gate < 0) { const char* g = cfg_str("PSXPORT_FPS60_GATE"); s_ocen_gate = g ? atoi(g) : 64; }
  if (abs(mx) + abs(my) > s_ocen_gate) return 0;       // scene cut / teleport → snap (no smear)
  *dx = -mx / 2; *dy = -my / 2;                         // B shifted back to (A+B)/2
  return 1;
}

static int s_sdbg = -1;
// Returns the number of prims actually translated (interpolated). 0 ⇒ nothing moved, so the caller
// should present the REAL frame instead of this (lossy) re-rasterized in-between (see fps60_present).
static long fps60_synthesize(void) {
  if (s_nB == 0) return 0;
  if (s_sdbg < 0) s_sdbg = cfg_dbg("fps60") ? 1 : 0;
  long d_prims = 0, d_obj_translated = 0, d_snapped = 0, d_tagged = 0;  // sdbg: interpolation outcome
  long moved_count = 0;
  int fy = fps60_front_off_y();
  gpu_fps60_begin_interp(0, fy, 0, fy, 319, fy + 239);     // target=s_interp, clear region, set env
  for (int i = 0; i < s_nB; i++) {
    Prim* B = &s_pB[i];
    int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
    // Per-OBJECT 2D screen translation to the midpoint, keyed by the node pointer (Prim.obj). The whole
    // primitive moves rigidly by its object's half-motion — no per-vertex remap, so 2D/HUD/unmatched
    // prims (obj 0 → dx=dy=0) simply snap, and a matched object can never stretch or duplicate.
    int dx, dy, moved = ocen_delta(B->obj, &dx, &dy);
    for (int k = 0; k < B->nv; k++) {
      xs[k] = B->x[k] + dx; ys[k] = B->y[k] + dy;
      us[k] = B->u[k]; vs[k] = B->v[k]; rs[k] = B->r[k]; gs[k] = B->g[k]; bs[k] = B->b[k];
    }
    if (moved) moved_count++;
    if (s_sdbg) { d_prims++; if (moved) d_obj_translated++; else d_snapped++;
                  if (B->obj) d_tagged++; }
    if (B->nv == 1)
      gpu_fps60_draw_sprite(B->op, xs[0], ys[0], us[0], vs[0], B->w, B->h, rs[0], gs[0], bs[0],
                          B->tp_x, B->tp_y, B->mode, B->blend, B->clut_x, B->clut_y);
    else if (B->nv == 2)
      gpu_fps60_draw_line(xs[0], ys[0], xs[1], ys[1], rs[0], gs[0], bs[0], B->blend);
    else
      gpu_fps60_draw_poly(B->op, B->nv, xs, ys, us, vs, rs, gs, bs,
                        B->tp_x, B->tp_y, B->mode, B->blend, B->dither, B->clut_x, B->clut_y);
  }
  gpu_fps60_end_interp();
  if (s_sdbg)
    fprintf(stderr, "[fps60-sdbg] f%ld prims=%ld  tagged=%ld  translated=%ld  snapped=%ld  "
            "rtp=%ld rtp_with_obj=%ld\n",
            s_fence, d_prims, d_tagged, d_obj_translated, d_snapped, s_rtp_calls, s_rtp_with_obj);
  s_rtp_calls = s_rtp_with_obj = 0;
  return moved_count;
}

// PSXPORT_FPS60_SYNTH=frame — headless validation: at logic fence `frame`, dump A (prev real frame =
// back buffer), the synthesized interpolated frame (from s_interp), and B (front buffer), so the
// interpolation can be eyeballed (objects at intermediate positions) WITHOUT the live present path.
void gpu_fps60_shot_vram(int, int, const char*);
void gpu_fps60_shot_interp(int, int, const char*);
static void fps60_synth_dumptest(void) {
  static int tf = -2;
  if (tf == -2) { const char* e = cfg_str("PSXPORT_FPS60_SYNTH"); tf = e ? atoi(e) : -1; }
  if (tf < 0 || s_fence != tf || s_nB == 0) return;
  int fy = fps60_front_off_y(), by = fy ^ 256;
  gpu_fps60_shot_vram(0, by, "scratch/screenshots/fps60_A.ppm");        // previous real frame
  fps60_synthesize();                                              // -> s_interp at front origin
  gpu_fps60_shot_interp(0, fy, "scratch/screenshots/fps60_inbetween.ppm");
  gpu_fps60_shot_vram(0, fy, "scratch/screenshots/fps60_B.ppm");        // current real frame
  fprintf(stderr, "[fps60] synth dumptest f%d: A/inbetween/B -> scratch/screenshots/fps60_*.ppm "
          "(front_y=%d back_y=%d, %d prims)\n", tf, fy, by, s_nB);
}

// ---- live 60fps present (windowed): 1 frame behind --------------------------------------
// Per logic frame present TWO display frames: the PREVIOUS real frame (still intact in the back VRAM
// buffer, blitted read-only) then the INTERPOLATED frame (rasterized into s_interp). The current
// frame B is held — it becomes the "previous" (back buffer) next logic frame, so output lags by one
// frame and the displayed stream is A, lerp(A,B), B, lerp(B,C), C… = 60 fps. SAFETY GATE: only when
// windowed + animating + actually double-buffering (front_y flips), else one faithful present of B.
void gpu_present_ex(int);
void gpu_pace_subframe(int);

static int s_prev_front_y = -1;    // last frame's front-buffer y (for flip detection)

static void fps60_present(void) {
  static int win = -1;
  if (win < 0) { const char* w = cfg_str("PSXPORT_GPU_WINDOW"); win = (w && atoi(w) != 0) ? 1 : 0; }
  void gpu_fps60_blit_vram(int, int); void gpu_fps60_blit_interp(int, int);
  int fy = fps60_front_off_y();
  int flipped = (fy != s_prev_front_y);
  s_prev_front_y = fy;
  if (win && flipped && s_frame_geom > 0 && s_nB > 0) {
    gpu_fps60_blit_vram(0, fy ^ 256);   // present the previous real frame (lives in the back buffer)
    gpu_pace_subframe(2);
    long moved = fps60_synthesize(); // interpolated frame -> s_interp (front origin), VRAM untouched
    // The re-rasterized in-between is LOSSY (re-draws only the captured GP0 subset; missing occluders/
    // fills/blend-order make hidden objects reappear → spurious copies). Only trust it when objects were
    // actually interpolated; otherwise present the REAL current frame (no artifacts). Real interpolation
    // needs object-tagged draws from the native renderer (see docs/journal.md later-86/87).
    if (moved > 0) gpu_fps60_blit_interp(0, fy);
    else           gpu_fps60_blit_vram(0, fy);   // real current frame
    gpu_pace_subframe(2);
    gpu_present_ex(0);                // bookkeeping only (watchdog, s_frame++, diagnostics; no blit)
  } else {
    gpu_present_ex(1);               // faithful single present of frame B (front buffer)
    gpu_pace_subframe(1);
  }
}

// ---- logic-rate detector (lrate_proto.c, validated) ---------------------------------
typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;
static RateDet s_rd = { .period = 2 };

static void rate_tick(RateDet* d, uint64_t set_hash) {
  if (set_hash == d->last_hash) { d->held++; return; }
  int p = d->held + 1;
  if (p >= 1 && p <= 8) d->votes[p]++;
  int best = 0, bp = 2;
  for (int i = 1; i <= 8; i++) if (d->votes[i] > best) { best = d->votes[i]; bp = i; }
  d->period = bp;
  d->last_hash = set_hash; d->held = 0; d->changes++;
}

// ---- per-logic-frame fence (games_tomba2.c ov_frame_update) -------------------------
void fps60_frame_commit(void) {
  if (!g_fps60_on) return;
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  // Object interpolation: tag each poly with its source object (pool-slot pointer) at capture, then
  // here compute this frame's per-object screen centroids (B) and match to last frame (A) BY POINTER.
  // The synth translates each matched object's prims to the midpoint. Must run before the A/B swap.
  ocen_build(s_ocB, s_pB, s_nB);
  if (s_sdbg < 0) s_sdbg = cfg_dbg("fps60") ? 1 : 0;
  // GTE-transform object matching (the camera+model identity that does NOT need the per-poly object
  // tag). Match this frame's transform-groups (B) to last frame's (A) by local-vertex fingerprint, then
  // commit (B→A) for the next frame. Reported under PSXPORT_DEBUG=fps60 to validate cross-frame object
  // identity + transform deltas — the foundation for re-submit interpolation. Must run before xobj_commit.
  xobj_match();
  if (s_sdbg) xobj_report();
  xobj_commit();                    // swap s_xA/s_xB + reset the per-frame local-vertex pool
  if (s_sdbg) fps60_synthesize();   // per-frame interpolation stats (headless diagnostic only)
  fps60_synth_dumptest();   // PSXPORT_FPS60_SYNTH: offline A/in-between/B dump (no live-path change)
  fps60_present();          // owns presentation: 60fps pair (prev + interpolated) or faithful single

  // Swap per-object centroids (B→A) and the prim double-buffer (so s_pB is clean next frame).
  { ObjCen* t = s_ocA; s_ocA = s_ocB; s_ocB = t; }
  { Prim* t = s_pA; s_pA = s_pB; s_pB = t; s_nA = s_nB; s_nB = 0; }

  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
  s_epoch++;                  // reset the SXY→obj grid for the next frame
}

void fps60_init(void) {
  g_fps60_on = cfg_on("PSXPORT_FPS60") ? 1 : 0;
  if (g_fps60_on) fprintf(stderr, "[fps60] enabled (rate detect + object join)\n");
}
