// dualcore.cpp — NATIVE-RENDER vs PSX-RENDER guest-RAM divergence harness (PSXPORT_DUALCORE=1).
//
// Purpose (user finding, 2026-06-24): the gameplay regressions DISAPPEAR with PSXPORT_RENDER_PSX=1 (PSX
// render, native gameplay) but persist with PSXPORT_GATE=1 (PSX gameplay, native render) — i.e. the
// NATIVE RENDERER is corrupting guest RAM that the gameplay logic reads. To find the corrupting write
// MECHANICALLY, run the SAME game (native boot + native frame loop + NATIVE gameplay) two ways, differing
// ONLY in the render path, and diff the guest RAM per frame:
//   * core A = NATIVE render (g_render_psx = 0) — ov_render_frame runs the native render walks that attach
//                                  per-object depth into guest object fields (submit.cpp).
//   * core B = PSX render    (g_render_psx = 1) — ov_render_frame dispatches the PSX recomp render 0x8003f9a8.
// Both run identical NATIVE gameplay, so they stay frame-synced (the FMV/load-time desync that breaks the
// psx_fallback compare does NOT apply here). We navigate each to the gameplay-START flag, then run N frames
// under an IDENTICAL scripted input schedule, snapshotting a focused RAM region + scratchpad each frame.
// Diffing A[k] vs B[k] yields the FIRST frame + address where native render's writes diverge from PSX —
// i.e. the guest state the native renderer corrupts. NB the render PACKET POOL + ordering tables (the
// GameConfig-derived RenderNoiseMask, render_noise.h) will diff legitimately (PSX writes GP0 packets
// there, native does not) — that is render noise, not the bug; the corruption is divergence OUTSIDE
// those windows (object structs / control blocks / scratchpad). The bounds used are PRINTED in the
// report header; they used to be Tomba!2 literals printed into every game's log.
//
// SEQUENTIAL by design: the Beetle GTE/MDEC backends are process-global singletons, so we run A fully
// (recording per-frame snapshots into host RAM), then B fully, then diff offline. diff_mode=1 skips only
// the final VK present/OT-submit (shared host singleton); ov_render_frame's guest writes still happen.
//
// Tunables: PSXPORT_DC_N (frames after gameplay-start, default 180), PSXPORT_DC_LO / PSXPORT_DC_HI
// (focused region guest base/end, default 0x800B0000..0x80110000).

#include "game.h"
#include "dualcore.h"
#include "cfg.h"
#include "game_iface.h"        // psxport_game_config() — the nav predicate + render mask come from it
#include "task_slot_layout.h"  // task0_stage_entry_addr() (STOPGAP: the slot-field offset)
#include "repl_service.h"      // refuse_if_unserviced — this harness has NO Repl::read() pump
#include <lucent/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
extern "C" void watchdog_suspend(void);
// (g_render_psx retired — per-Core Render::setPsxRender(bool). Set on THIS core in run_and_record.)
#include "render_substrate.h"

namespace {

// GAME_ENTRY / TASK0_ENTRY were Tomba!2 literals (0x8010637C / 0x801fe00c) sitting in game-agnostic
// framework code; they now come from GameConfig::stageGame / taskTableBase (DualCore::mStageGame /
// mStageEntryAddr, set in run(), which REFUSES to run without them).
//
// CUT_FLAG is still a Tomba!2 literal: the cutscene-active scratchpad byte has NO GameConfig field, and
// inventing one was out of scope for this sweep. It is only reached AFTER the stage predicate above
// fires, and run() cannot start without that predicate, so it is unreachable on a game that has not
// RE'd its stage entry. STOPGAP: GameConfig::cutsceneActiveFlag.
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)

// PSX digital pad bits (active-low: a CLEARED bit = pressed).
constexpr uint16_t BTN_CROSS = 0x4000;
constexpr uint16_t BTN_START = 0x0008;
constexpr uint16_t BTN_RIGHT = 0x0020;
constexpr uint16_t BTN_NONE  = 0xFFFF;

// (The 3-phase nav machine — REACH_GAME / AWAIT_CUT / SKIP_CUT / DONE — is DualCore::Nav; see
// dualcore.h. Frame/flag-driven only, no host input, so both cores navigate identically.)

} // namespace

bool DualCore::navStep(Core* c, Nav& nv, uint32_t f, const char* tag) {
  if ((f % 400u) == 0) lucent::info("dc-nav", "{} f{} phase={} stage={:08X} cut={}", tag, f, (int)nv.phase, c->mem_r32(mStageEntryAddr), c->mem_r8(CUT_FLAG));
  uint8_t cut = c->mem_r8(CUT_FLAG);
  switch (nv.phase) {
    case REACH_GAME:
      if (c->mem_r32(mStageEntryAddr) == mStageGame) { lucent::info("dc", "{} GAME @f{}", tag, f); nv.phase = AWAIT_CUT; }
      else if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
      break;
    case AWAIT_CUT:
      if (cut) { lucent::info("dc", "{} cutscene up @f{}", tag, f); nv.phase = SKIP_CUT; nv.idle = 0; }
      break;
    case SKIP_CUT:
      if (cut) { nv.idle = 0; if ((f % 40u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_START), 6); }
      else if (++nv.idle >= 60) { lucent::info("dc", "{} gameplay-start @f{}", tag, f); nv.phase = DONE; return true; }
      break;
    case DONE: return true;
  }
  return false;
}

// IDENTICAL scripted gameplay input by frames-since-start k: hold Right (walk into the field), with a
// Cross (jump) tap every 30 frames. Deterministic and the same for both cores.
void DualCore::scriptedInput(Core* c, int k) {
  c->game->pad.driveHold((uint16_t)(BTN_NONE & ~BTN_RIGHT));
  if ((k % 30) == 10) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_RIGHT & ~BTN_CROSS), 4);
}

// Boot one core, navigate to gameplay-start, then record `n` per-frame snapshots of [lo,hi) into `snaps`
// (snaps[k] = malloc'd region copy) plus the scratchpad into `spads[k]`. Returns frames actually recorded.
int DualCore::runAndRecord(const char* exe, int render_psx, const char* tag,
                           int n, uint32_t lo, uint32_t hi, uint8_t** snaps, uint8_t** spads) {
  uint32_t rsz = hi - lo;
  Game* g = new Game();
  g->psx_fallback = 0;                    // NATIVE gameplay in BOTH passes — only the render path differs
  g->diff_mode = 1;                       // skip the final VK present; ov_render_frame still runs + writes
  load_exe(exe, &g->core);
  dc_boot_init(&g->core);
  g->core.rsub.mode.setPath(render_psx ? RenderPath::Gte : RenderPath::Native);   // per-core render path

  Nav nv; uint32_t f = 0; const uint32_t MAXF = 6000; bool started = false; int k = 0;
  lucent::info("dc", "--- {} (psxRender={}) ---", tag, render_psx);
  for (; f < MAXF && k < n; f++) {
    if (!started) {
      started = navStep(&g->core, nv, f, tag);
      if (!started) { dc_step_frame(&g->core, f); continue; }
      // fall through on the start frame and record k=0 as the post-start state
    }
    scriptedInput(&g->core, k);
    dc_step_frame(&g->core, f);
    snaps[k] = (uint8_t*)malloc(rsz); memcpy(snaps[k], g->core.ram + lo - 0x80000000u, rsz);
    spads[k] = (uint8_t*)malloc(0x400); memcpy(spads[k], g->core.scratch, 0x400);
    k++;
  }
  lucent::info("dc", "{} recorded {} frames (reached f{})", tag, k, f);
  // NB: we intentionally leak the Game (one-shot harness, process exits after).
  return k;
}

// The legitimate render-only guest regions the native vs PSX render paths SHALL differ in (packet pool
// pointers, both packet-pool pages, both ordering-table pages, env). Divergence here is render noise, not
// the gameplay corruption we hunt. Excluded from the report unless PSXPORT_DC_ALL=1.
//
// The windows are DERIVED from GameConfig (render_noise.h) — they were Tomba!2 literals, which on any
// other game masked 168 KB of ordinary engine data out of a 384 KB focused region while the report still
// said "NO DIVERGENCE". An un-RE'd game gets an EMPTY mask (masks nothing) and a loud line; it never
// inherits another game's window.
bool DualCore::isRenderRegion(uint32_t a) const { return mMask.covers(a); }

// First-divergence coalesced report for two equal-length region buffers; skips render-only regions.
void DualCore::diffFrameRegion(const char* name, const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t gbase) {
  const uint32_t GAP = 64u;
  uint32_t i = 0, shown = 0;
  while (i < n && shown < 16) {
    if (a[i] == b[i] || (!show_all && isRenderRegion(gbase + i))) { i++; continue; }
    uint32_t s = i, last = i, gap = 0; i++;
    while (i < n && gap < GAP) { if (a[i] != b[i]) { last = i; gap = 0; } else gap++; i++; }
    lucent::Line ln;
    ln.add("    {} 0x{:08X}..0x{:08X} ({}B)  A:", name, gbase + s, gbase + last + 1, last + 1 - s);
    for (uint32_t k = s; k <= last && k < s + 8; k++) ln.add("{:02X}", a[k]);
    ln.add(" B:");
    for (uint32_t k = s; k <= last && k < s + 8; k++) ln.add("{:02X}", b[k]);
    ln.flush(lucent::Level::Info, "dc"); shown++;
  }
}

void DualCore::run(const char* exe_path) {
  // BEFORE ANYTHING IS BOOTED: this harness never reads stdin — Repl::read() is pumped only by the
  // single-core loop in native_boot.cpp — so a piped REPL script handed to a PSXPORT_DUALCORE run
  // would sit in the pipe while the harness ran its own scripted nav, and the RAM diff would be
  // reported as if the script had driven it (Tomba2Engine kanban #90, the same defect sbs.cpp was
  // guarded for). Same failure shape as the stage-predicate refusal below: a clean bill of health
  // over a run that measured something else.
  psx::repl_service::refuse_if_unserviced("DualCore", /*loopServicesRepl=*/false);

  // REFUSE rather than measure the wrong thing. The nav machine's REACH_GAME test is
  // `mem_r32(task0+0xc) == stageGame`; with both unset that is `mem_r32(0xc) == 0`, which is TRUE at
  // frame 0 (the stage word is zero during boot too). The harness would log "GAME @f0", record 180
  // frames of BIOS/crt0 boot as gameplay, and then — because both cores boot identically — print
  // "NO DIVERGENCE ... native gameplay == PSX gameplay here". A false clean bill of health is worse
  // than no run, so this exits before booting anything.
  const GameConfig* cfg = psxport_game_config();
  mStageGame      = cfg ? cfg->stageGame : 0;
  mStageEntryAddr = task0_stage_entry_addr(cfg);
  if (!mStageGame || !mStageEntryAddr) {
    lucent::error("dualcore",
                  "REFUSING TO RUN: DUALCORE requires GameConfig::stageGame (have 0x{:08X}) and "
                  "GameConfig::taskTableBase (stage-entry word 0x{:08X}) — this game has not RE'd its "
                  "stage entry, so I cannot tell gameplay from boot. With these at 0 the REACH_GAME "
                  "predicate matches on frame 0 and the harness would compare the BIOS boot and call it "
                  "clean. Fill those fields in the game's game_config.cpp and re-run.",
                  mStageGame, mStageEntryAddr);
    return;
  }
  watchdog_suspend();
  show_all = cfg_on("PSXPORT_DC_ALL");
  mMask = RenderNoiseMask::from(cfg, "dualcore");
  int n = cfg_int("PSXPORT_DC_N", 180);
  uint32_t lo = 0x800B0000u, hi = 0x80110000u;
  { const char* e = cfg_str("PSXPORT_DC_LO"); if (e && *e) lo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = cfg_str("PSXPORT_DC_HI"); if (e && *e) hi = (uint32_t)strtoul(e, 0, 0); }
  uint32_t rsz = hi - lo;
  lucent::info("dualcore", "NATIVE-render vs PSX-render RAM compare: N={} region 0x{:08X}..0x{:08X} ({}KB/frame)", n, lo, hi, rsz / 1024);

  uint8_t** snA = (uint8_t**)calloc(n, sizeof(void*)); uint8_t** spA = (uint8_t**)calloc(n, sizeof(void*));
  uint8_t** snB = (uint8_t**)calloc(n, sizeof(void*)); uint8_t** spB = (uint8_t**)calloc(n, sizeof(void*));

  int kA = runAndRecord(exe_path, 0, "A(native-render)", n, lo, hi, snA, spA);
  int kB = runAndRecord(exe_path, 1, "B(PSX-render)",    n, lo, hi, snB, spB);

  int kn = kA < kB ? kA : kB;
  lucent::info("dc", "\n========== RENDER DIFF  (A=native-render  B=PSX-render)  comparing {} frames ==========", kn);
  // Print the range ACTUALLY masked. This line used to print Tomba!2's pool bounds into every game's
  // log, which is how a wrong reading becomes a durable note in someone's findings file.
  { char b[256]; lucent::info("dc", "  (excluded as legit render-path difference: {})", mMask.describe(b, sizeof b)); }
  int first = -1;
  for (int k = 0; k < kn; k++) {
    bool ram_d = false;
    for (uint32_t i = 0; i < rsz; i++) if (snA[k][i] != snB[k][i] && !isRenderRegion(lo + i)) { ram_d = true; break; }
    bool spd_d = memcmp(spA[k], spB[k], 0x400) != 0;   // scratchpad has no render-pool exclusion
    if (ram_d || spd_d) {
      if (first < 0) { first = k; lucent::info("dc", "FIRST DIVERGENCE at gameplay-frame {}:", k); }
      if (k < first + 6) {     // detail the first few divergent frames
        lucent::info("dc", "  frame {}:", k);
        if (ram_d) diffFrameRegion("ram ", snA[k], snB[k], rsz, lo);
        if (spd_d) diffFrameRegion("spad", spA[k], spB[k], 0x400, 0x1F800000u);
      }
    }
  }
  if (first < 0) lucent::info("dc", "NO DIVERGENCE across {} frames — native gameplay == PSX gameplay here.", kn);
  else lucent::info("dc", "(divergence began at frame {})", first);
  lucent::info("dc", "========================================================================");
}
