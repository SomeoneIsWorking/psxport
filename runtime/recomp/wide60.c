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

// gte_op RTP tap. op 0x01 = RTPS (one new SXY, DR14); 0x30 = RTPT (three, DR12/13/14).
void wide60_rtp(uint32_t op) {
  if (!g_wide60_on) return;
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
  uint8_t  mode, blend;          // texture color mode / semi-transparency mode
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
                     int off_x, int off_y, int tp_x, int tp_y, int mode, int blend,
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
static long s_fence = 0;
void wide60_frame_commit(void) {
  if (!g_wide60_on) return;
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  // Match this frame's prims (B) against the previous frame (A) — the in-between's input.
  wide60_match();

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
