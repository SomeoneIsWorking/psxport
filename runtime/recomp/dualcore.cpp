// dualcore.cpp — NATIVE-RENDER vs PSX-RENDER guest-RAM divergence harness (PSXPORT_DUALCORE=1).
//
// Purpose (user finding, 2026-06-24): the gameplay regressions DISAPPEAR with PSXPORT_RENDER_PSX=1 (PSX
// render, native gameplay) but persist with PSXPORT_GATE=1 (PSX gameplay, native render) — i.e. the
// NATIVE RENDERER is corrupting guest RAM that the gameplay logic reads. To find the corrupting write
// MECHANICALLY, run the SAME game (native boot + native frame loop + NATIVE gameplay) two ways, differing
// ONLY in the render path, and diff the guest RAM per frame:
//   * core A = NATIVE render (g_render_psx = 0) — ov_render_frame runs the native render walks that attach
//                                  per-object depth into guest object fields (engine_submit.cpp).
//   * core B = PSX render    (g_render_psx = 1) — ov_render_frame dispatches the PSX recomp render 0x8003f9a8.
// Both run identical NATIVE gameplay, so they stay frame-synced (the FMV/load-time desync that breaks the
// psx_fallback compare does NOT apply here). We navigate each to the gameplay-START flag, then run N frames
// under an IDENTICAL scripted input schedule, snapshotting a focused RAM region + scratchpad each frame.
// Diffing A[k] vs B[k] yields the FIRST frame + address where native render's writes diverge from PSX —
// i.e. the guest state the native renderer corrupts. NB the render PACKET POOL [0x800BFE68,0x800E7E68) will
// diff legitimately (PSX writes GP0 packets there, native does not) — that is render noise, not the bug;
// the corruption is divergence OUTSIDE that pool (object structs / control blocks / scratchpad).
//
// SEQUENTIAL by design: the Beetle GTE/MDEC backends are process-global singletons, so we run A fully
// (recording per-frame snapshots into host RAM), then B fully, then diff offline. diff_mode=1 skips only
// the final VK present/OT-submit (shared host singleton); ov_render_frame's guest writes still happen.
//
// Tunables: PSXPORT_DC_N (frames after gameplay-start, default 180), PSXPORT_DC_LO / PSXPORT_DC_HI
// (focused region guest base/end, default 0x800B0000..0x80110000).

#include "game.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
extern "C" void watchdog_suspend(void);
// (g_render_psx retired — per-Core Render::setPsxRender(bool). Set on THIS core in run_and_record.)
#include "render/render.h"

namespace {

constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)

// PSX digital pad bits (active-low: a CLEARED bit = pressed).
constexpr uint16_t BTN_CROSS = 0x4000;
constexpr uint16_t BTN_START = 0x0008;
constexpr uint16_t BTN_RIGHT = 0x0020;
constexpr uint16_t BTN_NONE  = 0xFFFF;

// 3-phase machine, IDENTICAL to native_boot AUTO_SKIP (the proven nav): (0) tap Cross until GAME stage;
// (1) wait for the intro cutscene to start (flag -> 1); (2) pulse Start every 40 frames WHILE the flag is
// 1, and finish once the flag has read 0 for 60 CONSECUTIVE frames (the cutscene-end fade settles). The
// single phase-2 (keep tapping Start through any brief beat-gap re-assert) is what my earlier split got
// wrong. Frame/flag-driven only (no host input), so both cores navigate identically.
enum Phase { REACH_GAME, AWAIT_CUT, SKIP_CUT, DONE };
struct Nav { Phase phase = REACH_GAME; int idle = 0; };

bool nav_step(Core* c, Nav& nv, uint32_t f, const char* tag) {
  if ((f % 400u) == 0) fprintf(stderr, "[dc-nav] %s f%u phase=%d stage=%08X cut=%u\n",
                               tag, f, (int)nv.phase, c->mem_r32(TASK0_ENTRY), c->mem_r8(CUT_FLAG));
  uint8_t cut = c->mem_r8(CUT_FLAG);
  switch (nv.phase) {
    case REACH_GAME:
      if (c->mem_r32(TASK0_ENTRY) == GAME_ENTRY) { fprintf(stderr, "[dc] %s GAME @f%u\n", tag, f); nv.phase = AWAIT_CUT; }
      else if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
      break;
    case AWAIT_CUT:
      if (cut) { fprintf(stderr, "[dc] %s cutscene up @f%u\n", tag, f); nv.phase = SKIP_CUT; nv.idle = 0; }
      break;
    case SKIP_CUT:
      if (cut) { nv.idle = 0; if ((f % 40u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_START), 6); }
      else if (++nv.idle >= 60) { fprintf(stderr, "[dc] %s gameplay-start @f%u\n", tag, f); nv.phase = DONE; return true; }
      break;
    case DONE: return true;
  }
  return false;
}

// IDENTICAL scripted gameplay input by frames-since-start k: hold Right (walk into the field), with a
// Cross (jump) tap every 30 frames. Deterministic and the same for both cores.
void scripted_input(Core* c, int k) {
  c->game->pad.driveHold((uint16_t)(BTN_NONE & ~BTN_RIGHT));
  if ((k % 30) == 10) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_RIGHT & ~BTN_CROSS), 4);
}

// Boot one core, navigate to gameplay-start, then record `n` per-frame snapshots of [lo,hi) into `snaps`
// (snaps[k] = malloc'd region copy) plus the scratchpad into `spads[k]`. Returns frames actually recorded.
int run_and_record(const char* exe, int render_psx, const char* tag,
                   int n, uint32_t lo, uint32_t hi, uint8_t** snaps, uint8_t** spads) {
  uint32_t rsz = hi - lo;
  Game* g = new Game();
  g->psx_fallback = 0;                    // NATIVE gameplay in BOTH passes — only the render path differs
  g->diff_mode = 1;                       // skip the final VK present; ov_render_frame still runs + writes
  load_exe(exe, &g->core);
  dc_boot_init(&g->core);
  g->core.mRender->setPsxRender(render_psx != 0);   // per-core render path (0 = native walk, 1 = PSX recomp)

  Nav nv; uint32_t f = 0; const uint32_t MAXF = 6000; bool started = false; int k = 0;
  fprintf(stderr, "[dc] --- %s (psxRender=%d) ---\n", tag, render_psx);
  for (; f < MAXF && k < n; f++) {
    if (!started) {
      started = nav_step(&g->core, nv, f, tag);
      if (!started) { dc_step_frame(&g->core, f); continue; }
      // fall through on the start frame and record k=0 as the post-start state
    }
    scripted_input(&g->core, k);
    dc_step_frame(&g->core, f);
    snaps[k] = (uint8_t*)malloc(rsz); memcpy(snaps[k], g->core.ram + lo - 0x80000000u, rsz);
    spads[k] = (uint8_t*)malloc(0x400); memcpy(spads[k], g->core.scratch, 0x400);
    k++;
  }
  fprintf(stderr, "[dc] %s recorded %d frames (reached f%u)\n", tag, k, f);
  // NB: we intentionally leak the Game (one-shot harness, process exits after).
  return k;
}

// The legitimate render-only guest regions the native vs PSX render paths SHALL differ in (packet pool
// pointers, both packet-pool pages, both ordering-table pages, env). Divergence here is render noise, not
// the gameplay corruption we hunt. Excluded from the report unless PSXPORT_DC_ALL=1.
bool is_render_region(uint32_t a) {
  if (a >= 0x800BF4F0u && a < 0x800BF54Cu) return true;   // pool ptrs (0x800BF4F4/0x800BF544) + dwell
  if (a >= 0x800BFE68u && a < 0x800EA200u) return true;   // packet pool (×2 pages) + OT (×2 pages) + env
  return false;
}

// First-divergence coalesced report for two equal-length region buffers; skips render-only regions.
void diff_frame_region(const char* name, const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t gbase) {
  static int show_all = -1; if (show_all < 0) { const char* e = getenv("PSXPORT_DC_ALL"); show_all = (e && *e && strcmp(e,"0")) ? 1 : 0; }
  const uint32_t GAP = 64u;
  uint32_t i = 0, shown = 0;
  while (i < n && shown < 16) {
    if (a[i] == b[i] || (!show_all && is_render_region(gbase + i))) { i++; continue; }
    uint32_t s = i, last = i, gap = 0; i++;
    while (i < n && gap < GAP) { if (a[i] != b[i]) { last = i; gap = 0; } else gap++; i++; }
    fprintf(stderr, "    %s 0x%08X..0x%08X (%uB)  A:", name, gbase + s, gbase + last + 1, last + 1 - s);
    for (uint32_t k = s; k <= last && k < s + 8; k++) fprintf(stderr, "%02X", a[k]);
    fprintf(stderr, " B:");
    for (uint32_t k = s; k <= last && k < s + 8; k++) fprintf(stderr, "%02X", b[k]);
    fprintf(stderr, "\n"); shown++;
  }
}

} // namespace

void dualcore_run(const char* exe_path) {
  watchdog_suspend();
  int n = 180; { const char* e = getenv("PSXPORT_DC_N"); if (e && *e) n = atoi(e); }
  uint32_t lo = 0x800B0000u, hi = 0x80110000u;
  { const char* e = getenv("PSXPORT_DC_LO"); if (e && *e) lo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = getenv("PSXPORT_DC_HI"); if (e && *e) hi = (uint32_t)strtoul(e, 0, 0); }
  uint32_t rsz = hi - lo;
  fprintf(stderr, "[dualcore] NATIVE-render vs PSX-render RAM compare: N=%d region 0x%08X..0x%08X (%uKB/frame)\n",
          n, lo, hi, rsz / 1024);

  uint8_t** snA = (uint8_t**)calloc(n, sizeof(void*)); uint8_t** spA = (uint8_t**)calloc(n, sizeof(void*));
  uint8_t** snB = (uint8_t**)calloc(n, sizeof(void*)); uint8_t** spB = (uint8_t**)calloc(n, sizeof(void*));

  int kA = run_and_record(exe_path, 0, "A(native-render)", n, lo, hi, snA, spA);
  int kB = run_and_record(exe_path, 1, "B(PSX-render)",    n, lo, hi, snB, spB);

  int kn = kA < kB ? kA : kB;
  fprintf(stderr, "\n========== RENDER DIFF  (A=native-render  B=PSX-render)  comparing %d frames ==========\n", kn);
  fprintf(stderr, "  (ignore the render PACKET POOL 0x800BFE68..0x800E7E68 — legit render-path difference)\n");
  int first = -1;
  for (int k = 0; k < kn; k++) {
    bool ram_d = false;
    for (uint32_t i = 0; i < rsz; i++) if (snA[k][i] != snB[k][i] && !is_render_region(lo + i)) { ram_d = true; break; }
    bool spd_d = memcmp(spA[k], spB[k], 0x400) != 0;   // scratchpad has no render-pool exclusion
    if (ram_d || spd_d) {
      if (first < 0) { first = k; fprintf(stderr, "[dc] FIRST DIVERGENCE at gameplay-frame %d:\n", k); }
      if (k < first + 6) {     // detail the first few divergent frames
        fprintf(stderr, "  frame %d:\n", k);
        if (ram_d) diff_frame_region("ram ", snA[k], snB[k], rsz, lo);
        if (spd_d) diff_frame_region("spad", spA[k], spB[k], 0x400, 0x1F800000u);
      }
    }
  }
  if (first < 0) fprintf(stderr, "[dc] NO DIVERGENCE across %d frames — native gameplay == PSX gameplay here.\n", kn);
  else fprintf(stderr, "[dc] (divergence began at frame %d)\n", first);
  fprintf(stderr, "========================================================================\n");
}
