// wide60 — interpolated-60fps tier for the native PC port (design: docs/wide60_recomp_60fps.md).
//
// This file owns the capture buffers, the logic-rate detector, and (later) the primitive matcher
// and in-between synthesizer. It is GATED behind PSXPORT_WIDE60 and is purely additive: when off,
// the taps in gte_beetle.c / gpu_native.c / games_tomba2.c are no-ops and the faithful 4:3/30fps
// path is byte-identical.
//
// MILESTONE 1 (this commit): measure the game's real logic rate. The project never verified
// Tomba2's framerate ("believed 30 fps — measure"). The detector hashes each logic frame's
// projected-geometry set (the GTE's SXY output — pure logic state, no GPU double-buffer parity)
// and counts how many consecutive ov_frame_update() calls produced an identical set. The modal
// run-length is the content-change period; combined with the engine's vblank quota (DAT_1f800235,
// vblanks per displayed frame) it yields the real logic rate and the in-between count needed for 60.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

uint8_t mem_r8(uint32_t);

int g_wide60_on = 0;   // read by the gte_op tap; set by wide60_init from PSXPORT_WIDE60

// ---- per-frame projected-geometry fingerprint ---------------------------------------
// Folded incrementally as the GTE projects (gte_op RTPS/RTPT tap -> wide60_geom_xy), finalized at
// the frame fence. FNV-1a over the SXY stream; order-sensitive, which is fine (draw order is
// stable per logic state). Parity-invariant: projection depends only on logic inputs, not on which
// double-buffer is the draw target.
static uint64_t s_frame_hash = 1469598103934665603ull;
static long     s_frame_geom = 0;   // projected vertices folded this frame

void wide60_geom_xy(uint32_t sxy) {
  if (!g_wide60_on) return;
  uint64_t h = s_frame_hash;
  for (int i = 0; i < 4; i++) { h ^= (sxy & 0xFF); h *= 1099511628211ull; sxy >>= 8; }
  s_frame_hash = h;
  s_frame_geom++;
}

// ---- logic-rate detector (lrate_proto.c, validated) ---------------------------------
typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;
static RateDet s_rd = { .period = 2 };

static void rate_tick(RateDet* d, uint64_t set_hash) {
  if (set_hash == d->last_hash) { d->held++; return; }
  int p = d->held + 1;                 // ov_frame_update calls the prior set persisted
  if (p >= 1 && p <= 8) d->votes[p]++;
  int best = 0, bp = 2;
  for (int i = 1; i <= 8; i++) if (d->votes[i] > best) { best = d->votes[i]; bp = i; }
  d->period = bp;
  d->last_hash = set_hash; d->held = 0; d->changes++;
}

// ---- per-logic-frame fence (called from games_tomba2.c ov_frame_update) -------------
static long s_fence = 0;
void wide60_frame_commit(void) {
  if (!g_wide60_on) return;
  // A frame with no projected geometry (FMV/menu/paused — no GTE ops) is a HOLD: fold a constant so
  // the detector sees "unchanged" rather than a spurious empty-set change every such frame.
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  if ((s_fence % 500) == 0) {
    int quota = mem_r8(0x1F800235u);              // engine vblanks per displayed frame (=2 => 30fps)
    double content_fps = quota > 0 ? 60.0 / (quota * s_rd.period) : 0.0;
    fprintf(stderr, "[wide60] f%ld  modal-period=%d call(s)/change  quota=%d vbl/frame  "
            "=> content ~%.1f fps  (votes p1=%d p2=%d p3=%d p4=%d; changes=%ld, geom_last=%ld)\n",
            s_fence, s_rd.period, quota, content_fps,
            s_rd.votes[1], s_rd.votes[2], s_rd.votes[3], s_rd.votes[4], s_rd.changes, s_frame_geom);
  }
  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
}

void wide60_init(void) {
  g_wide60_on = getenv("PSXPORT_WIDE60") ? 1 : 0;
  if (g_wide60_on) fprintf(stderr, "[wide60] enabled (logic-rate measurement)\n");
}
