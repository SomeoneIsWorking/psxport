// PC-PSX hybrid native boot driver.
//
// Architecture (locked): the PC engine is the driver. It runs the game's INIT calls (the
// prefix of game-main FUN_80050b08) via the recompiler, then OWNS the frame loop, replacing
// the PSX cooperative-task scheduler (FUN_80051e60) with native per-frame stepping of the
// current stage's state machine. No BIOS threads, no ucontext: leaf logic fns are normal
// recompiled fns that RETURN; the stage sequencers' infinite yield-loops are reimplemented
// natively (one state-machine iteration per frame == one original FUN_80051f80 yield).
//
// We do NOT call FUN_80050b08 directly (it ends in the infinite scheduler loop). Instead we
// override it: crt0 (func_800896E0) does BSS-zero + SP/gp/heap setup and calls main, which is
// now this native driver. The init prefix below is transcribed 1:1 from FUN_80050b08
// (ram_f1000_all.c:31275-31299); each call is dispatched into the recompiled/overridden body,
// whose CD/sync/vsync/pad/thread dependencies are already native overrides.
//
// MILESTONE 1 (this file, current): run the init prefix and confirm it executes cleanly via
// PC/RAM probes. The native frame loop + per-stage stepping land next.
#include "core.h"
#include "game.h"      // PcScheduler (per-instance cooperative-task state) reached via c->game->pcSched
#include "hw_bind.h"   // spu_bind/mdec_bind/xa_bind (per-instance HW-peripheral binders)
#include "scheduler.h" // scheduler_yield + TASKBASE/TASKSTRIDE/CUR_TASK (scheduler.cpp)
#include "c_subsys.h"
#include "cfg.h"
#include "asset.h"     // class Asset — c->engine.asset (unpackGroup / uploadImage / preload*)
#include "render.h"  // full game Render umbrella — native_step_frame calls the game method c->mRender->bbFrameReset()
                     // (rsub substrate members come via core.h -> render_substrate.h)
#include "mods.h"            // g_mods.fps60 / g_mods.aspect — forced off in PSXPORT_ORACLE
#include "audio/music_list.h"   // native sound-test: music_list_play/stop (engine/audio/)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>   // usleep (debug-server pause/step idle wait)
#include <execinfo.h>
#include "coro.h"      // thread-fiber for full-PSX mid-function resume (later-264)
#include "dualview_snapshot.h"  // dv_capture_post/dv_restore_pre/dv_restore_post (extern "C" linkage)
#include "guest_call.h" // rc0-4 guest-call helpers (shared with repl.cpp)
#include "repl.h"       // class Repl — REPL driver + auto-drive request state (per-Core, on Game)

// Native XA voice/BGM clip player (xa_stream.c) owns task slot 2 — it replaced the FUN_8001cfc8
// streaming-reader coroutine. The scheduler skips slot 2 while owned and reflects clip completion
// into the task-2 state byte (the cutscene waits `while (DAT_801fe0e0 != 0)`).
// class MusicCoord (game/audio/music_coord.h) — reached as c->engine.musicCoord.tick() per frame

// The native cooperative-task scheduler (scheduler_yield + PcScheduler::step — the FUN_80080880/
// FUN_80051e60 replacements) lives in scheduler.cpp + game/core/pc_scheduler.cpp; TASKBASE/
// TASKSTRIDE/CUR_TASK (used by the REPL/debug state probes below) are reached via "scheduler.h".

// ---- BGM frame counter (PSXPORT_BGMDBG trace shared with cd_override.cpp) --------------------
// FUN_80074BF8(idx) starts BGM #idx; FUN_80074E48() stops it. These are now OWNED PC-native by
// game/audio/sfx.cpp, which also carries the instant-CD dialog-music cut hook. Only
// the shared frame counter remains here (cd_override.cpp externs it for its own BGM trace).
// g_bgm_frame retired 2026-07-03 — per-Core Timing::logicFrame (c->game->timing.logicFrame).
void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

// ONE frame of deterministic guest work — the steppable core of the native frame loop, factored out so
// the in-process dual-core diff can step TWO cores in lockstep (it calls this on `a` then `b`). It is
// EXACTLY the guest-mutating body of the loop below (per-frame IRQ events, draw/display-env setup, the
// FUN_800788ac frame update + native scheduler pass + dialog-music coord + draw sync + buffer flip); the
// loop's driver scaffolding (REPL, auto-navigation/input, pause/step, diagnostics, dbg_server) stays in
// the loop and runs ONCE around this call. No input is injected here — drive pads before calling it.
// gpu_perf.cpp — per-frame CPU phase / frame-time profiler (REPL `debug perf`), default off.
#include "gpu_perf.h"

// ---- DUAL-VIEW guest-state snapshots (native | PSX side-by-side render of ONE game state) -------------
// Save/restore of full guest state (main RAM + scratchpad + GTE regs) lives in dualview_snapshot.cpp
// (dv_snapshot / dv_capture_post / dv_restore_pre / dv_restore_post + g_dv_have_pre). See
// native_step_frame's dual-view block below for how the sequence is driven.
// g_dualview retired 2026-07-02 — per-Core Render::mDualview / dualview() / setDualview(bool).
#include "sbs.h"                        // class Sbs — the PSXPORT_SBS two-core side-by-side harness

static void native_step_frame(Core* c, uint32_t f) {
  const GameConfig* cfg = c->cfg;
  void gte_bind(Core*); gte_bind(c);   // bind THIS core's GTE register file (per-instance — no shared GTE)
  // #42 widescreen symmetry: re-assert the projection center (GTE CR24 = OFX) from the LIVE present
  // width every frame. Engine::initDisplay baked OFX once at boot, but the SDL window is lazy-created
  // (first present), so at boot win_w=320 -> AUTO nw=320 -> OFX=160 (the 4:3 center) got baked, and the
  // wide view then expanded only to the RIGHT. Re-asserting nw/2 each frame centers it symmetrically
  // (the middle 4:3 band stays pixel-identical). CR24 is a GTE control reg, not guest RAM -> read-only-
  // overlay compliant; gated on wide_engine so 4:3 (OFX=160, margin==0) and the oracle are untouched.
  { int gpu_gpu_wide_engine(Core*); int gpu_gpu_wide_engine_ofx(Core*);
    if (gpu_gpu_wide_engine(c)) gte_write_ctrl(24u, (uint32_t)gpu_gpu_wide_engine_ofx(c) << 16); }
  c->rsub.projprim.bind(c);         // bind THIS core's native-depth cache (class ProjPrim on Render)
  spu_bind(c);                          // bind THIS core's SPU state (per-instance — no shared SPU)
  mdec_bind(c);                         // bind THIS core's MDEC state (per-instance — no shared MDEC)
  xa_bind(c);                           // bind THIS core's XA streamer state (per-instance — no shared XA)
  c->game->ffspan.resetFrame();   // backdrop-attribution: reset the per-frame builder span table
  void gpu_set_disp_origin(Core* c, int x, int y);
  // Timing::logicFrame is the per-Core frame counter several subsystems consult (audio trace tags,
  // BGM-director gating, findings/debug instrumentation). The standalone frame loop (below, in main())
  // used to be the ONLY writer (`c->game->timing.logicFrame = f` right before calling this function),
  // so under SBS — which reaches this function via dc_step_frame()/stepCore() without ever running that
  // loop — logicFrame silently stayed 0 for the whole run. Set it here, at the single per-frame entry
  // point both the standalone loop and SBS funnel through, so it is correct under every caller.
  c->game->timing.logicFrame = f;
  c->game->perf.frameBegin();   // perf: start the frame clock (top of the deterministic per-frame work)
  // Advance the libetc VSync counter (DAT_800abde0) — one vblank per native frame. VSync(0) is trapped,
  // so this is the only thing that ticks the recomp timebase (recomp tasks read it to pace animations).
  c->game->timing.frameTick();
  // Per-frame IRQ-driven events the game's waits poll via TestEvent (VBlank classes + sound-DMA-complete).
  c->game->hle.deliverEvent(cfg->irqEventClasses[0], 0xFFFFFFFFu);
  c->game->hle.deliverEvent(cfg->irqEventClasses[1], 0xFFFFFFFFu);
  c->game->hle.deliverEvent(cfg->irqEventClasses[2], 0xFFFFFFFFu);
  // SINGLE-BUFFERED (PC-native) — the game's own PSX double-buffering is REMOVED (user: "remove the
  // game's own double buffering, it causes problems"). The PSX flips between two VRAM pages each frame:
  // OT region 0x800e80a8 + parity*0x2070 and packet pool 0x800bfe68 + parity*0x14000, with the display/
  // draw env following parity. That page-flip is pointless on PC — the VK renderer composites a COMPLETE
  // frame and the present provides the display buffering — and it actively caused aliasing: the native
  // display-list buckets are keyed by OT address (ndl_alloc on otaddr & 0x1FFFFC), so they alternated
  // between the two pages, and the present could sample the page being drawn. Pin the back-buffer parity
  // to 0: one OT region, one packet pool, the same env every frame, and NO flip below. 0x1f800135 stays
  // 0 (set by eng_init_framestate), so any guest code that reads it also sees a stable single buffer.
  const uint32_t parity = 0;                                 // was mem_r8(0x1f800135) — pinned single-buffer
  uint32_t envp = cfg->otRegionBase + parity * cfg->otRegionStride;     // the one VRAM region we DRAW into every frame
  c->mem_w32(cfg->otBasePtr, envp);                           // PTR_DAT_800ed8c8 (OT base, now constant)
  rc2(c, cfg->clearOtagR, envp, 0x800);                       // ClearOTagR(ot, 0x800)
  c->mem_w16(cfg->dwellCounter, 0);                           // DAT_800e809c = 0 (dwell counter)
  c->mem_w32(cfg->poolPtrLast, c->mem_r32(cfg->poolPtrCur));  // keep last pool ptr (read by some submitters)
  c->mem_w32(cfg->poolPtrCur, (parity * cfg->packetPoolStride + cfg->packetPoolBase) & 0xffffff);   // packet pool (now constant)
  c->game->pad.serviceFrame();                                       // host input -> game pad buffer (pre-read)
  c->game->cd.audioTrace("pre");                                   // CD-vol fade state BEFORE tick+mix
  c->game->perf.markPre();   // perf: charge the pre-tick host work (input/IRQ/OT-clear) to `pre`
  // PC-driven frame body: per-frame state update (still-PSX leaf) + per-vblank audio + fps60 commit +
  // present + pace. Called as a plain C call (top-down PC-driven model) — NOT an override. This is where
  // gpu_present / gpu_pace_frame / the per-vblank sequencer+SPU tick are reached every live frame; before
  // this was wired they were orphaned with the override table (a live window ran uncapped + stale). The
  // present happens here (before the OT submit below) so the VK batch shown is the one DrawOTag built last
  // frame, exactly as the override-era ordering did.
  c->game->ffspan.begin();
  c->hooks->frameUpdate(c);                                   // tick + per-vblank audio + present + pace
  c->game->ffspan.end("frameupd");
  // Billboard-record frame boundary (#67): the records the guest render walk (pcSched.step below) is
  // about to capture belong to the NEW logic frame; the presents above (fps60's interp re-run included)
  // consumed last frame's. Reset here — after present, before the walk — mirroring the mObjCur rotation.
  c->mRender->bbFrameReset();
  c->game->cd.audioTrace("post");                                  // CD-vol fade state AFTER tick+mix
  c->game->perf.phaseBegin(3);   // perf: SCHED-LOGIC = the cooperative scheduler step (the real per-frame GAME logic)
  // The native scheduler is the frame-loop's task-stepping HARNESS (no BIOS threads — yields are setjmp/
  // longjmp coroutines, CD loads are synchronous). It stays native at every gate level. What the gate
  // controls is whether the TASK BODIES it steps run as native stage dispatchers + content (full native) or
  // as pure PSX recomp coroutines (psx_fallback on) — see the gate checks inside PcScheduler::step.
  c->game->ffspan.begin();
  c->game->pcSched.step();                                    // <- replaces FUN_80051e60 (BIOS scheduler)
  c->game->ffspan.end("scheduler");
  c->game->perf.phaseEnd(3);
  c->hooks->musicCoordTick(c);                                // dialogs stop/restore ingame music
  c->game->cd.audioTrace("coord");                                 // CD-vol fade state AFTER coord
  rc1(c, cfg->drawSync, 0);                                   // draw sync
  c->game->pcSched.tickSleepCountdown();                      // was rc0(c, 0x800506d0) — task sleep-countdown (re-arm 1->2)
  // Display + OT submit (LAB_80050c6c, DAT_1f80019c==0 branch). Single-buffered, PC-native display.
  // The PSX disp-env dance is GONE: we draw page `parity` (env0 → VRAM (0,0)) and tell the PC present to
  // scan that very page DIRECTLY via gpu_set_disp_origin, instead of routing through PutDispEnv and the
  // opposite buffer's disp-env struct (the later-161 (1-parity) trick — a band-aid over the env pairing
  // that depended on the two structs lining up). One fixed page, drawn and displayed; nothing PSX in the
  // display path. (env0's draw area starts at VRAM (0,0); the display W/H stay as the boot env's mode set.)
  // Dual-core diff mode skips the whole display/OT-submit block (host render output, shared VK singleton).
  // SBS: both cores set diff_mode=1 (suppress per-core present at the engine frame tail) but ALSO
  // sbs_render=1 to re-enable THIS render-submit block, so each core EMITS its geometry into its own VK
  // batch (the SBS composite then draws each into its pane). So gate on "not-diff-mode OR sbs_render".
  if ((!c->game->diff_mode || c->game->sbs_render) && c->mem_r16(0x1f80019c) == 0) {
    rc1(c, cfg->putDrawEnv, envp + 0x2014);                   // PutDrawEnv (draw area/offset/clip for page 0)
    gpu_set_disp_origin(c, 0, 0);                             // PC-native: present scans the page we draw
    // DrawOTag, PC-native: call Engine::drawOTag DIRECTLY (top-down) instead of interpreting the PSX FUN_80081560.
    // drawOTag walks the OT to ENUMERATE the leftover guest prims (its draw ORDER is discarded), QUEUES
    // them into the engine render queue (rq_active() is always on), then rq_flush()es the queue in ENGINE
    // order. The interpreted PSX DrawOTag did the walk+queue but NOT the flush (rq_flush only lives in
    // Engine::drawOTag, orphaned by the override-table removal) — so the queue filled every frame and never
    // drained, and NOTHING 2D reached the VK renderer (the whole front-end rendered black). OT head arg.
    c->hooks->drawOTag(c, envp + 0x1ffcu);
    // ---- DUAL-VIEW second render pass: render the SAME game state via the PSX recomp path into render
    // target 1 (right panel). The engine render is NOT idempotent (its per-frame queues/OT get consumed),
    // so the PSX pass must run from the PRE-render state captured in ov_field_frame (dv_snapshot, before
    // the native render ran), not from the post-native-render state. We then restore the POST-FRAME state
    // so the canonical game (which includes the post-render per-frame area update) is undisturbed.
    extern void gpu_gpu_select_target(int);
    // SBS owns BOTH panes (core A | core B); its target-1 batch is core B's render, NOT a PSX re-render of
    // THIS core — so skip the in-engine dualview second pass. g_sbs declared at file scope below.
    DualviewSnapshot& dv = c->rsub.dualviewSnapshot;
    if (c->rsub.mode.dualview() && dv.havePre() && !c->game->sbs) {
      dv.capturePost(c);             // save the real post-frame canonical state
      dv.restorePre(c);              // rewind to the pre-render (post-gameplay) state the PSX pass needs
      rc2(c, cfg->clearOtagR, envp, 0x800);                     // ClearOTagR(ot, 0x800)
      c->mem_w32(cfg->otBasePtr, envp);                         // OT base
      c->mem_w16(cfg->dwellCounter, 0);                         // dwell counter
      c->mem_w32(cfg->poolPtrLast, c->mem_r32(cfg->poolPtrCur));
      c->mem_w32(cfg->poolPtrCur, (cfg->packetPoolBase) & 0xffffff);  // reset packet pool ptr
      gpu_gpu_select_target(1);
      rec_dispatch(c, cfg->dualviewRenderOrch);                 // PSX field render orchestrator (full OT build)
      rec_dispatch(c, cfg->dualviewSubmit);                     // render submit (faithful to ov_field_frame)
      c->hooks->drawOTag(c, envp + 0x1ffcu);                    // walk PSX OT -> target-1 batch
      gpu_gpu_select_target(0);
      dv.restorePost(c);             // restore the real canonical state (PSX pass fully undone)
      dv.clearPre();
    }
  }
  c->game->perf.frameEnd();   // perf: close the frame (post-tick remainder + full wall time) + emit rolling avg
}

// Native override of game-main FUN_80050b08: init prefix, then (later) native frame loop.

static void game_main(Core* c);

// PC-native crt0 (faithful reimplementation of FUN_800896E0): BSS-zero, heap setup, then a DIRECT
// call to game_main — the top of the top-down PC-driven spine. Replaces the old bootstrap flip
// (crt0 jal main -> override). The libc/heap init at 0x80089860 stays a dispatched PSX leaf.
// crt0 register/heap setup only (no main call) — shared by native_crt0 and the dual-core harness.
static void crt0_setup(Core* c) {
  const GameConfig* cfg = c->cfg;
  for (uint32_t a = cfg->bssZeroLo; a < cfg->bssZeroHi; a += 4) c->mem_w32(a, 0);   // BSS zero
  uint32_t v0 = c->mem_r32(cfg->stackTopBase) - 8;   // stack top base
  uint32_t sp = v0 | 0x80000000u;
  uint32_t a0 = cfg->heapBase & 0x1FFFFFFFu;          // 0x00106228 (heap base, masked)
  uint32_t v1 = c->mem_r32(cfg->stackTopBase2);
  uint32_t heapsz = (v0 - v1) - a0;
  c->mem_w32(cfg->heapSizePtr, heapsz);              // heap size
  a0 |= 0x80000000u;                                 // 0x80106228
  c->mem_w32(cfg->heapBasePtr, a0);                  // heap base
  c->r[28] = cfg->gp;                                // gp
  c->r[29] = sp; c->r[30] = sp;                      // sp, fp
  c->r[4]  = a0 + 4;                                 // a0 for the init call
  rec_dispatch(c, cfg->libcInit);                    // libc/heap init (PSX leaf) — keep dispatched
}

static void native_crt0(Core* c) {
  crt0_setup(c);
  game_main(c);                                   // DIRECT PC call (was: crt0 jal main -> flip)
}

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop). Factored out
// of game_main so the dual-core harness can init two cores then drive the frame loop itself.
static void game_init(Core* c) {
  fprintf(stderr, "[native_boot] FUN_80050b08 override: running init prefix\n");

  // --- init prefix, transcribed from FUN_80050b08 (no scheduler loop) ---
  rc0(c, 0x80089788);
  rc0(c, 0x80085b20);
  // CD init: native HLE, NOT the recomp libcd (FUN_800898a0). The recomp CdInit busy-waits on the
  // CD-controller reset handshake (no IRQ ever acks → 5 retries → "CD timeout" → "Init failed").
  // We model no CD controller; all CD ops are native synchronous (cd_override.cpp). (was rc0 0x800898a0)
  c->game->cd.hleInit();
  rc1(c, 0x80080bf0, 3);
  rc1(c, 0x80080d64, 0);
  rc1(c, 0x80080ed4, 1);
  rc1(c, 0x800865f0, 0);
  // Engine frame-state + camera init reimplemented PC-native (game/scene/startup.cpp), replacing the
  // 1:1 rec_dispatch transcription. FUN_800509b4 (display/GTE projection + PSX draw/disp double-buffer
  // env) stays dispatched for now — it sets DAT_801003f8 = H that eng_init_camera reads, and entangles
  // the PSX-GPU env, so it is the next target. (later-159, top-down engine port from main.)
  c->engine.initFrameState();      // was rc0(c, 0x80050a0c)
  c->engine.initDisplay();         // was rc0(c, 0x800509b4) — GTE projection + display (sets H=DAT_801003f8)
  c->engine.initCamera();          // was rc0(c, 0x80050a80)
  rc0(c, 0x80096a70);
  rc1(c, 0x80099310, 0x1010);
  rc1(c, 0x800991b0, 0x20000);
  rc1(c, 0x800993a0, 1);
  // FUN_80089bac(cmd=0xe Setmode, &mode=0x80, 0) — CdControlB. The recomp body busy-waits in CD_cw
  // on the controller ack (no IRQ -> "CdlSetmode timeout"). We model no CD drive mode (every read is
  // by LBA, served natively), so Setmode is a native no-op. (was rc3 0x80089bac)
  // (removed: VSync(3) display-settle wait — the PC-native frame loop owns ALL timing; boot does not
  // call libetc VSync. Any code that reaches VSync now TRAPS (sync_overrides.cpp vsync_trap).)
  // FUN_80075130 font/text init reimplemented PC-native (game/ui/font.cpp): owns the orchestration +
  // direct writes + the 3 engine-state callees (FUN_800963a0/80096370/800752b4); the 8 libgpu/sound callees
  // stay rec_dispatched in-context (later-182b nested-dispatch risk). Replaces the rc0 transcription.
  c->engine.font.init();                       // was rc0(c, 0x80075130)
  rc1(c, 0x8009c620, 0);
  rc0(c, 0x8001cc00);
  c->engine.initSubsystems();               // was rc0(c, 0x800520e0) — own orchestration native
  // (removed: VSync(1) — see above; PC owns timing, boot never calls VSync.)
  rc0(c, 0x80051e00);                       // scheduler-table init (task objs @0x801fe000)
  rc2(c, 0x80051f14, 0, 0x800499e8);        // register task 0, entry FUN_800499e8
  // VSyncCallback(LAB_800506b4): native no-op — we deliver no preemptive VBlank IRQ (the per-vblank
  // callback's unmodeled interrupt-vector deref is skipped). (was rc1 0x80085bb0)
  c->game->timing.vsyncCallback();          // callback ptr arg unused (no preemptive vblank IRQ delivered)

  fprintf(stderr, "[native_boot] init prefix complete\n");

  // --- task 0 initial entry: FUN_800499e8 resolves \BIN\START.BIN and FUN_80052078(0) loads
  // the stage-0 overlay to 0x80106228 + restarts task 0 at stage 0 (0x8010649c). It yields once
  // (FUN_80051f80, a no-op with threads stubbed) so it runs straight to completion here. The
  // scheduler's "current task" ptr DAT_1f800138 is normally set by FUN_80051e60; set it to task0
  // so FUN_80052078/FUN_800450bc operate on task 0. ---
  c->mem_w32(0x1f800138, 0x801fe000);
  c->engine.task0Bootstrap();   // PC-native: was rc0(c, 0x800499e8) — CD subtree owned top-down
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          c->mem_r32(0x80106228), c->mem_r32(0x8010649c), c->mem_r16(0x801fe000), c->mem_r32(0x801fe00c));
}

// Dual-core harness hooks (dualcore.cpp / selftest.cpp / sbs.cpp): boot a core to the start of the
// frame loop, then step it one frame at a time. dc_boot_init = crt0 setup + the init prefix/bootstrap;
// dc_step_frame = one frame.
//
// c->hooks->registerOverrides(c->game) HAS to run here (2026-07-08 fix, docs/findings/tooling.md
// "SBS/DualCore/Selftest never populate their own Game's override table"): every harness that boots
// via dc_boot_init constructs its OWN Game, and the registration previously lived ONLY inline in
// boot.cpp's main(), against a single throwaway Game these harnesses never touch. Without calling it
// here, the process-global override registry (overrides::install, override_registry.h) never gets
// entries for anything wired ONLY through this path (Animation, ActorZonedAttacker, Spawn,
// ReleaseTriggerMotion) — both SBS cores would silently run the identical substrate body, and SBS's
// byte-exact compare would "pass" without the native port ever running. See boot.cpp for the shared
// registration function + full writeup.
// Runs BEFORE crt0_setup/game_init: game_init's init prefix can itself reach a direct-substrate
// g_override[] call (e.g. FUN_80050b08's task0 bootstrap arming a scheduler slot via spawnPrim) —
// the registry must already carry these entries before any guest code runs, or a call through an
// unregistered thunk aborts on "unregistered" (found live: SBS core A crashed here before this line
// was moved above the init calls).
void dc_boot_init(Core* c) { void gte_bind(Core*); gte_bind(c); c->rsub.projprim.bind(c); spu_bind(c); mdec_bind(c); xa_bind(c); c->hooks->registerOverrides(c->game); crt0_setup(c); game_init(c); }
void dc_step_frame(Core* c, uint32_t f) { native_step_frame(c, f); }

static void game_main(Core* c) {
  void gte_bind(Core*); gte_bind(c);   // bind this core's GTE before the init prefix / frame loop
  c->rsub.projprim.bind(c);         // and this core's native depth-cache (class ProjPrim on Render)
  spu_bind(c);                          // and this core's SPU
  mdec_bind(c);                         // and this core's MDEC
  xa_bind(c);                           // and this core's XA streamer
  game_init(c);
  // --- native frame loop (replaces LAB_80050c6c). Per frame, faithful to the game-main loop
  // body but with the scheduler call FUN_80051e60 replaced by native stage stepping (added
  // incrementally). native_step_frame calls ov_frame_update DIRECTLY (PC-driven, top-down): real
  // per-frame update (still-PSX leaf FUN_800788ac) + per-vblank audio + fps60 commit + gpu_present +
  // gpu_pace_frame + satisfies the vblank pacing dwell. PSXPORT_NATIVE_FRAMES caps the run (headless). ---
  // switch (the cooperative task-switch) is wired via the platform-HLE table — see
  // PlatformHle::initBuiltins: FUN_80080880 (ChangeThread, the universal yield/task-end primitive that
  // FUN_80051f80/FUN_80051fb4 funnel through) -> switch, so a yield from an interpreted task
  // coroutine longjmps back to the native scheduler. (Was the removed address-keyed override table.)
  // BGM start/stop (FUN_80074BF8 / FUN_80074E48) are now OWNED PC-native in game/audio/music_coord.cpp
  // (sound_register, called from games_tomba2_init). The instant-CD "cut looping ingame music when a
  // dialog tone starts" hook (MusicCoord::cutIfDialog) moved into ov_sound_play_bgm there. The REPL
  // `bgm`/`bgmstop` commands still rc1/rc0 those addresses directly (now routed through the overrides).

  // Frame budget: an explicit PSXPORT_NATIVE_FRAMES always wins (headless tests). Otherwise, when
  // a window is up this is the real interactive game loop — run until the user closes the window
  // (SDL_QUIT -> exit(0) in present_window); headless with no cap defaults to 120 (CI/smoke).
  uint32_t nframes = 0;   // 0 == run until window close / REPL quit
  int repl_mode = cfg_on("PSXPORT_REPL") != 0;
  if (repl_mode) nframes = 0;                       // REPL drives frame count via `run N`
  else { if (!gpu_windowed()) nframes = 120; }  // headless smoke default
  // PSXPORT_AUTO_SKIP needs time to tap through title -> GAME -> field: raise the headless smoke cap so a
  // no-REPL run actually reaches free-roam before the loop ends (REPL runs gate frames via `run N` instead).
  if (!repl_mode && !gpu_windowed() && cfg_str("PSXPORT_AUTO_SKIP")) nframes = 1500;
  // PSXPORT_NATIVE_FRAMES: the comment above (and docs/driving-the-game.md) promised "an explicit
  // NATIVE_FRAMES always wins" but nothing ever READ the var — every headless PAD_REPLAY/no-REPL run
  // silently hit the smoke cap regardless. Explicit request now wins over every default above.
  if (!repl_mode) { int nf = cfg_int("PSXPORT_NATIVE_FRAMES", 0); if (nf > 0) nframes = (uint32_t)nf; }
  // When the debug server is up (headless, no REPL), the run is INTERACTIVELY DRIVEN over the socket
  // (rw/w16/press/shot/dumpram, step/play) — do NOT cap it, or it exits before we can drive. The
  // server's `quit` command (or SIGINT) ends it. AUTO_SKIP still auto-drives to free-roam first.
  if (!repl_mode && !gpu_windowed() && cfg_on("PSXPORT_DEBUG_SERVER")) nframes = 0;
  fprintf(stderr, "[native_boot] entering native frame loop (%s)\n",
          nframes ? "capped" : "interactive (until window close)");
  c->game->dbg_server.start(c);    // PSXPORT_DEBUG_SERVER: non-blocking live TCP debug server (dbg_server.cpp)
  long repl_budget = 0;   // frames remaining in the current REPL `run N`
  // Per-loop state carried across frames (plain locals — one frame loop per Core, no hidden globals):
  int      as_phase = -1;             // autoskip: -1 uninit, 0 reach-GAME, 1 await-cutscene, 2 skip, 3 done
  int      as_idle  = 0;              // autoskip: consecutive flag==0 frames in phase 2
  uint32_t seq_last = 0xFFFFFFFF;     // seqdbg change detector
  uint64_t state_last_sig = 0;        // `debug state` change detector
  uint32_t bgm_rd[14] = {};           // bgmtick per-slot read-pointer change detector
  uint32_t last_entry = 0;            // stage/sm change detector
  uint32_t last_sm = 0xFFFFFFFF;
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    // c->game->timing.logicFrame = f is now set centrally in native_step_frame() itself (so SBS's
    // dc_step_frame() path gets it too, not just this standalone loop).
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      // Blocking on stdin for the next command is an intentional idle, not a hang — suspend the
      // frame-progress watchdog while waiting so it doesn't fire at a paused REPL prompt.
      if (repl_budget <= 0) watchdog_suspend();
      while (repl_budget <= 0) { repl_budget = c->game->repl.read(c, f); if (repl_budget < 0) break; }
      if (repl_budget < 0) break;
      repl_budget--;
    }
    // PSXPORT_AUTO_SKIP=1 — headless AUTO-DRIVE into the real GAME free-roam FIELD (NOT the attract DEMO, NOT
    // the intro cutscene). The dead PSXPORT_AUTO_GAMEPLAY/AUTO_SKIP env vars previously did nothing, so a
    // no-input run only ever reached the attract demo (stage 0x801062E4, fully interpreted), never player
    // control. Self-contained state machine driven by the CUTSCENE-ACTIVE flag *(0x1F800137) — verified 1
    // throughout the post-NewGame intro cutscene (scripted camera pan + dialog) and 0 in free-roam:
    //   (0) REACH GAME: tap Cross until task0 enters the GAME stage (0x8010637C).
    //   (1) WAIT FOR CUTSCENE: hold until the flag first reads 1 (skips the early loading 0-window before the
    //       cutscene starts), so a flag==0 later is unambiguously free-roam, not "not started yet".
    //   (2) SKIP: the cutscene does NOT end on its own — pulse START (every ~40 frames) while the flag is 1.
    //       Start ends it (it can take a few taps until the cutscene reaches a skippable point). Pressing
    //       Start ONLY while the flag is 1 is what keeps us OUT of the pause menu (Start in free-roam opens
    //       it). Done once the flag has been 0 for a short persistence window (survives any brief beat gap).
    // Works with or without the REPL (the REPL's `run N` still gates frame budget).
    if (as_phase == -1) {
      const char* s = cfg_str("PSXPORT_AUTO_SKIP");
      as_phase = (s && strcmp(s, "0")) ? 0 : 3;
      if (as_phase == 0) fprintf(stderr, "[autoskip] armed: drive into GAME free-roam\n");
    }
    if (as_phase < 3) {
      uint32_t stg = c->mem_r32(TASKBASE + 0xc);
      uint8_t  cut = c->mem_r8(0x1F800137u);             // cutscene-active flag
      if (as_phase == 0) {                             // tap Cross until the GAME stage
        if (stg != c->cfg->stageGame) { if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x4000), 6); }
        else { as_phase = 1; fprintf(stderr, "[autoskip] reached GAME at frame %u\n", f); }
      } else if (as_phase == 1) {                      // wait for the cutscene to actually start (flag -> 1)
        if (cut) { as_phase = 2; fprintf(stderr, "[autoskip] intro cutscene up at frame %u; skipping (Start)\n", f); }
      } else {                                           // phase 2: pulse Start while the cutscene is active
        if (cut) { as_idle = 0; if ((f % 40u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x0008), 6); }
        else if (++as_idle >= 60) {   // ~2s after the flag clears: lets the cutscene-END FADE finish before
          c->game->pad.driveRelease(); as_phase = 3;   // hand-off, so a Start right after skip opens the pause menu
          fprintf(stderr, "[autoskip] free-roam reached at frame %u (cutscene ended)\n", f);   // (not mid-fade)
        }
      }
    }
    // REPL-armed auto-drive (the `newgame` / `skip` commands). One way to drive the game: pipe REPL
    // commands. `newgame` pulses Cross at the title until task0 enters the GAME prologue (0x8010637C),
    // then returns to the REPL prompt. `skip N` then pulses Start each frame for N frames to advance the
    // post-newgame fisherman dialog cutscene into the field. Manual walking = `press`/`run`/`release`.
    if (c->game->repl.navNewgame) {
      if (c->mem_r32(0x801fe00c) != c->cfg->stageGame) {
        if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x4000), 6);   // tap Cross
      } else {
        fprintf(stderr, "[repl] newgame: reached GAME prologue at frame %u\n", f);
        c->game->repl.navNewgame = 0; repl_budget = 0;                                     // back to the REPL prompt
        // Do NOT run this frame's native_step_frame: it would advance into the GAME loop body (area
        // INIT -> running sub-mode) and, with the area-code overlay not yet loaded, derail before the
        // REPL prompt regains control. `continue` freezes task0 right after the GAME prologue (before
        // INIT runs) so immediate `r`/`rw`/`dumpram` reads see a clean GAME-entry state.
        continue;
      }
    }
    if (c->game->repl.skipFrames > 0) {
      if ((f % 24u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x0008), 6);     // pulse Start
      if (--c->game->repl.skipFrames == 0) { c->game->pad.driveRelease();
        fprintf(stderr, "[repl] skip done at frame %u\n", f); }
    }
    // `warp` — fire the armed area change by writing the REAL DOOR RECORD, then letting the game's own
    // field-run state machine run its natural transition sequence. RE (engine_re.md "Area WARP", door-record
    // mechanism): a door does NOT write the destination area id (0x800bf870) directly. It writes a 2-word
    // DOOR RECORD read by the running field-run frame (Engine::fieldRun / fieldRunFaithful case 1):
    //   - 0x800BF83A (u16) = packed dest: (destArea << 8) | subState   [decoded in case 6:
    //                        0x800bf870 = byteswap(0x800BF83A) & 0x3f1f -> area=high&0x1f, sub=low&0x3f]
    //   - 0x800BF839 (u8)  = trigger type: 3 = normal cross-area door (routes case 6 -> sm[0x4c]=1, the
    //                        FULL area machine: FUN_8005245c CD-lib cleanup, then teardown+reload).
    // The running field frame sees 0x800BF839!=0 (with 0x800BF80F==0) and arms sm[0x4a]=1/4c=2/4e=6; case 6
    // decodes the record into 0x800bf870, and (trig==3) hands off to the sm[0x4c]=1 area machine which tears
    // down the old area's object tasks BEFORE swapping the overlay. This is why it is CLEAN where the old
    // forced-case0 warp was not: case0 skipped straight to FUN_80044bd4's reload while the old area's spawned
    // handlers were still registered and ran against swapped memory (bad-opcode flood). VERIFIED: same-area
    // `warp 0` = 0 recomp-miss, full respawn; cross-area teardown drops warp-1 from a 72-miss flood to a
    // single overlay-seed miss (the destination area's MODE code overlay A0<id> not yet resident — a
    // separate recompiler-seeding gap, see engine_re.md). We write the record inline at the frame-loop top;
    // no yielding call is involved (unlike the old FUN_80044bd4 direct-dispatch that deadlocked).
    if (c->game->repl.warpArmed) {
      c->game->repl.warpArmed = 0;
      const uint32_t dest = c->game->repl.warpDest & 0x1fu;
      // DEV-WARP CROSS-OVERLAY FULL LOAD (2026-07-17): warping to an area whose field CODE overlay is a
      // SEPARATE ov_a0<id> (not the resident A00 — e.g. area 3 = ov_a03, hut interior area 21 = ov_a0l)
      // crashes even the ORACLE with the old door-record warp, because a real walk-in LOADS that overlay
      // + its area DATA/tables when entering the region, while a warp jumps in cold and the door record
      // routes through nexttab -> a running state that never runs the load. So reproduce the FULL per-area
      // load directly: prime the load-task slot fields (sm[0x6e]=dest, sm[0x6d]=2 -> main DMA path) and run
      // the native area loader (= guest FUN_800452c0 body: FUN_80045080(0x80108f9c, dest+3) code overlay +
      // area DATA + reloc tables + bf870=dest), then force the field area machine into its running state
      // (sm[0x4a]=1, sm[0x4c]=nexttab[dest]). VERIFIED: warp 3 (ov_a03) now loads clean, 0 miss (was a
      // miss-crash). DEV-ONLY reachability aid on the debug warp path; normal gameplay loads it through the
      // area machine's own case-0 on region entry. (Old teardown via the door record is skipped — a warp is
      // a cold jump, not an in-game door; some stale prior-area object state may linger, acceptable for a
      // debug warp.)
      uint32_t wsm = c->mem_r32(0x1f800138u);
      c->mem_w8(wsm + 0x6e, (uint8_t)dest);
      c->mem_w8(wsm + 0x6d, 2);
      c->engine.sop.transitionAreaLoad();     // full native area load for `dest` (sets bf870=dest, loads a0<id>)
      c->mem_w16(wsm + 0x48, 2);
      c->mem_w16(wsm + 0x4a, 1);
      c->mem_w16(wsm + 0x4c, c->mem_r8(0x80108f60u + dest));
      c->mem_w16(wsm + 0x4e, 0);
      fprintf(stderr, "[repl] warp: full area load for area %u done (f%u), bf870=%u, sm[0x4c]=%u\n",
              dest, f, c->mem_r8(0x800bf870u), c->mem_r8(wsm + 0x4c));
    }
    // PSXPORT_DEBUG_SERVER pause/step: when frozen, do NOT advance the game — just pump host input
    // (keeps the window alive) and service debug commands so `step`/`play` can arrive. A `step` runs
    // exactly one real frame then re-freezes, so transient bad frames can be inspected one at a time.
    { void gpu_repaint(Core*);
      DbgServer& dbg = c->game->dbg_server;
      if (dbg.isPaused()) watchdog_suspend();   // a debug pause is intentional idle, not a hang
      while (dbg.isPaused()) {
        if (dbg.stepPending()) { dbg.consumeStep(); break; }   // run exactly one frame
        c->game->pad.serviceFrame();      // pump host input (keeps the window responsive)
        c->game->gpu.gpu_repaint();           // re-present current frame: window stays live + readback is accurate
        dbg.service(c);    // receive step/play/capture commands
        usleep(15000);
      } }
    watchdog_pet();   // re-arm the timer for THIS step (covers a step that hangs before it presents,
                      // and re-arms after an idle suspend); c_subsys.h C-linkage decl
    native_step_frame(c, f);   // one frame of deterministic guest work (steppable core; see fn above).
    // native_step_frame -> ov_frame_update OWNS present + pace + per-vblank audio (PC-driven frame body),
    // so the loop no longer needs a separate pacer here (the earlier orphaned-override stopgap is gone).
    // PSXPORT_SEQDBG — libsnd sequencer STATE trace (from SsSeqCalled @0x80090BD0): is any BGM
    // sequence OPEN/PLAYING? 0x801054B0=open-seq count, 0x80104C28=playing bitmask, 0x800AC424=tick
    // mode, 0x800AC42C=SsSeqCalled ptr. If these never go nonzero, no song is ever started → the
    // missing-BGM root cause is upstream (song open/play not happening), not the SPU/tick.
    if (cfg_dbg("seq")) {
      uint32_t st = (c->mem_r16(0x801054B0) << 16) | (c->mem_r32(0x80104C28) & 0xFFFF);
      if (st != seq_last) {
        cfg_logf("seq", "[seqdbg] f%u open=%d playmask=0x%04X tickmode=%d seqfn=0x%08X stage=0x%08X",
                f, c->mem_r16s(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF,
                c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(TASKBASE + 0xc));
        seq_last = st;
      }
    }
    // PSXPORT_DEBUG=state — RELIABLE game-state probe (replaces the old `nav` camera guess, which
    // disagreed with the screen). Dumps all 3 cooperative-task slots (state@+0x00, entry@+0x0c) so a
    // PAUSE MENU (a separate task spawned in the GAME overlay, entry 0x80108xxx, page byte that task+0x6B)
    // is DETECTABLE: when one of the 3 slots has a 0x80108xxx entry and is alive, a menu is open and
    // gameplay input is going to the menu, NOT the player. Also prints the GAME stage sm + camera pos.
    // Logged only on change of the (slot-state/entry + page) signature to stay quiet.
    // PSXPORT_DEBUG=cam — per-frame camera pos (tracks Tomba). Used to determine controls empirically:
    // hold one button and watch for a vertical (Y) excursion = a JUMP, vs a planar (X/Z) shift = walking.
    cfg_logf("cam", "f%u (%d,%d,%d)", f, c->mem_r16s(0x1f8000d2u),
              c->mem_r16s(0x1f8000d6u), c->mem_r16s(0x1f8000dau));
    if (cfg_dbg("state")) {
      uint64_t sig = 0; int menu_slot = -1; uint8_t menu_page = 0;
      for (int i = 0; i < 3; i++) {
        uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
        uint16_t st = c->mem_r16(base);
        uint32_t ent = c->mem_r32(base + 0xc);
        sig = sig * 1099511628211ull + ((uint64_t)st << 32 | ent);
        if (st && (ent & 0xFFFFF000u) == 0x80108000u) { menu_slot = i; menu_page = c->mem_r8(base + 0x6bu); }
      }
      sig = sig * 31 + ((uint64_t)menu_slot << 8 | menu_page);
      if (sig != state_last_sig) {
        state_last_sig = sig;
        fprintf(stderr, "[state] f%u", f);
        for (int i = 0; i < 3; i++) {
          uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
          fprintf(stderr, " | s%d st=%u ent=0x%08X", i, c->mem_r16(base), c->mem_r32(base + 0xc));
        }
        fprintf(stderr, "  MENU=%s", menu_slot >= 0 ? "OPEN" : "no");
        if (menu_slot >= 0) fprintf(stderr, "(slot%d page=%u)", menu_slot, menu_page);
        fprintf(stderr, "  cam=(%d,%d,%d) sm[0x4a]=%u\n",
                c->mem_r16s(0x1f8000d2u), c->mem_r16s(0x1f8000d6u),
                c->mem_r16s(0x1f8000dau), c->mem_r16(0x801fe000u + 0x4au));
      }
    }
    // BGM-active probe (PSXPORT_BGMDBG): each frame scan the 14 libsnd sequence slots
    // (0x800be3d8 + i*0xB0) for the active/play flag (+0x98 bit0). For any active slot, log
    // its read pointer (+0x00) vs base (+0x04) — if the read ptr ADVANCES frame-to-frame the
    // sequence is genuinely ticking (audible); if it stays == base the SsSeqCalled tick isn't
    // advancing it (frozen, the handoff's hypothesis). Scene-independent: catches any window
    // where a BGM is active, without needing to reach a specific scene.
    if (cfg_str("PSXPORT_BGMDBG")) {
      for (int i = 0; i < 14; i++) {
        uint32_t s = 0x800be3d8u + (uint32_t)i * 0xB0u;
        uint32_t flag = c->mem_r32(s + 0x98), rd = c->mem_r32(s);
        if ((flag & 1) && rd != bgm_rd[i]) {
          fprintf(stderr, "[bgmtick] f%u slot%d active rdptr=%08X base=%08X (%+d)\n",
                  f, i, rd, c->mem_r32(s + 4), (int)(rd - c->mem_r32(s + 4)));
          bgm_rd[i] = rd;
        }
        if (!(flag & 1)) bgm_rd[i] = 0;
      }
    }
    uint32_t t0e = c->mem_r32(TASKBASE + 0xc), s48 = c->mem_r16(TASKBASE + 0x48);
    // GAME runs a 4-level nested state machine (task +0x48/4a/4c/4e). Track all of it so a
    // stuck leaf is visible, not just the outer s48.
    uint32_t sm = (c->mem_r16(TASKBASE+0x48)<<24)|(c->mem_r16(TASKBASE+0x4a)<<16)|
                  (c->mem_r16(TASKBASE+0x4c)<<8)|c->mem_r16(TASKBASE+0x4e)
                  ^ (c->mem_r16(TASKBASE+0x50)<<12)^(c->mem_r16(TASKBASE+0x52)<<4);
    if (t0e != last_entry || sm != last_sm) {
      const char* stg = t0e == c->cfg->stageStart ? "START" : t0e == c->cfg->stageDemo ? "DEMO" :
                        t0e == c->cfg->stageGame ? "GAME" : "?";
      fprintf(stderr, "[native_boot]   frame %u: stage=%s(0x%08X) sm[48=%u 4a=%u 4c=%u 4e=%u 50=%u 52=%u]"
              " @0x80109450=%08X\n",
              f, stg, t0e, c->mem_r16(TASKBASE+0x48), c->mem_r16(TASKBASE+0x4a),
              c->mem_r16(TASKBASE+0x4c), c->mem_r16(TASKBASE+0x4e), c->mem_r16(TASKBASE+0x50),
              c->mem_r16(TASKBASE+0x52), c->mem_r32(0x80109450));
      last_entry = t0e; last_sm = sm;
    }
    // One-shot: when GAME has settled, dump the CD-streaming contract (FUN_8001cfc8, task
    // slot 2). task2 obj @0x801fe0e0; +0x54=start LBA, +0x58=end LBA (= globals
    // DAT_801fe134/138). DAT_801fe146=channel/type. _DAT_1f8001f8=dest, _DAT_1f8001f4=words.
    if (cfg_dbg("stream") && t0e == c->cfg->stageGame && f == 75) {
      cfg_logf("stream", "[streamdbg] task2 obj @0x801fe0e0 state=%u entry=0x%08X",
              c->mem_r16(0x801fe0e0), c->mem_r32(0x801fe0ec));
      cfg_logf("stream", "[streamdbg] startLBA(+54/801fe134)=%u endLBA(+58/801fe138)=%u "
              "chan(801fe146)=%u be0e4=0x%02X",
              c->mem_r32(0x801fe134), c->mem_r32(0x801fe138), c->mem_r8(0x801fe146), c->mem_r8(0x800be0e4));
      cfg_logf("stream", "[streamdbg] dest(_DAT_1f8001f8)=0x%08X words(_DAT_1f8001f4)=%u "
              "f0=%u f1f800224=0x%08X",
              c->mem_r32(0x1f8001f8), c->mem_r32(0x1f8001f4), c->mem_r32(0x1f8001f0), c->mem_r32(0x1f800224));
    }
    // PSXPORT_RAMDUMP_FRAME=N — dump RAM mid-run at native frame N (overlay state during gameplay
    // differs from end-of-run; needed to disasm the LIVE level/stage overlay at 0x8010/0x8011xxxx).
    { const char* rdf = cfg_str("PSXPORT_RAMDUMP_FRAME");
      if (rdf && f == (uint32_t)strtoul(rdf, 0, 0)) {
        
        const char* rd = cfg_str("PSXPORT_RAMDUMP"); if (!rd) rd = "scratch/bin/midrun_ram.bin";
        FILE* mf = fopen(rd, "wb");
        if (mf) { fwrite(c->ram, 1, 0x200000, mf); fclose(mf);
                  fprintf(stderr, "[native_boot] mid-run RAM dump @frame %u -> %s\n", f, rd); }
      } }
    if (cfg_dbg("schedf"))
      cfg_logf("schedf", "f%u t0[st=%u e=%08X s48=%u s4a=%u s4c=%u s5c=%u] t1[st=%u] t2[st=%u]",
              f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48),
              c->mem_r16(TASKBASE + 0x4a), c->mem_r16(TASKBASE + 0x4c), c->mem_r16(TASKBASE + 0x5c),
              c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0));
    else if (f < 10 || (f % 30) == 0)
      fprintf(stderr, "[native_boot]   frame %u: t0[st=%u e=0x%08X s48=%u] t1[st=%u] t2[st=%u] "
                      "f135=%u\n", f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc),
              c->mem_r16(TASKBASE + 0x48), c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0),
              c->mem_r8(0x1f800135));
    c->game->dbg_server.service(c);  // service one queued live-debug-server command (non-blocking)
  }
  fprintf(stderr, "[native_boot] frame loop done; task0 state=%u entry=0x%08X obj+0x48=%u\n",
          c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48));
  const char* rd = cfg_str("PSXPORT_RAMDUMP");
  if (rd) {
    
    FILE* f = fopen(rd, "wb");
    if (f) { fwrite(c->ram, 1, 0x200000, f); fclose(f);
             fprintf(stderr, "[native_boot] dumped 2MB RAM -> %s\n", rd); }
  }
}

// Wired from boot.c when PSXPORT_NATIVE_BOOT is set. Registers the main override and enters
// crt0; crt0's call to FUN_80050b08 lands in game_main.
void native_boot_run(Core* c) {
  { void cfg_dump(void); cfg_dump(); }   // log active PSXPORT_* config once (see docs/config.md)
  // PSXPORT_ORACLE — THE PURE PSX REFERENCE (user 2026-07-08). recomp gameplay + UNENHANCED PSX render:
  // the substrate's own GTE+OT+GP0 composited in painter order, with NO native enhancement able to touch
  // the picture — no fps60 interpolation, no widescreen FOV/stretch, no native depth/obj_depth compositing,
  // no RenderObserver tagging. THE trustworthy oracle for byte + picture comparison. Implies GATE +
  // RENDER_PSX; the render-internal enhancement gates (painter order, wide_engine, observer) consult
  // oracle_mode() so nothing can leak an enhancement into it. Forced here BEFORE the GATE/RENDER_PSX
  // blocks so their log lines report the final state.
  if (oracle_mode()) {
    c->game->setOracle();                        // per-Game: recomp gameplay + mods forced neutral
    c->rsub.mode.setPsxRender(true);         // full PSX OT walk — not the native scene pass
    fprintf(stderr, "[native_boot] PSXPORT_ORACLE=1 — pure recomp + pure PSX render "
                    "(no fps60 / wide / native-depth / observer)\n");
  }
  // PSX-fallback gate (diagnostic): boot + frame-loop stay native; everything the loop calls runs as PSX
  // recomp (sync CD). PSXPORT_GATE nonzero turns it on; the REPL `gate on|off` toggles it live.
  { const char* g = cfg_str("PSXPORT_GATE");
    if (g && *g) c->game->psx_fallback = (atoi(g) != 0);
    fprintf(stderr, "[native_boot] psx_fallback=%d (%s)\n", c->game->psx_fallback,
            c->game->psx_fallback ? "native boot+frameloop, PSX everything else (sync)" : "full native"); }
  // RENDER-path compare switch: PSXPORT_RENDER_PSX renders the field via the PSX recomp path (native state).
  // Per-Core now (Render::mPsxRender) — was the process-global g_render_psx.
  { const char* r = cfg_str("PSXPORT_RENDER_PSX");
    if (r && *r) c->rsub.mode.setPsxRender(atoi(r) != 0);
    if (c->rsub.mode.psxRender()) fprintf(stderr, "[native_boot] Render::psxRender=1 (field render via PSX recomp path)\n"); }
  // DUAL-VIEW: render the SAME game state TWICE per frame — engine-native (left) + PSX-recomp (right) — and
  // composite side by side. Set at launch (PSXPORT_DUALVIEW=1) so the GPU allocates two geometry batches.
  { const char* r = cfg_str("PSXPORT_DUALVIEW");
    if (r && *r) c->rsub.mode.setDualview(atoi(r) != 0);
    if (c->rsub.mode.dualview()) fprintf(stderr, "[native_boot] Render::dualview=1 (side-by-side native | PSX render)\n"); }
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo (LOGO.STR) -> opening movie (OP.STR) ->
  // title/menu. The game's own STR streaming (strNext) TIMES OUT under our runtime (we don't feed
  // CD-streamed FMV sectors to its StrPlayer — see "time out in strNext()" in the DEMO stage), so the
  // movies are played here with our self-contained native FMV player (native_fmv.c).
  // SPLIT OF OWNERSHIP: only LOGO.STR (the Whoopee logo, which plays BEFORE the front-end overlay is
  // even loaded) is played at boot. OP.STR (the opening movie) is OWNED BY THE FRONT-END — the DEMO
  // menu machine's states 4..7 ARE the OP.STR sequence (demo.cpp demo_menu_machine), which now
  // plays it via fmv.play. Playing OP here too made it play TWICE (boot + front-end) — the
  // "FMV repeats" bug. Boot plays LOGO; the front-end plays OP -> SCEA->LOGO->OP->title, no repeat.
  // Skip the intro FMVs when there's no viewer: PSXPORT_NO_FMV, OR any headless run (a headless probe
  // has nobody watching — playing/decoding the intro movies just burns wall-clock; a field probe went
  // from ~77s to ~1.4s). The in-game/cutscene FMVs that still play are also auto-uncapped in headless
  // (native_fmv.c) so they fast-forward. Set PSXPORT_NO_FMV=0 explicitly to force them on if ever needed.
  int skip_fmv = cfg_on("PSXPORT_NO_FMV") || cfg_on("PSXPORT_VK_HEADLESS");
  const char* nf_ov = cfg_str("PSXPORT_NO_FMV");
  if (nf_ov && atoi(nf_ov) == 0 && *nf_ov) skip_fmv = 0;     // explicit PSXPORT_NO_FMV=0 forces FMVs on
  if (!skip_fmv) {
    fprintf(stderr, "[native_boot] playing boot FMV (Whoopee logo); OP.STR is the front-end's\n");
    c->game->fmv.play("MOVIE/LOGO.STR");
  } else {
    fprintf(stderr, "[native_boot] skipping intro FMVs (headless/NO_FMV)\n");
  }
  // Clean hand-off to the front-end (issues #7/#11): black the display FB before the title builds, so the
  // title's first frames (drawn over several frames while its background/font/CLUT upload) never composite
  // over the stale SCEA white-fill or an FMV last-frame. Covers the no-FMV-ran case too (the stub splash
  // fill is still resident in s_vram even when both intros are skipped). Deterministic, no timer.
  void gpu_clear_display(Core*);
  gpu_clear_display(c);
  fprintf(stderr, "[native_boot] entering native crt0 (PC-driven)\n");
  native_crt0(c);
  fprintf(stderr, "[native_boot] returned from native crt0\n");
}
