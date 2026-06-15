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

  if ((s_fence % 500) == 0) {
    int quota = mem_r8(0x1F800235u);
    double content_fps = quota > 0 ? 60.0 / (quota * s_rd.period) : 0.0;
    long jt = s_join_hit + s_join_miss;
    fprintf(stderr, "[wide60] f%ld  period=%d quota=%d => ~%.1f fps  |  poly-join: %ld/%ld (%.1f%% "
            "object-tagged 3D; rest=terrain/HUD→snap)  (geom_last=%ld)\n",
            s_fence, s_rd.period, quota, content_fps, s_join_hit, jt,
            jt ? 100.0 * s_join_hit / jt : 0.0, s_frame_geom);
    s_join_hit = s_join_miss = 0;
  }
  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
  s_epoch++;                  // reset the SXY→obj grid for the next frame
}

void wide60_init(void) {
  g_wide60_on = getenv("PSXPORT_WIDE60") ? 1 : 0;
  if (g_wide60_on) fprintf(stderr, "[wide60] enabled (rate detect + object join)\n");
}
