// wide60 — interpolated-60fps tier for the native PC port (design: docs/wide60_recomp_60fps.md).
//
// This file owns the capture buffers, the logic-rate detector, the object→primitive join, and
// (later) the matcher + in-between synthesizer. GATED behind PSXPORT_WIDE60 and purely additive:
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

uint8_t  mem_r8(uint32_t);
uint32_t GTE_ReadDR(unsigned which);   // Beetle GTE data regs (SXY-FIFO = DR12/13/14)

int g_wide60_on = 0;          // read by the gte_op tap; set by wide60_init from PSXPORT_WIDE60
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
static uint32_t grid_get(int px, int py) {  // ±2px search; returns obj (0 = no join)
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int x = (px + dx) & (GW - 1), y = (py + dy) & (GH - 1);
      int i = y * GW + x;
      if (s_obj_stamp[i] == s_epoch) return s_obj_grid[i] ? s_obj_grid[i] : 0xFFFFFFFFu /*tagged@obj0*/;
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
typedef struct {
  uint32_t r0, r1, r2, r3, r4;     // rotation matrix, GTE control regs CR0..4 (packed int16 pairs)
  int32_t  trx, try_, trz;         // translation, CR5..7
  uint64_t fp;                     // local-vertex fingerprint = cross-frame identity
  long     nrtps;                  // RTPS/RTPT count (object size)
} XObj;
static XObj s_xa[XOBJ_MAX], s_xb[XOBJ_MAX];
static XObj* s_xA = s_xa;          // previous frame's objects
static XObj* s_xB = s_xb;          // current frame's objects (capturing)
static int   s_nxA = 0, s_nxB = 0;
static int   s_xb_started = 0;     // a group is open in s_xB

static void xfold(XObj* o, uint32_t v) {     // fold a local-vertex word into the fingerprint
  o->fp ^= v + 0x9E3779B97F4A7C15ull + (o->fp << 6) + (o->fp >> 2);
}

// Called per RTPS(0x01)/RTPT(0x30) from wide60_rtp, with the GTE holding this vertex's transform.
static void xobj_rtp(uint32_t op) {
  uint32_t r0 = GTE_ReadCR(0), r1 = GTE_ReadCR(1), r2 = GTE_ReadCR(2), r3 = GTE_ReadCR(3), r4 = GTE_ReadCR(4);
  int32_t  t5 = (int32_t)GTE_ReadCR(5), t6 = (int32_t)GTE_ReadCR(6), t7 = (int32_t)GTE_ReadCR(7);
  XObj* o = s_xb_started ? &s_xB[s_nxB - 1] : NULL;
  int same = o && o->r0==r0 && o->r1==r1 && o->r2==r2 && o->r3==r3 && o->r4==r4
               && o->trx==t5 && o->try_==t6 && o->trz==t7;
  if (!same) {                                  // transform changed → new object
    if (s_nxB >= XOBJ_MAX) return;
    o = &s_xB[s_nxB++]; s_xb_started = 1;
    o->r0=r0; o->r1=r1; o->r2=r2; o->r3=r3; o->r4=r4; o->trx=t5; o->try_=t6; o->trz=t7;
    o->fp = 1469598103934665603ull; o->nrtps = 0;
  }
  // Fold the LOCAL input vertices (DR0/1 for RTPS; DR0..5 for RTPT) — model-space, frame-invariant.
  xfold(o, GTE_ReadDR(0)); xfold(o, GTE_ReadDR(1) & 0xFFFF);
  if (op == 0x30) { xfold(o, GTE_ReadDR(2)); xfold(o, GTE_ReadDR(3) & 0xFFFF);
                    xfold(o, GTE_ReadDR(4)); xfold(o, GTE_ReadDR(5) & 0xFFFF); }
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

static void xobj_commit(void) {                 // swap A/B at frame end
  XObj* t = s_xA; s_xA = s_xB; s_xB = t; s_nxA = s_nxB; s_nxB = 0; s_xb_started = 0;
}

// gte_op RTP tap. op 0x01 = RTPS (one new SXY, DR14); 0x30 = RTPT (three, DR12/13/14).
void wide60_rtp(uint32_t op) {
  if (!g_wide60_on) return;
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
void wide60_join_poly(int px, int py) {
  if (!g_wide60_on) return;
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
void wide60_cap_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                     const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                     int off_x, int off_y, int tp_x, int tp_y, int mode, int blend, int dither,
                     int clut_x, int clut_y) {
  if (!g_wide60_on) return;
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
void wide60_cap_sprite(int op, int x, int y, int u, int v, int w, int h,
                       int r, int g, int b, int off_x, int off_y,
                       int tp_x, int tp_y, int mode, int blend, int clut_x, int clut_y) {
  if (!g_wide60_on) return;
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
void wide60_cap_line(int op, int x0, int y0, int x1, int y1, int r, int g, int b, int semi) {
  if (!g_wide60_on) return;
  if (s_nB >= PRIM_MAX) { s_overflow = 1; return; }
  Prim* P = &s_pB[s_nB++];
  P->op = (uint8_t)op; P->nv = 2;
  P->x[0] = (int16_t)x0; P->y[0] = (int16_t)y0; P->x[1] = (int16_t)x1; P->y[1] = (int16_t)y1;
  P->r[0] = (uint8_t)r; P->g[0] = (uint8_t)g; P->b[0] = (uint8_t)b;
  P->blend = (uint8_t)semi;           // reuse `blend` to carry the segment's semi-transparency flag
  P->obj = 0;
}

// ---- the matcher: B-prim → A-prim by object id + draw-state token --------------------
// Primary key = obj (the GTE-tagged object pointer, stable across logic frames). Within an
// object, several prims can share the object; disambiguate by the draw-state token (op, texpage,
// color mode, CLUT) AND draw order — the i-th occurrence of a token in B pairs with the i-th in
// A, because both frames emit an object's prims in the same deterministic cull order. The "first
// unused A prim with an equal full key, scanned in A draw order" rule realizes that ordering.
// obj==0 (sprites/HUD/terrain) never matches → snap. Result: s_match[i] = A index or -1.
static int  s_match[PRIM_MAX];
static long s_m_matched, s_m_snap, s_m_total;   // accumulated over the report window
static long s_disp_hist[16];                     // |Δ| of matched pairs, bucketed (px), for sanity

// Key = obj + op + texpage + color-mode + CLUT. Anchored on the GTE-tagged object id, the texpage
// and CLUT are a strong DISAMBIGUATOR between the many prims of one object (a character mesh = dozens
// of tris): the greedy "first unused A prim with equal key, in A draw order" scan then pairs the k-th
// same-key prim in B with the k-th in A. Measured: this gives median cross-frame displacement of only
// 2–9 px (= genuinely the same prim a frame apart). Dropping texpage/CLUT and relying on draw-order
// ordinal alone was tried and is WORSE (median jumps to 15+ px — ordinals shift with per-tri LOD/cull),
// so the richer key earns its keep here. (The classic "CLUT high halves alternate by parity, don't key
// on them" warning applied to the OBJECT-LESS offline fingerprint matcher; with obj as the anchor the
// texpage/CLUT refine rather than collapse the match — verified by the displacement histogram.)
static int prim_key_eq(const Prim* a, const Prim* b) {
  return a->obj == b->obj && a->op == b->op && a->tp_x == b->tp_x && a->tp_y == b->tp_y &&
         a->mode == b->mode && a->clut_x == b->clut_x && a->clut_y == b->clut_y;
}

static void wide60_match(void) {
  static uint8_t used[PRIM_MAX];
  for (int j = 0; j < s_nA; j++) used[j] = 0;
  for (int i = 0; i < s_nB; i++) {
    s_match[i] = -1;
    Prim* B = &s_pB[i];
    s_m_total++;
    if (B->obj == 0) { s_m_snap++; continue; }     // unjoined → snap, never lerp
    for (int j = 0; j < s_nA; j++) {
      if (used[j]) continue;
      if (prim_key_eq(B, &s_pA[j])) { s_match[i] = j; used[j] = 1; break; }
    }
    if (s_match[i] < 0) { s_m_snap++; continue; }
    s_m_matched++;
    Prim* A = &s_pA[s_match[i]];
    int d = abs(B->x[0] - A->x[0]) + abs(B->y[0] - A->y[0]);   // L1 displacement (packet space)
    int bk = d; if (bk > 15) bk = 15;
    s_disp_hist[bk]++;
  }
}

// ---- in-between synthesizer ----------------------------------------------------------
// Re-rasterize a lerped copy of frame B's captured display list into the BACK buffer (the VRAM
// framebuffer NOT currently shown — it holds A, already presented, so overwriting it is safe). For
// each prim: matched + within the displacement gate → lerp its screen vertices at t=0.5 (UV/color/
// state kept from B to avoid UV-parity smear); everything else (sprites/HUD/terrain, unmatched polys,
// teleports) → drawn at B's position (snap). This redraws the WHOLE list, so background/HUD are
// reproduced — no holes, no clear needed. The two framebuffers sit at y=0 and y=256 (320x240 each),
// so the back origin is just the front's y flipped by 256.
void gpu_w60_begin_interp(int, int, int, int, int, int);
void gpu_w60_end_interp(void);
void gpu_w60_draw_poly(int, int, const int*, const int*, const int*, const int*,
                       const unsigned char*, const unsigned char*, const unsigned char*,
                       int, int, int, int, int, int, int);
void gpu_w60_draw_sprite(int, int, int, int, int, int, int, int, int, int,
                         int, int, int, int, int, int);
void gpu_w60_draw_line(int, int, int, int, int, int, int, int);

static int s_disp_gate = -1;   // PSXPORT_WIDE60_GATE: max L1 motion (px) of vertex 0 to still lerp
static int s_shape_gate = -1;  // PSXPORT_WIDE60_SHAPE: max per-vertex deviation from rigid motion

static int w60_front_off_y(void) { return s_nB > 0 ? s_pB[0].off_y : 0; }

// ---- per-OBJECT lerp decision --------------------------------------------------------
// An object interpolates only if EVERY one of its prims is matched AND moves rigidly (vertex-0 within
// the position gate, other vertices within the shape gate of vertex-0's delta). Otherwise the WHOLE
// object snaps to B. Deciding per-object (not per-prim) keeps an object's adjacent surfaces consistent
// — a per-prim choice would lerp one triangle and snap its neighbour, opening a thin gap between them.
#define OBJH 4096
static uint32_t s_oh_key[OBJH];
static uint8_t  s_oh_ok[OBJH];
static int      s_lerp[PRIM_MAX];     // final per-prim decision (1 = lerp, 0 = snap)

static int oh_find(uint32_t obj) {    // open-addressing slot for obj (0 = empty); inserts if absent
  uint32_t h = obj * 2654435761u;
  for (int n = 0; n < OBJH; n++) { int s = (int)((h + n) & (OBJH - 1));
    if (s_oh_key[s] == 0) { s_oh_key[s] = obj; s_oh_ok[s] = 1; return s; }
    if (s_oh_key[s] == obj) return s; }
  return -1;
}
static int prim_rigid(const Prim* A, const Prim* B) {
  int dx0 = (int)B->x[0] - A->x[0], dy0 = (int)B->y[0] - A->y[0];
  if (abs(dx0) + abs(dy0) > s_disp_gate) return 0;
  for (int k = 1; k < B->nv; k++) {
    int dxk = (int)B->x[k] - A->x[k], dyk = (int)B->y[k] - A->y[k];
    if (abs(dxk - dx0) + abs(dyk - dy0) > s_shape_gate) return 0;
  }
  return 1;
}
static void wide60_decide(void) {
  if (s_disp_gate  < 0) { const char* g = getenv("PSXPORT_WIDE60_GATE");  s_disp_gate  = g ? atoi(g) : 48; }
  if (s_shape_gate < 0) { const char* g = getenv("PSXPORT_WIDE60_SHAPE"); s_shape_gate = g ? atoi(g) : 6; }
  memset(s_oh_key, 0, sizeof s_oh_key);
  for (int i = 0; i < s_nB; i++) {                  // aggregate readiness per object
    Prim* B = &s_pB[i];
    if (B->obj == 0) continue;
    int s = oh_find(B->obj); if (s < 0) continue;
    int ready = (s_match[i] >= 0) && prim_rigid(&s_pA[s_match[i]], B);
    if (!ready) s_oh_ok[s] = 0;
  }
  for (int i = 0; i < s_nB; i++) {                  // a prim lerps iff its object is all-ready
    Prim* B = &s_pB[i];
    s_lerp[i] = 0;
    if (B->obj == 0 || s_match[i] < 0) continue;
    int s = oh_find(B->obj);
    s_lerp[i] = (s >= 0 && s_oh_ok[s]);
  }
}

// Rasterize the interpolated frame (lerp A→B at t=0.5) into the SEPARATE s_interp buffer at the FRONT
// framebuffer origin — the 60fps layer sits ON TOP of the renderer and NEVER writes the game's VRAM.
// The whole captured list is redrawn over a cleared region (background + HUD reproduced, no holes);
// matched prims within the displacement gate lerp their screen vertices, everything else snaps to B.
// Textures are read from VRAM (live atlas). Call before the A/B swap (uses s_pB=B, s_pA=A, s_match).
static void wide60_synthesize(void) {
  if (s_nB == 0) return;
  int fy = w60_front_off_y();
  gpu_w60_begin_interp(0, fy, 0, fy, 319, fy + 239);     // target=s_interp, clear region, set env
  for (int i = 0; i < s_nB; i++) {
    Prim* B = &s_pB[i];
    int lerp = s_lerp[i];                                 // per-OBJECT decision (wide60_decide)
    Prim* A = lerp ? &s_pA[s_match[i]] : NULL;
    int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
    for (int k = 0; k < B->nv; k++) {
      xs[k] = lerp ? ((int)A->x[k] + B->x[k]) / 2 : B->x[k];   // t=0.5 screen-XY lerp (buffer-relative)
      ys[k] = lerp ? ((int)A->y[k] + B->y[k]) / 2 : B->y[k];
      us[k] = B->u[k]; vs[k] = B->v[k]; rs[k] = B->r[k]; gs[k] = B->g[k]; bs[k] = B->b[k];
    }
    if (B->nv == 1)                                      // sprite/rect (snaps; obj 0)
      gpu_w60_draw_sprite(B->op, xs[0], ys[0], us[0], vs[0], B->w, B->h, rs[0], gs[0], bs[0],
                          B->tp_x, B->tp_y, B->mode, B->blend, B->clut_x, B->clut_y);
    else if (B->nv == 2)                                  // line segment (snaps; obj 0)
      gpu_w60_draw_line(xs[0], ys[0], xs[1], ys[1], rs[0], gs[0], bs[0], B->blend);
    else
      gpu_w60_draw_poly(B->op, B->nv, xs, ys, us, vs, rs, gs, bs,
                        B->tp_x, B->tp_y, B->mode, B->blend, B->dither, B->clut_x, B->clut_y);
  }
  gpu_w60_end_interp();
}

// PSXPORT_WIDE60_SYNTH=frame — headless validation: at logic fence `frame`, dump A (prev real frame =
// back buffer), the synthesized interpolated frame (from s_interp), and B (front buffer), so the
// interpolation can be eyeballed (objects at intermediate positions) WITHOUT the live present path.
void gpu_w60_shot_vram(int, int, const char*);
void gpu_w60_shot_interp(int, int, const char*);
static void wide60_synth_dumptest(void) {
  static int tf = -2;
  if (tf == -2) { const char* e = getenv("PSXPORT_WIDE60_SYNTH"); tf = e ? atoi(e) : -1; }
  if (tf < 0 || s_fence != tf || s_nB == 0) return;
  int fy = w60_front_off_y(), by = fy ^ 256;
  gpu_w60_shot_vram(0, by, "scratch/screenshots/w60_A.ppm");        // previous real frame
  wide60_synthesize();                                              // -> s_interp at front origin
  gpu_w60_shot_interp(0, fy, "scratch/screenshots/w60_inbetween.ppm");
  gpu_w60_shot_vram(0, fy, "scratch/screenshots/w60_B.ppm");        // current real frame
  fprintf(stderr, "[wide60] synth dumptest f%d: A/inbetween/B -> scratch/screenshots/w60_*.ppm "
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

static void wide60_present(void) {
  static int win = -1;
  if (win < 0) { const char* w = getenv("PSXPORT_GPU_WINDOW"); win = (w && atoi(w) != 0) ? 1 : 0; }
  void gpu_w60_blit_vram(int, int); void gpu_w60_blit_interp(int, int);
  int fy = w60_front_off_y();
  int flipped = (fy != s_prev_front_y);
  s_prev_front_y = fy;
  if (win && flipped && s_frame_geom > 0 && s_nB > 0) {
    gpu_w60_blit_vram(0, fy ^ 256);   // present the previous real frame (lives in the back buffer)
    gpu_pace_subframe(2);
    wide60_synthesize();              // interpolated frame -> s_interp (front origin), VRAM untouched
    gpu_w60_blit_interp(0, fy);
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
void wide60_frame_commit(void) {
  if (!g_wide60_on) return;
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  // Native graphical objects (GTE transform groups): report cross-frame matching every 500 frames.
  if ((s_fence % 500) == 0) xobj_report();
  xobj_commit();

  // Match this frame's prims (B) against the previous frame (A), then decide per-object lerp/snap.
  wide60_match();
  wide60_decide();
  wide60_synth_dumptest();   // PSXPORT_WIDE60_SYNTH: offline A/in-between/B dump (no live-path change)
  wide60_present();          // owns presentation: 60fps pair (prev + interpolated) or faithful single

  if ((s_fence % 500) == 0) {
    int quota = mem_r8(0x1F800235u);
    double content_fps = quota > 0 ? 60.0 / (quota * s_rd.period) : 0.0;
    long jt = s_join_hit + s_join_miss;
    long mt = s_m_matched + s_m_snap;
    // displacement: report the median bucket and the 90th-pct bucket of matched pairs
    long acc = 0, med = -1, p90 = -1, tot = 0;
    for (int k = 0; k < 16; k++) tot += s_disp_hist[k];
    for (int k = 0; k < 16; k++) { acc += s_disp_hist[k];
      if (med < 0 && acc * 2 >= tot) med = k;
      if (p90 < 0 && acc * 10 >= tot * 9) p90 = k; }
    fprintf(stderr, "[wide60] f%ld  period=%d quota=%d => ~%.1f fps  |  poly-join: %ld/%ld (%.1f%%)  "
            "|  match: %ld/%ld lerp (%.1f%%), %ld snap  |  Δpx med=%ld p90=%ld%s\n",
            s_fence, s_rd.period, quota, content_fps, s_join_hit, jt,
            jt ? 100.0 * s_join_hit / jt : 0.0,
            s_m_matched, mt, mt ? 100.0 * s_m_matched / mt : 0.0, s_m_snap,
            med, p90, s_overflow ? "  [PRIM OVERFLOW]" : "");
    s_join_hit = s_join_miss = 0;
    s_m_matched = s_m_snap = s_m_total = 0;
    for (int k = 0; k < 16; k++) s_disp_hist[k] = 0;
    s_overflow = 0;
  }

  // Swap PrimFrames: B (just captured) becomes A (prev) for the next frame; reuse the old A buffer.
  { Prim* t = s_pA; s_pA = s_pB; s_pB = t; s_nA = s_nB; s_nB = 0; }

  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
  s_epoch++;                  // reset the SXY→obj grid for the next frame
}

void wide60_init(void) {
  g_wide60_on = getenv("PSXPORT_WIDE60") ? 1 : 0;
  if (g_wide60_on) fprintf(stderr, "[wide60] enabled (rate detect + object join)\n");
}
