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
#include "game.h"      // SchedulerState (per-instance cooperative-task state) reached via c->game->sched
#include "hw_bind.h"   // spu_bind/mdec_bind/cdc_bind/xa_bind (per-instance HW-peripheral binders)
#include "scheduler.h" // ov_switch/native_scheduler_step + TASKBASE/TASKSTRIDE/CUR_TASK (scheduler.cpp)
#include "c_subsys.h"
#include "cfg.h"
#include "asset.h"     // class Asset — c->engine.asset (unpackGroup / uploadImage / preload*)
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
#include "repl.h"       // native_repl_read + the REPL-armed g_nav_* auto-drive state

// Native XA voice/BGM clip player (xa_stream.c) owns task slot 2 — it replaced the FUN_8001cfc8
// streaming-reader coroutine. The scheduler skips slot 2 while owned and reflects clip completion
// into the task-2 state byte (the cutscene waits `while (DAT_801fe0e0 != 0)`).
// class MusicCoord (game/audio/music_coord.h) — reached as c->engine.musicCoord.tick() per frame
void xa_audio_trace(Core* c, const char* tag);    // CD-vol fade + XA lifecycle trace (cd_override.cpp)

// The native cooperative-task scheduler (ov_switch + native_scheduler_step — the FUN_80080880/
// FUN_80051e60 replacements) now lives in scheduler.cpp/scheduler.h; TASKBASE/TASKSTRIDE/CUR_TASK
// (used by the REPL/debug state probes below) are reached via "scheduler.h".
extern "C" void ffspan_reset_frame(void), ffspan_begin(void), ffspan_end(const char*);  // PSXPORT_BDTAG (engine_stage.cpp); native_step_frame below still calls these directly

// ---- BGM frame counter (PSXPORT_BGMDBG trace shared with cd_override.cpp) --------------------
// FUN_80074BF8(idx) starts BGM #idx; FUN_80074E48() stops it. These are now OWNED PC-native by
// engine/sound.cpp (sound_register), which also carries the instant-CD dialog-music cut hook. Only
// the shared frame counter remains here (cd_override.cpp externs it for its own BGM trace).
volatile uint32_t g_bgm_frame = 0;   // current logic frame (extern: cd_override.cpp trace)
void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

// ONE frame of deterministic guest work — the steppable core of the native frame loop, factored out so
// the in-process dual-core diff can step TWO cores in lockstep (it calls this on `a` then `b`). It is
// EXACTLY the guest-mutating body of the loop below (per-frame IRQ events, draw/display-env setup, the
// FUN_800788ac frame update + native scheduler pass + dialog-music coord + draw sync + buffer flip); the
// loop's driver scaffolding (REPL, auto-navigation/input, pause/step, diagnostics, dbg_server) stays in
// the loop and runs ONCE around this call. No input is injected here — drive pads before calling it.
// gpu_perf.cpp — per-frame CPU phase / frame-time profiler (REPL `debug perf`), default off.
extern "C" void perf_frame_begin(void), perf_mark_pre(void), perf_frame_end(void),
                perf_phase_begin(int), perf_phase_end(int);

// ---- DUAL-VIEW guest-state snapshots (native | PSX side-by-side render of ONE game state) -------------
// Save/restore of full guest state (main RAM + scratchpad + GTE regs) lives in dualview_snapshot.cpp
// (dv_snapshot / dv_capture_post / dv_restore_pre / dv_restore_post + g_dv_have_pre). See
// native_step_frame's dual-view block below for how the sequence is driven.
extern "C" int g_dualview;       // defined in engine_render.cpp
extern "C" int g_sbs;            // defined in sbs.cpp (PSXPORT_SBS two-core side-by-side harness)

static void native_step_frame(Core* c, uint32_t f) {
  void gte_bind(Core*); gte_bind(c);   // bind THIS core's GTE register file (per-instance — no shared GTE)
  spu_bind(c);                          // bind THIS core's SPU state (per-instance — no shared SPU)
  mdec_bind(c);                         // bind THIS core's MDEC state (per-instance — no shared MDEC)
  cdc_bind(c);                          // bind THIS core's CD-controller registers (per-instance — no shared CD)
  xa_bind(c);                           // bind THIS core's XA streamer state (per-instance — no shared XA)
  ffspan_reset_frame();   // backdrop-attribution: reset the per-frame builder span table
  void gpu_set_disp_origin(Core* c, int x, int y);
  (void)f;
  perf_frame_begin();   // perf: start the frame clock (top of the deterministic per-frame work)
  // Advance the libetc VSync counter (DAT_800abde0) — one vblank per native frame. VSync(0) is trapped,
  // so this is the only thing that ticks the recomp timebase (recomp tasks read it to pace animations).
  c->game->timing.frameTick();
  // Per-frame IRQ-driven events the game's waits poll via TestEvent (VBlank classes + sound-DMA-complete).
  c->game->hle.deliverEvent(0xF2000003u, 0xFFFFFFFFu);
  c->game->hle.deliverEvent(0xF0000001u, 0xFFFFFFFFu);
  c->game->hle.deliverEvent(0xF0000009u, 0xFFFFFFFFu);
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
  uint32_t envp = 0x800e80a8u + parity * 0x2070u;            // the one VRAM region we DRAW into every frame
  c->mem_w32(0x800ed8c8, envp);                               // PTR_DAT_800ed8c8 (OT base, now constant)
  rc2(c, 0x80081458, envp, 0x800);                            // ClearOTagR(ot, 0x800)
  c->mem_w16(0x800e809c, 0);                                  // DAT_800e809c = 0 (dwell counter)
  c->mem_w32(0x800bf4f4, c->mem_r32(0x800bf544));             // keep last pool ptr (read by some submitters)
  c->mem_w32(0x800bf544, (parity * 0x14000 + 0x800bfe68) & 0xffffff);   // packet pool (now constant)
  c->game->pad.serviceFrame();                                       // host input -> game pad buffer (pre-read)
  xa_audio_trace(c, "pre");                                   // CD-vol fade state BEFORE tick+mix
  perf_mark_pre();   // perf: charge the pre-tick host work (input/IRQ/OT-clear) to `pre`
  // PC-driven frame body: per-frame state update (still-PSX leaf) + per-vblank audio + fps60 commit +
  // present + pace. Called as a plain C call (top-down PC-driven model) — NOT an override. This is where
  // gpu_present / gpu_pace_frame / the per-vblank sequencer+SPU tick are reached every live frame; before
  // this was wired they were orphaned with the override table (a live window ran uncapped + stale). The
  // present happens here (before the OT submit below) so the VK batch shown is the one DrawOTag built last
  // frame, exactly as the override-era ordering did.
  ffspan_begin();
  c->engine.frameUpdate();                                    // tick + per-vblank audio + present + pace
  ffspan_end("frameupd");
  xa_audio_trace(c, "post");                                  // CD-vol fade state AFTER tick+mix
  perf_phase_begin(3);   // perf: SCHED-LOGIC = the cooperative scheduler step (the real per-frame GAME logic)
  // The native scheduler is the frame-loop's task-stepping HARNESS (no BIOS threads — yields are setjmp/
  // longjmp coroutines, CD loads are synchronous). It stays native at every gate level. What the gate
  // controls is whether the TASK BODIES it steps run as native stage dispatchers + content (full native) or
  // as pure PSX recomp coroutines (psx_fallback on) — see the gate checks inside native_scheduler_step.
  ffspan_begin();
  native_scheduler_step(c);                                   // <- replaces FUN_80051e60 (BIOS scheduler)
  ffspan_end("scheduler");
  perf_phase_end(3);
  c->engine.musicCoord.tick();                                // dialogs stop/restore ingame music
  xa_audio_trace(c, "coord");                                 // CD-vol fade state AFTER coord
  rc1(c, 0x80080f6c, 0);                                      // draw sync
  rc0(c, 0x800506d0);                                         // task sleep-countdown (re-arm 1->2)
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
    rc1(c, 0x800815d0, envp + 0x2014);                        // PutDrawEnv (draw area/offset/clip for page 0)
    gpu_set_disp_origin(c, 0, 0);                             // PC-native: present scans the page we draw
    // DrawOTag, PC-native: call Engine::drawOTag DIRECTLY (top-down) instead of interpreting the PSX FUN_80081560.
    // drawOTag walks the OT to ENUMERATE the leftover guest prims (its draw ORDER is discarded), QUEUES
    // them into the engine render queue (rq_active() is always on), then rq_flush()es the queue in ENGINE
    // order. The interpreted PSX DrawOTag did the walk+queue but NOT the flush (rq_flush only lives in
    // Engine::drawOTag, orphaned by the override-table removal) — so the queue filled every frame and never
    // drained, and NOTHING 2D reached the VK renderer (the whole front-end rendered black). OT head arg.
    c->engine.drawOTag(envp + 0x1ffcu);
    // ---- DUAL-VIEW second render pass: render the SAME game state via the PSX recomp path into render
    // target 1 (right panel). The engine render is NOT idempotent (its per-frame queues/OT get consumed),
    // so the PSX pass must run from the PRE-render state captured in ov_field_frame (dv_snapshot, before
    // the native render ran), not from the post-native-render state. We then restore the POST-FRAME state
    // so the canonical game (which includes the post-render per-frame area update) is undisturbed.
    extern int g_dualview, g_dv_have_pre; extern void gpu_gpu_select_target(int);
    // SBS owns BOTH panes (core A | core B); its target-1 batch is core B's render, NOT a PSX re-render of
    // THIS core — so skip the in-engine dualview second pass. g_sbs declared at file scope below.
    if (g_dualview && g_dv_have_pre && !g_sbs) {
      dv_capture_post(c);            // save the real post-frame canonical state
      dv_restore_pre(c);             // rewind to the pre-render (post-gameplay) state the PSX pass needs
      rc2(c, 0x80081458, envp, 0x800);                          // ClearOTagR(ot, 0x800)
      c->mem_w32(0x800ed8c8, envp);                             // OT base
      c->mem_w16(0x800e809c, 0);                                // dwell counter
      c->mem_w32(0x800bf4f4, c->mem_r32(0x800bf544));
      c->mem_w32(0x800bf544, (0x800bfe68u) & 0xffffff);         // reset packet pool ptr
      gpu_gpu_select_target(1);
      rec_dispatch(c, 0x8003f9a8u);                             // PSX field render orchestrator (full OT build)
      rec_dispatch(c, 0x8010810cu);                             // render submit (faithful to ov_field_frame)
      c->engine.drawOTag(envp + 0x1ffcu);                       // walk PSX OT -> target-1 batch
      gpu_gpu_select_target(0);
      dv_restore_post(c);            // restore the real canonical state (PSX pass fully undone)
      g_dv_have_pre = 0;
    }
  }
  perf_frame_end();   // perf: close the frame (post-tick remainder + full wall time) + emit rolling avg
}

// Native override of game-main FUN_80050b08: init prefix, then (later) native frame loop.

static void ov_game_main(Core* c);

// PC-native crt0 (faithful reimplementation of FUN_800896E0): BSS-zero, heap setup, then a DIRECT
// call to ov_game_main — the top of the top-down PC-driven spine. Replaces the old bootstrap flip
// (crt0 jal main -> override). The libc/heap init at 0x80089860 stays a dispatched PSX leaf.
// crt0 register/heap setup only (no main call) — shared by native_crt0 and the dual-core harness.
static void crt0_setup(Core* c) {
  for (uint32_t a = 0x800be0d8; a < 0x80106228; a += 4) c->mem_w32(a, 0);   // BSS zero
  uint32_t v0 = c->mem_r32(0x800a3f88) - 8;          // stack top base
  uint32_t sp = v0 | 0x80000000u;
  uint32_t a0 = 0x80106228u & 0x1FFFFFFFu;           // 0x00106228 (heap base, masked)
  uint32_t v1 = c->mem_r32(0x800a3f8c);
  uint32_t heapsz = (v0 - v1) - a0;
  c->mem_w32(0x800abef8, heapsz);                    // heap size
  a0 |= 0x80000000u;                                 // 0x80106228
  c->mem_w32(0x800abef4, a0);                        // heap base
  c->r[28] = 0x800be0d4u;                            // gp
  c->r[29] = sp; c->r[30] = sp;                      // sp, fp
  c->r[4]  = a0 + 4;                                 // a0 for the init call
  rec_dispatch(c, 0x80089860u);                      // libc/heap init (PSX leaf) — keep dispatched
}

static void native_crt0(Core* c) {
  crt0_setup(c);
  ov_game_main(c);                                   // DIRECT PC call (was: crt0 jal main -> flip)
}

void native_task0_bootstrap(Core* c);   // engine_stage.cpp

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop). Factored out
// of ov_game_main so the dual-core harness can init two cores then drive the frame loop itself.
static void ov_game_init(Core* c) {
  fprintf(stderr, "[native_boot] FUN_80050b08 override: running init prefix\n");

  // --- init prefix, transcribed from FUN_80050b08 (no scheduler loop) ---
  rc0(c, 0x80089788);
  rc0(c, 0x80085b20);
  // CD init: native HLE, NOT the recomp libcd (FUN_800898a0). The recomp CdInit busy-waits on the
  // CD-controller reset handshake (no IRQ ever acks → 5 retries → "CD timeout" → "Init failed").
  // We model no CD controller; all CD ops are native synchronous (cd_override.cpp). (was rc0 0x800898a0)
  { void cd_hle_init(Core*); cd_hle_init(c); }
  rc1(c, 0x80080bf0, 3);
  rc1(c, 0x80080d64, 0);
  rc1(c, 0x80080ed4, 1);
  rc1(c, 0x800865f0, 0);
  // Engine frame-state + camera init reimplemented PC-native (engine/engine_init.cpp), replacing the
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
  // call libetc VSync. Any code that reaches VSync now TRAPS (sync_overrides.cpp ov_vsync_trap).)
  // FUN_80075130 font/text init reimplemented PC-native (engine/engine_font.cpp): owns the orchestration +
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
  native_task0_bootstrap(c);   // PC-native: was rc0(c, 0x800499e8) — CD subtree owned top-down
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          c->mem_r32(0x80106228), c->mem_r32(0x8010649c), c->mem_r16(0x801fe000), c->mem_r32(0x801fe00c));
}

// Dual-core harness hooks (dualcore.cpp): boot a core to the start of the frame loop, then step it one
// frame at a time. dc_boot_init = crt0 setup + the init prefix/bootstrap; dc_step_frame = one frame.
void dc_boot_init(Core* c) { void gte_bind(Core*); gte_bind(c); spu_bind(c); mdec_bind(c); cdc_bind(c); xa_bind(c); crt0_setup(c); ov_game_init(c); }
void dc_step_frame(Core* c, uint32_t f) { native_step_frame(c, f); }

static void ov_game_main(Core* c) {
  void gte_bind(Core*); gte_bind(c);   // bind this core's GTE before the init prefix / frame loop
  spu_bind(c);                          // and this core's SPU
  mdec_bind(c);                         // and this core's MDEC
  cdc_bind(c);                          // and this core's CD-controller registers
  xa_bind(c);                           // and this core's XA streamer
  ov_game_init(c);
  // --- native frame loop (replaces LAB_80050c6c). Per frame, faithful to the game-main loop
  // body but with the scheduler call FUN_80051e60 replaced by native stage stepping (added
  // incrementally). native_step_frame calls ov_frame_update DIRECTLY (PC-driven, top-down): real
  // per-frame update (still-PSX leaf FUN_800788ac) + per-vblank audio + fps60 commit + gpu_present +
  // gpu_pace_frame + satisfies the vblank pacing dwell. PSXPORT_NATIVE_FRAMES caps the run (headless). ---
  // ov_switch (the cooperative task-switch) is wired via the platform-HLE table — see
  // sync_overrides_init: FUN_80080880 (ChangeThread, the universal yield/task-end primitive that
  // FUN_80051f80/FUN_80051fb4 funnel through) -> ov_switch, so a yield from an interpreted task
  // coroutine longjmps back to the native scheduler. (Was the removed address-keyed override table.)
  // BGM start/stop (FUN_80074BF8 / FUN_80074E48) are now OWNED PC-native by engine/sound.cpp
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
  // When the debug server is up (headless, no REPL), the run is INTERACTIVELY DRIVEN over the socket
  // (rw/w16/press/shot/dumpram, step/play) — do NOT cap it, or it exits before we can drive. The
  // server's `quit` command (or SIGINT) ends it. AUTO_SKIP still auto-drives to free-roam first.
  if (!repl_mode && !gpu_windowed() && cfg_on("PSXPORT_DEBUG_SERVER")) nframes = 0;
  fprintf(stderr, "[native_boot] entering native frame loop (%s)\n",
          nframes ? "capped" : "interactive (until window close)");
  void dbg_server_start(Core* c); void dbg_server_service(Core* c);
  dbg_server_start(c);    // PSXPORT_DEBUG_SERVER: non-blocking live TCP debug server (dbg_server.c)
  long repl_budget = 0;   // frames remaining in the current REPL `run N`
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    g_bgm_frame = f;
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      // Blocking on stdin for the next command is an intentional idle, not a hang — suspend the
      // frame-progress watchdog while waiting so it doesn't fire at a paused REPL prompt.
      if (repl_budget <= 0) watchdog_suspend();
      while (repl_budget <= 0) { repl_budget = native_repl_read(c, f); if (repl_budget < 0) break; }
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
    static int  s_as_phase = -1;     // -1 uninit, 0 reach-GAME, 1 await-cutscene, 2 skip-cutscene, 3 done
    static int  s_as_idle  = 0;      // consecutive flag==0 frames in phase 2
    if (s_as_phase == -1) {
      const char* s = cfg_str("PSXPORT_AUTO_SKIP");
      s_as_phase = (s && strcmp(s, "0")) ? 0 : 3;
      if (s_as_phase == 0) fprintf(stderr, "[autoskip] armed: drive into GAME free-roam\n");
    }
    if (s_as_phase < 3) {
      uint32_t stg = c->mem_r32(TASKBASE + 0xc);
      uint8_t  cut = c->mem_r8(0x1F800137u);             // cutscene-active flag
      if (s_as_phase == 0) {                             // tap Cross until the GAME stage
        if (stg != 0x8010637Cu) { if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x4000), 6); }
        else { s_as_phase = 1; fprintf(stderr, "[autoskip] reached GAME at frame %u\n", f); }
      } else if (s_as_phase == 1) {                      // wait for the cutscene to actually start (flag -> 1)
        if (cut) { s_as_phase = 2; fprintf(stderr, "[autoskip] intro cutscene up at frame %u; skipping (Start)\n", f); }
      } else {                                           // phase 2: pulse Start while the cutscene is active
        if (cut) { s_as_idle = 0; if ((f % 40u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x0008), 6); }
        else if (++s_as_idle >= 60) {   // ~2s after the flag clears: lets the cutscene-END FADE finish before
          c->game->pad.driveRelease(); s_as_phase = 3;   // hand-off, so a Start right after skip opens the pause menu
          fprintf(stderr, "[autoskip] free-roam reached at frame %u (cutscene ended)\n", f);   // (not mid-fade)
        }
      }
    }
    // REPL-armed auto-drive (the `newgame` / `skip` commands). One way to drive the game: pipe REPL
    // commands. `newgame` pulses Cross at the title until task0 enters the GAME prologue (0x8010637C),
    // then returns to the REPL prompt. `skip N` then pulses Start each frame for N frames to advance the
    // post-newgame fisherman dialog cutscene into the field. Manual walking = `press`/`run`/`release`.
    if (g_nav_newgame) {
      if (c->mem_r32(0x801fe00c) != 0x8010637Cu) {
        if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x4000), 6);   // tap Cross
      } else {
        fprintf(stderr, "[repl] newgame: reached GAME prologue at frame %u\n", f);
        g_nav_newgame = 0; repl_budget = 0;                                     // back to the REPL prompt
        // Do NOT run this frame's native_step_frame: it would advance into the GAME loop body (area
        // INIT -> running sub-mode) and, with the area-code overlay not yet loaded, derail before the
        // REPL prompt regains control. `continue` freezes task0 right after the GAME prologue (before
        // INIT runs) so immediate `r`/`rw`/`dumpram` reads see a clean GAME-entry state.
        continue;
      }
    }
    if (g_skip_frames > 0) {
      if ((f % 24u) == 0) c->game->pad.driveTap((uint16_t)(0xFFFF & ~0x0008), 6);     // pulse Start
      if (--g_skip_frames == 0) { c->game->pad.driveRelease();
        fprintf(stderr, "[repl] skip done at frame %u\n", f); }
    }
    // `warp` — fire the armed area change. We replicate the NON-yielding essential body of the GAME-stage
    // area-change call FUN_80044bd4(0x800452c0, dest, mode=0, phase=2). FUN_80044bd4 cannot be rec_dispatched
    // directly here: it has cooperative FUN_80051f80(1) yields (waiting for in-flight CD-DMA on 0x801fe070)
    // that deadlock when invoked outside a real task run (verified: a direct dispatch hangs). Its meaningful
    // work in a steady field (DMA idle) is just: (1) FUN_80052010(2) kill sub-task slot 2; (2) write the
    // dest area id into task0[0x6e] and the load mode into task0[0x6d]; (3) FUN_80051f14(1, 0x800452c0)
    // restart AREA-LOAD TASK slot 1 at its entry. The cooperative area-load task then runs naturally over
    // the following frames (commits 0x800bf870=translate(dest), pulls the area overlay from the table at
    // 0x800be118[id+3], walks the asset table) — no nested-yield hazard. Both callees are non-yielding.
    if (g_warp_armed) {
      g_warp_armed = 0;
      // The IN-GAME area reload (no DEMO/title bounce) is driven by the GAME-stage steady handler
      // 0x801088d8 case sm[0x4c]==0 (running sub-mode sm[0x4a]==1): it calls
      // FUN_80044bd4(0x800452c0, a1=lbu@0x800bf870, a2=0, a3=2) IN-CONTEXT (task0), so FUN_80044bd4's
      // cooperative FUN_80051f80 yields work correctly — task0 yields, the area-load task (slot 1) pulls the
      // new overlay, task0 resumes. (Restarting the load task ourselves, or rec_dispatching FUN_80044bd4
      // from the frame-loop top, both fail: the former corrupts the live area -> bad opcodes, the latter
      // deadlocks on the yield outside a task run — both verified.) So we DON'T do the load here: we seed the
      // destination into the area id global 0x800bf870 (case0's a1) and set sm[0x4a]=1, sm[0x4c]=0 so the
      // GAME stage runs case0 itself next frame and loads the dest area. (sm[0x4c]==4 -> FUN_80052078(1) is
      // the title/DEMO teardown, NOT an area-to-area warp — verified it bounces to stage DEMO.)
      const uint32_t TASK0 = 0x801fe000u;
      fprintf(stderr, "[repl] warp: dest=%u at f%u (cur area=%u) -> seed area id + drive GAME case0\n",
              g_warp_dest, f, c->mem_r8(0x800bf870u));
      c->mem_w8(0x800bf870u, (uint8_t)g_warp_dest);    // area id global = dest (case0 passes it to FUN_80044bd4)
      c->mem_w16(TASK0 + 0x4au, 1);                    // sm[0x4a] = 1 (running sub-mode -> handler 0x801088d8)
      c->mem_w16(TASK0 + 0x4cu, 0);                    // sm[0x4c] = 0 -> case0 = FUN_80044bd4 area-load
      fprintf(stderr, "[repl] warp: drove GAME case0; sm[0x4a]=%u sm[0x4c]=%u — run frames to load\n",
              c->mem_r16(TASK0 + 0x4au), c->mem_r16(TASK0 + 0x4cu));
    }
    // PSXPORT_DEBUG_SERVER pause/step: when frozen, do NOT advance the game — just pump host input
    // (keeps the window alive) and service debug commands so `step`/`play` can arrive. A `step` runs
    // exactly one real frame then re-freezes, so transient bad frames can be inspected one at a time.
    { int dbg_is_paused(void), dbg_step_pending(void); void dbg_consume_step(void); void gpu_repaint(Core*);
      if (dbg_is_paused()) watchdog_suspend();   // a debug pause is intentional idle, not a hang
      while (dbg_is_paused()) {
        if (dbg_step_pending()) { dbg_consume_step(); break; }   // run exactly one frame
        c->game->pad.serviceFrame();      // pump host input (keeps the window responsive)
        gpu_repaint(c);           // re-present current frame: window stays live + readback is accurate
        dbg_server_service(c);    // receive step/play/capture commands
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
      static uint32_t ls = 0xFFFFFFFF;
      uint32_t st = (c->mem_r16(0x801054B0) << 16) | (c->mem_r32(0x80104C28) & 0xFFFF);
      if (st != ls) {
        fprintf(stderr, "[seqdbg] f%u open=%d playmask=0x%04X tickmode=%d seqfn=0x%08X stage=0x%08X\n",
                f, c->mem_r16s(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF,
                c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(TASKBASE + 0xc));
        ls = st;
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
    if (cfg_dbg("cam"))
      fprintf(stderr, "[cam] f%u (%d,%d,%d)\n", f, c->mem_r16s(0x1f8000d2u),
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
      static uint64_t last_sig = 0;
      if (sig != last_sig) {
        last_sig = sig;
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
      static uint32_t s_rd[14];
      for (int i = 0; i < 14; i++) {
        uint32_t s = 0x800be3d8u + (uint32_t)i * 0xB0u;
        uint32_t flag = c->mem_r32(s + 0x98), rd = c->mem_r32(s);
        if ((flag & 1) && rd != s_rd[i]) {
          fprintf(stderr, "[bgmtick] f%u slot%d active rdptr=%08X base=%08X (%+d)\n",
                  f, i, rd, c->mem_r32(s + 4), (int)(rd - c->mem_r32(s + 4)));
          s_rd[i] = rd;
        }
        if (!(flag & 1)) s_rd[i] = 0;
      }
    }
    static uint32_t s_last_entry = 0; static uint32_t s_last_sm = 0xFFFFFFFF;
    uint32_t t0e = c->mem_r32(TASKBASE + 0xc), s48 = c->mem_r16(TASKBASE + 0x48);
    // GAME runs a 4-level nested state machine (task +0x48/4a/4c/4e). Track all of it so a
    // stuck leaf is visible, not just the outer s48.
    uint32_t sm = (c->mem_r16(TASKBASE+0x48)<<24)|(c->mem_r16(TASKBASE+0x4a)<<16)|
                  (c->mem_r16(TASKBASE+0x4c)<<8)|c->mem_r16(TASKBASE+0x4e)
                  ^ (c->mem_r16(TASKBASE+0x50)<<12)^(c->mem_r16(TASKBASE+0x52)<<4);
    if (t0e != s_last_entry || sm != s_last_sm) {
      const char* stg = t0e == 0x8010649Cu ? "START" : t0e == 0x801062E4u ? "DEMO" :
                        t0e == 0x8010637Cu ? "GAME" : "?";
      fprintf(stderr, "[native_boot]   frame %u: stage=%s(0x%08X) sm[48=%u 4a=%u 4c=%u 4e=%u 50=%u 52=%u]"
              " @0x80109450=%08X\n",
              f, stg, t0e, c->mem_r16(TASKBASE+0x48), c->mem_r16(TASKBASE+0x4a),
              c->mem_r16(TASKBASE+0x4c), c->mem_r16(TASKBASE+0x4e), c->mem_r16(TASKBASE+0x50),
              c->mem_r16(TASKBASE+0x52), c->mem_r32(0x80109450));
      s_last_entry = t0e; s_last_sm = sm;
    }
    // One-shot: when GAME has settled, dump the CD-streaming contract (FUN_8001cfc8, task
    // slot 2). task2 obj @0x801fe0e0; +0x54=start LBA, +0x58=end LBA (= globals
    // DAT_801fe134/138). DAT_801fe146=channel/type. _DAT_1f8001f8=dest, _DAT_1f8001f4=words.
    if (cfg_dbg("stream") && t0e == 0x8010637Cu && f == 75) {
      fprintf(stderr, "[streamdbg] task2 obj @0x801fe0e0 state=%u entry=0x%08X\n",
              c->mem_r16(0x801fe0e0), c->mem_r32(0x801fe0ec));
      fprintf(stderr, "[streamdbg] startLBA(+54/801fe134)=%u endLBA(+58/801fe138)=%u "
              "chan(801fe146)=%u be0e4=0x%02X\n",
              c->mem_r32(0x801fe134), c->mem_r32(0x801fe138), c->mem_r8(0x801fe146), c->mem_r8(0x800be0e4));
      fprintf(stderr, "[streamdbg] dest(_DAT_1f8001f8)=0x%08X words(_DAT_1f8001f4)=%u "
              "f0=%u f1f800224=0x%08X\n",
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
      fprintf(stderr, "[schedf] f%u t0[st=%u e=%08X s48=%u s4a=%u s4c=%u s5c=%u] t1[st=%u] t2[st=%u]\n",
              f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48),
              c->mem_r16(TASKBASE + 0x4a), c->mem_r16(TASKBASE + 0x4c), c->mem_r16(TASKBASE + 0x5c),
              c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0));
    else if (f < 10 || (f % 30) == 0)
      fprintf(stderr, "[native_boot]   frame %u: t0[st=%u e=0x%08X s48=%u] t1[st=%u] t2[st=%u] "
                      "f135=%u\n", f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc),
              c->mem_r16(TASKBASE + 0x48), c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0),
              c->mem_r8(0x1f800135));
    dbg_server_service(c);  // service one queued live-debug-server command (non-blocking)
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
// crt0; crt0's call to FUN_80050b08 lands in ov_game_main.
void native_boot_run(Core* c) {
  { void cfg_dump(void); cfg_dump(); }   // log active PSXPORT_* config once (see docs/config.md)
  // PSX-fallback gate (diagnostic): boot + frame-loop stay native; everything the loop calls runs as PSX
  // recomp (sync CD). PSXPORT_GATE nonzero turns it on; the REPL `gate on|off` toggles it live.
  { const char* g = cfg_str("PSXPORT_GATE");
    if (g && *g) c->game->psx_fallback = (atoi(g) != 0);
    fprintf(stderr, "[native_boot] psx_fallback=%d (%s)\n", c->game->psx_fallback,
            c->game->psx_fallback ? "native boot+frameloop, PSX everything else (sync)" : "full native"); }
  // RENDER-path compare switch: PSXPORT_RENDER_PSX renders the field via the PSX recomp path (native state).
  { extern int g_render_psx; const char* r = cfg_str("PSXPORT_RENDER_PSX");
    if (r && *r) g_render_psx = (atoi(r) != 0);
    if (g_render_psx) fprintf(stderr, "[native_boot] g_render_psx=1 (field render via PSX recomp path)\n"); }
  // DUAL-VIEW: render the SAME game state TWICE per frame — engine-native (left) + PSX-recomp (right) — and
  // composite side by side. Set at launch (PSXPORT_DUALVIEW=1) so the GPU allocates two geometry batches.
  { extern int g_dualview; const char* r = cfg_str("PSXPORT_DUALVIEW");
    if (r && *r) g_dualview = (atoi(r) != 0);
    if (g_dualview) fprintf(stderr, "[native_boot] g_dualview=1 (side-by-side native | PSX render)\n"); }
  { extern int g_perobj_psx; const char* r = cfg_str("PSXPORT_PEROBJ_PSX");   // BISECT: per-object render -> PSX
    if (r && *r) g_perobj_psx = (atoi(r) != 0); }
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo (LOGO.STR) -> opening movie (OP.STR) ->
  // title/menu. The game's own STR streaming (strNext) TIMES OUT under our runtime (we don't feed
  // CD-streamed FMV sectors to its StrPlayer — see "time out in strNext()" in the DEMO stage), so the
  // movies are played here with our self-contained native FMV player (native_fmv.c).
  // SPLIT OF OWNERSHIP: only LOGO.STR (the Whoopee logo, which plays BEFORE the front-end overlay is
  // even loaded) is played at boot. OP.STR (the opening movie) is OWNED BY THE FRONT-END — the DEMO
  // menu machine's states 4..7 ARE the OP.STR sequence (engine_demo.cpp demo_menu_machine), which now
  // plays it via native_fmv_play. Playing OP here too made it play TWICE (boot + front-end) — the
  // "FMV repeats" bug. Boot plays LOGO; the front-end plays OP -> SCEA->LOGO->OP->title, no repeat.
  int native_fmv_play(Core*, const char*);
  // Skip the intro FMVs when there's no viewer: PSXPORT_NO_FMV, OR any headless run (a headless probe
  // has nobody watching — playing/decoding the intro movies just burns wall-clock; a field probe went
  // from ~77s to ~1.4s). The in-game/cutscene FMVs that still play are also auto-uncapped in headless
  // (native_fmv.c) so they fast-forward. Set PSXPORT_NO_FMV=0 explicitly to force them on if ever needed.
  int skip_fmv = cfg_on("PSXPORT_NO_FMV") || cfg_on("PSXPORT_VK_HEADLESS");
  const char* nf_ov = cfg_str("PSXPORT_NO_FMV");
  if (nf_ov && atoi(nf_ov) == 0 && *nf_ov) skip_fmv = 0;     // explicit PSXPORT_NO_FMV=0 forces FMVs on
  if (!skip_fmv) {
    fprintf(stderr, "[native_boot] playing boot FMV (Whoopee logo); OP.STR is the front-end's\n");
    native_fmv_play(c, "MOVIE/LOGO.STR");
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
