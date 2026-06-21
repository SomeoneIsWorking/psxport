#include "core.h"
#include "game.h"   // Fps60State (per-instance render-interp state) via core->game->fps60
extern "C" {  // Beetle GTE (mednafen gte.c, compiled as C)
  uint32_t GTE_ReadDR(unsigned); uint32_t GTE_ReadCR(unsigned);
  void GTE_WriteDR(unsigned, uint32_t); void GTE_WriteCR(unsigned, uint32_t);
  int32_t GTE_Instruction(uint32_t);
}
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


int g_fps60_on = 0;          // read by the gte_op tap; set by fps60_init from PSXPORT_FPS60

// ---- per-frame projected-geometry fingerprint (rate detector input) -----------------

void Fps60State::fold(uint32_t v) {
  uint64_t h = s_frame_hash;
  for (int i = 0; i < 4; i++) { h ^= (v & 0xFF); h *= 1099511628211ull; v >>= 8; }
  s_frame_hash = h; s_frame_geom++;
}

// ---- SXY → object-id grid (the join) -------------------------------------------------
// Last object that projected a vertex to each (sx,sy). Epoch-stamped so it resets per frame with
// no 2 MB memset: a cell is live only when its stamp == the current epoch.

void Fps60State::grid_put(int sx, int sy, uint32_t obj) {
  int x = sx & (GW - 1), y = sy & (GH - 1);
  int i = y * GW + x;
  s_obj_grid[i] = obj; s_obj_stamp[i] = s_epoch;
}
uint32_t Fps60State::grid_get(int px, int py) {  // ±2px search; returns the source object's node pointer
  for (int dy = -2; dy <= 2; dy++)          // (0 = no object near here / CPU-projected → caller snaps)
    for (int dx = -2; dx <= 2; dx++) {
      int x = (px + dx) & (GW - 1), y = (py + dy) & (GH - 1);
      int i = y * GW + x;
      if (s_obj_stamp[i] == s_epoch && s_obj_grid[i]) return s_obj_grid[i];
    }
  return 0;
}


// ---- native graphical objects (GTE transform groups) ---------------------------------
// The game's camera + models, ported to native: every run of RTPS/RTPT sharing one GTE transform
// (rotation matrix CR0-4 + translation CR5-7 = the model-view, camera baked in) is ONE object. Its
// identity ACROSS frames is its local-vertex fingerprint (the model mesh is invariant; only the
// transform moves). Interpolating each object's transform between frames and re-projecting its verts
// reproduces camera pan + object motion perspective-correctly. (RE: the 0x8007712C cull dispatcher does
// NOT tag these scenes — the GTE transform is the real object identity.)
// Per-frame local-vertex pool for the CURRENT frame (B): each captured vertex's model-space coords and
// the screen XY the GTE produced for it (the key we remap on). Rebuilt every frame.

static void xfold(XObj* o, uint32_t v) {     // fold a local-vertex word into the fingerprint
  o->fp ^= v + 0x9E3779B97F4A7C15ull + (o->fp << 6) + (o->fp >> 2);
}
void Fps60State::xvert(int16_t vx, int16_t vy, int16_t vz, uint32_t sxy) {
  if (s_nv >= XV_MAX) return;
  s_lvx[s_nv] = vx; s_lvy[s_nv] = vy; s_lvz[s_nv] = vz; s_osxy[s_nv] = (int32_t)sxy; s_nv++;
}

// Called per RTPS(0x01)/RTPT(0x30) from fps60_rtp, with the GTE holding this vertex's transform.
void Fps60State::xobj_rtp(uint32_t insn) {
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
void Fps60State::xobj_report() {
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

void Fps60State::xobj_commit() {                 // swap A/B at frame end; reset the per-frame vert pool
  XObj* t = s_xA; s_xA = s_xB; s_xB = t; s_nxA = s_nxB; s_nxB = 0; s_xb_started = 0; s_nv = 0;
}

// ---- transform interpolation + GTE re-projection -------------------------------------
// For each current object (B) matched to a previous one (A) by fingerprint, interpolate the transform
// (rotation matrix + translation at t=0.5) and re-project the object's local verts through the REAL
// Beetle GTE → an old-SXY→new-SXY remap. Rendering the in-between then just remaps B's prim vertices
// through this table (unmapped verts = CPU/2D/unmatched → snap). This is the camera+object motion,
// perspective-correct, with the game's own projection math.

static int s_disp_gate = -1;     // PSXPORT_FPS60_GATE: max screen motion (px) to still interpolate

void Fps60State::xobj_match() {   // by fingerprint + identical vertex count (strong key)
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
void Fps60State::remap_reset() {
  for (int i = 0; i < REMAP_SZ; i++) s_rm_key[i] = (int32_t)0x80000000;
  s_rm_init = 1;
}
void Fps60State::remap_put(int32_t key, int32_t val) {
  uint32_t h = (uint32_t)key * 2654435761u;
  for (int n = 0; n < REMAP_SZ; n++) { int s = (int)((h + n) & (REMAP_SZ - 1));
    if (s_rm_key[s] == (int32_t)0x80000000 || s_rm_key[s] == key) { s_rm_key[s] = key; s_rm_val[s] = val; return; } }
}
int Fps60State::remap_get(int32_t key, int32_t* out) {
  uint32_t h = (uint32_t)key * 2654435761u;
  for (int n = 0; n < REMAP_SZ; n++) { int s = (int)((h + n) & (REMAP_SZ - 1));
    if (s_rm_key[s] == (int32_t)0x80000000) return 0;
    if (s_rm_key[s] == key) { *out = s_rm_val[s]; return 1; } }
  return 0;
}

// Build the remap: interpolate each matched object's transform and re-project its verts through the GTE.
void Fps60State::fps60_build_remap() {
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
void Fps60State::rtp(uint32_t op) {
  if (!g_fps60_on) return;
  s_rtp_calls++; if (current_object) s_rtp_with_obj++;
  xobj_rtp(op);                 // capture this vertex's GTE transform-group (native object)
  unsigned lo = (op == 0x30) ? 12 : 14, hi = 14;
  for (unsigned r = lo; r <= hi; r++) {
    uint32_t sxy = GTE_ReadDR(r);
    fold(sxy);
    int16_t sx = (int16_t)(sxy & 0xFFFF), sy = (int16_t)(sxy >> 16);
    grid_put(sx, sy, current_object);
  }
}

// gp0_exec polygon tap: join the packet's lead vertex to a captured SXY.
void Fps60State::join_poly(int px, int py) {
  if (!g_fps60_on) return;
  if (grid_get(px, py)) s_join_hit++; else s_join_miss++;
}

// ---- full primitive capture: PrimFrame A (prev) / B (current) ------------------------
// Every completed GP0 draw is teed here so the in-between synthesizer (next milestone) can
// re-rasterize a lerped copy of frame B's display list. Coords are stored in PACKET space
// (pre-E5-offset = buffer-relative); the E5 offset at draw time is kept so the synth can apply
// the CURRENT frame's buffer origin and so we can reason in absolute space when needed. Polys
// carry the joined object id (the matcher's primary key); sprites/lines get obj=0 → they snap.


// Full capture for polygons (nv 3/4). xs/ys are packet coords; us/vs/rs/gs/bs per vertex.
void Fps60State::cap_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
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
void Fps60State::cap_sprite(int op, int x, int y, int u, int v, int w, int h,
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
void Fps60State::cap_line(int op, int x0, int y0, int x1, int y1, int r, int g, int b, int semi) {
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
void gpu_fps60_begin_interp(Core*, int, int, int, int, int, int);
void gpu_fps60_end_interp(Core*);
void gpu_fps60_draw_poly(Core*, int, int, const int*, const int*, const int*, const int*,
                       const unsigned char*, const unsigned char*, const unsigned char*,
                       int, int, int, int, int, int, int);
void gpu_fps60_draw_sprite(Core*, int, int, int, int, int, int, int, int, int, int,
                         int, int, int, int, int, int);
void gpu_fps60_draw_line(Core*, int, int, int, int, int, int, int, int);

int Fps60State::fps60_front_off_y() { return s_nB > 0 ? s_pB[0].off_y : 0; }

// ---- per-object screen-centroid motion (interpolation key = the node pointer) --------
// Each captured poly is tagged with its source object's pool-slot pointer (Prim.obj, from the
// cull-dispatcher tap g_current_object). An object's screen motion = the delta of its prim-centroid
// between this frame (B) and the previous (A), matched by that POINTER — a stable engine identity
// (no GTE fingerprints, no screen-XY collisions). The in-between translates each matched object's
// prims to the midpoint (A+B)/2. obj==0 (2D/HUD/CPU-projected) and sprites/lines → snap (30fps).
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
int Fps60State::ocen_delta(uint32_t obj, int* dx, int* dy) {
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
long Fps60State::fps60_synthesize(Core* core) {
  if (s_nB == 0) return 0;
  if (s_sdbg < 0) s_sdbg = cfg_dbg("fps60") ? 1 : 0;
  long d_prims = 0, d_obj_translated = 0, d_snapped = 0, d_tagged = 0;  // sdbg: interpolation outcome
  long moved_count = 0;
  int fy = fps60_front_off_y();
  gpu_fps60_begin_interp(core, 0, fy, 0, fy, 319, fy + 239);     // target=s_interp, clear region, set env
  for (int i = 0; i < s_nB; i++) {
    Prim* B = &s_pB[i];
    int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
    for (int k = 0; k < B->nv; k++) {
      us[k] = B->u[k]; vs[k] = B->v[k]; rs[k] = B->r[k]; gs[k] = B->g[k]; bs[k] = B->b[k];
    }
    // ALL-OR-NOTHING SXY remap (later-83): a poly moves to the interpolated midpoint ONLY if EVERY
    // vertex resolves in the GTE-transform remap table (the whole prim belongs to one matched+
    // interpolated object). Sprites/lines (2D) and any poly with an unresolved vertex (CPU-projected /
    // HUD / spans objects) SNAP unchanged — no partial/mixed remap, so nothing stretches or duplicates.
    // The remap key is the packed SXY (SX low, SY high) the GTE produced = the captured packet coords.
    int moved = 0;
    if (B->nv >= 3) {
      int rx[4], ry[4], all = 1;
      for (int k = 0; k < B->nv; k++) {
        int32_t key = (int32_t)((uint32_t)(uint16_t)B->x[k] | ((uint32_t)(uint16_t)B->y[k] << 16));
        int32_t nsxy;
        if (remap_get(key, &nsxy)) { rx[k] = (int16_t)(nsxy & 0xFFFF); ry[k] = (int16_t)((uint32_t)nsxy >> 16); }
        else { all = 0; break; }
      }
      if (all) { for (int k = 0; k < B->nv; k++) { xs[k] = rx[k]; ys[k] = ry[k]; } moved = 1; }
    }
    if (!moved) for (int k = 0; k < B->nv; k++) { xs[k] = B->x[k]; ys[k] = B->y[k]; }
    if (moved) moved_count++;
    if (s_sdbg) { d_prims++; if (moved) d_obj_translated++; else d_snapped++; if (B->nv >= 3) d_tagged++; }
    if (B->nv == 1)
      gpu_fps60_draw_sprite(core, B->op, xs[0], ys[0], us[0], vs[0], B->w, B->h, rs[0], gs[0], bs[0],
                          B->tp_x, B->tp_y, B->mode, B->blend, B->clut_x, B->clut_y);
    else if (B->nv == 2)
      gpu_fps60_draw_line(core, xs[0], ys[0], xs[1], ys[1], rs[0], gs[0], bs[0], B->blend);
    else
      gpu_fps60_draw_poly(core, B->op, B->nv, xs, ys, us, vs, rs, gs, bs,
                        B->tp_x, B->tp_y, B->mode, B->blend, B->dither, B->clut_x, B->clut_y);
  }
  gpu_fps60_end_interp(core);
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
void gpu_fps60_shot_vram(Core*, int, int, const char*);
void gpu_fps60_shot_interp(Core*, int, int, const char*);
void Fps60State::fps60_synth_dumptest(Core* core) {
  static int tf = -2;
  if (tf == -2) { const char* e = cfg_str("PSXPORT_FPS60_SYNTH"); tf = e ? atoi(e) : -1; }
  if (tf < 0 || s_fence != tf || s_nB == 0) return;
  int fy = fps60_front_off_y(), by = fy ^ 256;
  gpu_fps60_shot_vram(core, 0, by, "scratch/screenshots/fps60_A.ppm");  // previous real frame
  fps60_synthesize(core);                                          // -> s_interp at front origin
  gpu_fps60_shot_interp(core, 0, fy, "scratch/screenshots/fps60_inbetween.ppm");
  gpu_fps60_shot_vram(core, 0, fy, "scratch/screenshots/fps60_B.ppm");  // current real frame
  fprintf(stderr, "[fps60] synth dumptest f%d: A/inbetween/B -> scratch/screenshots/fps60_*.ppm "
          "(front_y=%d back_y=%d, %d prims)\n", tf, fy, by, s_nB);
}

// ---- live 60fps present (windowed): 1 frame behind --------------------------------------
// Per logic frame present TWO display frames: the PREVIOUS real frame (still intact in the back VRAM
// buffer, blitted read-only) then the INTERPOLATED frame (rasterized into s_interp). The current
// frame B is held — it becomes the "previous" (back buffer) next logic frame, so output lags by one
// frame and the displayed stream is A, lerp(A,B), B, lerp(B,C), C… = 60 fps. SAFETY GATE: only when
// windowed + animating + actually double-buffering (front_y flips), else one faithful present of B.


void Fps60State::fps60_present(Core* core) {
  static int win = -1;
  if (win < 0) { const char* w = cfg_str("PSXPORT_GPU_WINDOW"); win = (w && atoi(w) != 0) ? 1 : 0; }
  void gpu_fps60_blit_vram(Core*, int, int); void gpu_fps60_blit_interp(Core*, int, int);
  int fy = fps60_front_off_y();
  int flipped = (fy != s_prev_front_y);
  s_prev_front_y = fy;
  if (win && flipped && s_frame_geom > 0 && s_nB > 0) {
    gpu_fps60_blit_vram(core, 0, fy ^ 256);   // present the previous real frame (lives in the back buffer)
    gpu_pace_subframe(core, 2);
    long moved = fps60_synthesize(core); // interpolated frame -> s_interp (front origin), VRAM untouched
    // The re-rasterized in-between is LOSSY (re-draws only the captured GP0 subset; missing occluders/
    // fills/blend-order make hidden objects reappear → spurious copies). Only trust it when objects were
    // actually interpolated; otherwise present the REAL current frame (no artifacts). Real interpolation
    // needs object-tagged draws from the native renderer (see docs/journal.md later-86/87).
    if (moved > 0) gpu_fps60_blit_interp(core, 0, fy);
    else           gpu_fps60_blit_vram(core, 0, fy);   // real current frame
    gpu_pace_subframe(core, 2);
    gpu_present_ex(core, 0);                // bookkeeping only (watchdog, s_frame++, diagnostics; no blit)
  } else {
    gpu_present_ex(core, 1);               // faithful single present of frame B (front buffer)
    gpu_pace_subframe(core, 1);
  }
}

// ---- logic-rate detector (lrate_proto.c, validated) ---------------------------------

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
void Fps60State::frame_commit(Core* core) {
  if (!g_fps60_on) return;
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  // The live in-between is built by fps60_present_vk → build_lerp (the ACTOR-TRANSFORM reprojection path
  // below). The legacy GTE-SXY remap (xobj_* + fps60_build_remap) and the SW s_interp synth are RETIRED:
  // the remap was keyed on raw GTE DR14 SXY while the live queue carries float-projected screen verts, so
  // it never bridged to the live present (diagnostics only) AND it drove the Beetle GTE per vertex every
  // frame for nothing. Only xobj_commit is kept — it resets the per-frame local-vertex pool that the RTP
  // tap (rtp→xobj_rtp) still appends to, so without it s_nv would grow until XV_MAX and freeze the capture.
  if (s_sdbg < 0) s_sdbg = cfg_dbg("fps60") ? 1 : 0;
  if (s_sdbg) { xobj_match(); xobj_report(); }   // optional cross-frame transform-group diagnostic
  xobj_commit();                                 // reset the per-frame local-vertex pool (RTP-tap hygiene)
  fps60_synth_dumptest(core);   // PSXPORT_FPS60_SYNTH: offline A/in-between/B dump (no live-path change)
  fps60_present_vk(core);       // owns presentation: VK 60fps pair (interpolated in-between + real frame)

  // Swap the prim double-buffer (so s_pB is clean next frame).
  { Prim* t = s_pA; s_pA = s_pB; s_pB = t; s_nA = s_nB; s_nB = 0; }

  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
  s_epoch++;                  // reset the SXY→obj grid for the next frame
}


// ============================================================================================
// VK 60fps: render-queue-snapshot interpolation (the LIVE path; replaces the dead SW s_interp synth).
// Each logic frame the engine render queue (the WHOLE frame — world + 2D, already engine-sorted) is
// captured here (render_queue.cpp's flush snapshots instead of emitting when fps60 is on). We match each
// current world prim to the previous frame's by a position-INDEPENDENT material/UV/color fingerprint
// (nearest centroid as tiebreak), lerp matched prims' screen verts + depth to the A/B midpoint, render
// the in-between THROUGH the normal VK emit path + present it, then render+present the real frame — a
// 60fps stream (in-between, real, ...). Static prims (terrain/HUD) have zero motion -> unchanged; moving
// objects + camera-panned geometry interpolate; teleports/cuts exceed the gate -> snap (no smear).
#include "render_queue.h"
#include "game.h"
#include <unordered_map>
#include <math.h>
void gpu_present_ex(Core* core, int do_blit);
void gpu_fps60_present_pass(Core* core);
void gpu_pace_subframe(Core* core, int n);
// fps60 midpoint reprojection: project model verts through an explicit composed transform (gte_beetle.cpp).
void proj_native_xform_cr(const uint32_t cr[11], const int16_t mv[4][3], int nv, float px[4], float py[4], float pz[4]);
float proj_pz_to_ord(float pz);

#define FPS60_RQ_MAX 16384

// ---- ACTOR-TRANSFORM 60fps tier (user spec 2026-06-21) -------------------------------------------------
// The interpolated in-between is built by REPROJECTING each captured world prim under the A/B MIDPOINT of
// its source actor's transform — NOT by matching screen prims (the retired fingerprint matcher exploded on
// real meshes) and NOT by re-running any guest/interpreted render code (unsafe: the field's per-mode
// renderers mutate guest packet RAM). At native projection (fps60_stamp_world) every GTE-composed world
// quad records its MODEL verts, its composed transform CR0-7, and its actor key (the per-object render
// command). The composed transform already bakes in the camera, so lerping it per actor reproduces BOTH
// camera pan (a static actor's transform differs frame-to-frame only by the camera) AND object motion in
// one mechanism — exactly the user's "static objects move via the interpolated camera only; movers also
// get object-motion interp", with the per-actor transform delta as the static/mover signal (no separate
// flag). Terrain (separate float projection) + 2D/HUD carry fps_world=0 and snap (documented follow-up).

// Capture hook: stamp the just-pushed world RqItem with this prim's reprojection inputs. Called by the GTE
// submitters (engine_submit.cpp) right after gpu_draw_world_quad. No-op unless fps60 is on.
void fps60_stamp_world(Core* c, const int16_t mv[4][3], int nv, uint32_t key) {
  RenderQueue& q = c->game->rq;
  if (q.consumed || q.n == 0) return;                 // the world quad wasn't actually queued
  RqItem* it = &q.items[q.n - 1];
  if (it->layer != RQ_WORLD) return;
  it->fps_world = 1; it->fps_key = key;
  for (int i = 0; i < 8; i++) it->fps_cr[i] = GTE_ReadCR(i);   // composed camera×object transform CR0-7
  it->fps_cr[8] = GTE_ReadCR(24); it->fps_cr[9] = GTE_ReadCR(25); it->fps_cr[10] = GTE_ReadCR(26); // OFX/OFY/H
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    it->fps_mv[k][0] = mv[s][0]; it->fps_mv[k][1] = mv[s][1]; it->fps_mv[k][2] = mv[s][2]; }
  it->fps_offx = (int16_t)c->game->gpu.s_off_x;       // draw offset baked into xs/ys (reproject reproduces)
  it->fps_offy = (int16_t)c->game->gpu.s_off_y;
}

static int s_lerpdbg = -1;        // PSXPORT_DEBUG=fps60 — per-frame reproject stats

void Fps60State::rq_capture(const RqItem* items, int n) {
  if (!s_rqCur) s_rqCur = new RqItem[FPS60_RQ_MAX];
  if (n > FPS60_RQ_MAX) n = FPS60_RQ_MAX;
  if (n > 0) memcpy(s_rqCur, items, (size_t)n * sizeof(RqItem));
  s_nCur = n;
}

// Max per-VERTEX screen motion (px, L1) we will treat as "the same vertex moved a little" and lerp.
// This is the load-bearing robustness gate, not the centroid one. Justification: Tomba2 logic runs at
// 30fps and the in-between is the t=0.5 midpoint, so a vertex's true frame-to-frame motion is at most
// ~half the on-screen speed of the fastest thing we interpolate. World geometry (camera pan + model
// motion) moves at most a few tens of px/frame at this rate; 48px L1 comfortably covers genuine motion
// while rejecting the cross-vertex pairings that cause the explosion (a mis-paired vertex jumps a large
// fraction of the screen). A vertex whose paired displacement exceeds this is NOT the same vertex (wrong
// fingerprint collision, permuted winding, or a real teleport/cut) -> we must not average it.
#define FPS60_VTX_GATE 48

// Reproject one captured world prim under crM (its actor's A/B-midpoint composed transform) into `out`.
// Returns the worst per-vertex L1 screen displacement from the prim's real (frame-B) position — the caller
// snaps instead when this exceeds the gate (teleport/cut/bad match → no smear). Reproduces the exact
// round+offset gpu_draw_world_quad applied, so a crM == fps_cr (t=1.0) reprojection is bit-faithful.
static int fps60_reproject(const RqItem* C, const uint32_t crM[8], RqItem* out) {
  float px[4], py[4], pz[4];
  proj_native_xform_cr(crM, C->fps_mv, C->nv, px, py, pz);
  int worst = 0;
  for (int k = 0; k < C->nv; k++) {
    int xs = (int)(px[k] < 0 ? px[k] - 0.5f : px[k] + 0.5f) + C->fps_offx;
    int ys = (int)(py[k] < 0 ? py[k] - 0.5f : py[k] + 0.5f) + C->fps_offy;
    int d = abs(xs - C->xs[k]) + abs(ys - C->ys[k]);
    if (d > worst) worst = d;
    out->xs[k] = xs; out->ys[k] = ys; out->depth[k] = proj_pz_to_ord(pz[k]);
    // Vertex smoothing (#15): also carry the SUB-PIXEL reprojected XY so the 60fps in-between is smooth
    // too (the gate above still uses the integer position). has_xyf came along in the *C copy.
    out->xsf[k] = px[k] + (float)C->fps_offx;
    out->ysf[k] = py[k] + (float)C->fps_offy;
  }
  return worst;
}

// Midpoint of an actor's composed transform: rotation CR0-4 = packed-int16 average, translation CR5-7 =
// integer average. (Small-angle matrix lerp; refine to euler-slerp if fast spins warp — see journal.)
static void fps60_compose_mid(const uint32_t a[11], const uint32_t b[11], uint32_t m[11]) {
  for (int k = 0; k < 5; k++) m[k] = interp_packed(a[k], b[k]);                 // rotation CR0-4 (packed i16)
  for (int k = 5; k < 8; k++) m[k] = (uint32_t)(int32_t)(((int64_t)(int32_t)a[k] + (int32_t)b[k]) / 2);  // trans
  for (int k = 8; k < 11; k++) m[k] = (uint32_t)(int32_t)(((int64_t)(int32_t)a[k] + (int32_t)b[k]) / 2);  // OFX/OFY/H
}

// Build the interpolated in-between by reprojecting each world prim at its actor's transform midpoint.
// A static actor's transform differs across frames only by the camera → it reprojects under the half-camera
// (correct pan); a mover's also by its own motion → midpoint pose. 2D/HUD, terrain, unkeyed or new-this-
// frame actors, and any prim whose reprojection jumps more than the gate (cut/teleport) SNAP unchanged.
int Fps60State::build_lerp() {
  if (!s_rqLerp) s_rqLerp = new RqItem[FPS60_RQ_MAX];
  // actor key -> its composed CR0-7 LAST frame (all of an actor's prims share one composed transform, so
  // first occurrence wins). Points into s_rqPrev (stable for this call).
  std::unordered_map<uint32_t, const uint32_t*> prevcr;
  prevcr.reserve((size_t)s_nPrev * 2 + 16);
  for (int j = 0; j < s_nPrev; j++) { const RqItem* P = &s_rqPrev[j];
    if (P->fps_world && P->fps_key) prevcr.emplace(P->fps_key, P->fps_cr); }
  long moved = 0, snapped = 0;
  for (int i = 0; i < s_nCur; i++) {
    const RqItem* C = &s_rqCur[i];
    s_rqLerp[i] = *C;                                     // default: SNAP (real frame-B position)
    if (!C->fps_world || !C->fps_key) { snapped++; continue; }   // 2D/HUD/terrain/unkeyed → snap
    auto it = prevcr.find(C->fps_key);
    if (it == prevcr.end()) { snapped++; continue; }     // actor is new this frame (spawn/teleport) → snap
    uint32_t crM[11]; fps60_compose_mid(it->second, C->fps_cr, crM);
    RqItem tmp = *C;
    int worst = fps60_reproject(C, crM, &tmp);
    if (worst > FPS60_VTX_GATE) { snapped++; continue; } // cut/teleport/degenerate → snap (no smear)
    for (int k = 0; k < C->nv; k++) { s_rqLerp[i].xs[k] = tmp.xs[k]; s_rqLerp[i].ys[k] = tmp.ys[k];
                                      s_rqLerp[i].xsf[k] = tmp.xsf[k]; s_rqLerp[i].ysf[k] = tmp.ysf[k];
                                      s_rqLerp[i].depth[k] = tmp.depth[k]; }
    moved++;
  }
  if (s_lerpdbg < 0) s_lerpdbg = cfg_dbg("fps60") ? 1 : 0;
  if (s_lerpdbg) fprintf(stderr, "[fps60] f%ld reproject: prims=%d moved=%ld snapped=%ld actors=%zu\n",
                         s_fence, s_nCur, moved, snapped, prevcr.size());
  // MECHANICAL GATE (PSXPORT_DEBUG=fps60chk): reproject every world prim at t=1.0 (crM = its OWN captured
  // composed transform, no averaging) — this MUST reproduce the prim's real screen verts. A non-zero error
  // means the capture/recompose/round path is wrong (not a smoothness issue). Pure diagnostic, no output change.
  static int s_chk = -1; if (s_chk < 0) s_chk = cfg_dbg("fps60chk") ? 1 : 0;
  if (s_chk) {
    long n = 0, maxe = 0; double sume = 0;
    for (int i = 0; i < s_nCur; i++) { const RqItem* C = &s_rqCur[i];
      if (!C->fps_world || !C->fps_key) continue;
      RqItem tmp = *C; int e = fps60_reproject(C, C->fps_cr, &tmp);
      n++; sume += e; if (e > maxe) maxe = e;
    }
    fprintf(stderr, "[fps60chk] f%ld world=%ld  t=1.0 reproject error: max=%ld avg=%.3f px (0 = exact)\n",
            s_fence, n, maxe, n ? sume / n : 0.0);
  }
  return s_nCur;
}

void gpu_vk_shot(Core* core, const char* path);   // diagnostic: dump the CURRENT s_tex (the just-presented frame)
void Fps60State::fps60_present_vk(Core* core) {
  int nl = (s_have_prev && s_nCur > 0) ? build_lerp() : 0;
  if (nl > 0) {                                           // PASS 1 — the interpolated in-between
    for (int i = 0; i < nl; i++) gpu_emit_rq_item(core, &s_rqLerp[i]);
    gpu_fps60_present_pass(core);                         // show it + reset the VK batch (no s_frame++)
    // PSXPORT_FPS60_INTERPSHOT=path — one-shot: dump the INTERPOLATED in-between's s_tex (it persists until
    // the real pass overwrites it) so the 60fps in-between (mover at midpoint, shadow/SSAO/2D from the real
    // composite) can be eyeballed in isolation. Pure diagnostic; armed once, then disarmed.
    // PSXPORT_FPS60_INTERPSHOT="path[:fence]" — dump at logic-fence `fence` (default: the first interp frame).
    { static int armed = -1, tfence = -1; static char path[256];
      if (armed < 0) { const char* e = cfg_str("PSXPORT_FPS60_INTERPSHOT");
        armed = (e && *e) ? 1 : 0;
        if (armed) { const char* col = strrchr(e, ':');
          if (col) { tfence = atoi(col + 1); snprintf(path, sizeof path, "%.*s", (int)(col - e), e); }
          else snprintf(path, sizeof path, "%s", e); } }
      if (armed == 1 && (tfence < 0 || s_fence >= tfence)) { gpu_vk_shot(core, path); armed = 2;
        fprintf(stderr, "[fps60] interp-frame shot (f%ld) -> %s\n", s_fence, path); } }
    gpu_pace_subframe(core, 2);
  }
  for (int i = 0; i < s_nCur; i++) gpu_emit_rq_item(core, &s_rqCur[i]);   // PASS 2 — the real frame
  gpu_present_ex(core, 1);                                // present + per-logic-frame bookkeeping
  gpu_pace_subframe(core, nl > 0 ? 2 : 1);
  if (!s_rqPrev) s_rqPrev = new RqItem[FPS60_RQ_MAX];     // current -> previous for next frame
  if (s_nCur > 0) memcpy(s_rqPrev, s_rqCur, (size_t)s_nCur * sizeof(RqItem));
  s_nPrev = s_nCur; s_have_prev = 1;
}

// ---- Public capture API: thin free-function wrappers over the per-instance Fps60State methods.
// Keep the C-style call sites stable; each forwards to core->game->fps60 (de-globalization, 2026-06-19). ----
void fps60_rtp(Core* core, uint32_t op) { core->game->fps60.rtp(op); }
void fps60_join_poly(Core* core, int px, int py) { core->game->fps60.join_poly(px, py); }
void fps60_cap_poly(Core* core, int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                    const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                    int off_x, int off_y, int tp_x, int tp_y, int mode, int blend, int dither,
                    int clut_x, int clut_y) {
  core->game->fps60.cap_poly(op, nv, xs, ys, us, vs, rs, gs, bs, off_x, off_y, tp_x, tp_y, mode, blend, dither, clut_x, clut_y);
}
void fps60_cap_sprite(Core* core, int op, int x, int y, int u, int v, int w, int h,
                      int r, int g, int b, int off_x, int off_y,
                      int tp_x, int tp_y, int mode, int blend, int clut_x, int clut_y) {
  core->game->fps60.cap_sprite(op, x, y, u, v, w, h, r, g, b, off_x, off_y, tp_x, tp_y, mode, blend, clut_x, clut_y);
}
void fps60_cap_line(Core* core, int op, int x0, int y0, int x1, int y1, int r, int g, int b, int semi) {
  core->game->fps60.cap_line(op, x0, y0, x1, y1, r, g, b, semi);
}
void fps60_frame_commit(Core* core) { core->game->fps60.frame_commit(core); }

void fps60_init(void) {
  // 60fps is toggled in the F1 overlay (persisted to psxport_settings.ini via mods); g_fps60_on is loaded
  // by mods_init BEFORE this runs. NO env gate (user directive): do not read PSXPORT_FPS60 — that would
  // clobber the persisted overlay setting. Just report the loaded state.
  if (g_fps60_on) fprintf(stderr, "[fps60] interpolated 60fps ON (overlay)\n");
}
