// game_hooks.cpp — the Tomba!2 GameHooks instance (game_iface.h seam).
//
// Each hook is a thin function-pointer impl that reaches the game's per-Core Engine aggregate
// (`eng(c).*`). The framework substrate (runtime/recomp/*) calls these as `c->hooks->fn(c)` in
// place of the direct `eng(c).X()` calls it used to bake in, so the framework no longer names any
// game type. Installed alongside the GameConfig via tomba_install_game_config() (game_config.cpp),
// before any Game/Core is constructed, so Core's ctor snapshots a non-null c->hooks.
#include "game_iface.h"
#include "game_ctx.h"
#include "core.h"
#include "engine.h"
#include "game.h"        // c->game->cd / c->game->timing / c->game->pcSched reached by the boot + sched hooks
#include "guest_call.h"  // rc0/rc1/rc2 — rec_dispatch of the guest boot-prologue leaves (bootInit)
#include "render/screen_fade.h"  // ScreenFade — tomba_renderFadeState mirrors get() into a framework FadeState
#include "render.h"      // Render umbrella — tomba_renderBbFrameReset calls rend(c)->bbFrameReset()
#include <stdio.h>

// tomba_renderFadeState — mirror the game's per-frame ScreenFade into the framework FadeState POD, so the
// present path reads fade without naming ScreenFade. Same read the present path did directly (screenFade.get()).
static void tomba_renderFadeState(Core* c, FadeState* out) {
  ScreenFade::State s = fade(c).get();
  out->mode = (int)s.mode;
  out->r = s.r; out->g = s.g; out->b = s.b;
}
// REPL diagnostics — reach the game's engine subsystems (was direct eng(c).* calls in repl.cpp).
static const char* tomba_replBehaviorName(Core* c, unsigned int handle) { return eng(c).behaviors.nativeName(handle); }
static void        tomba_replCamTeleport(Core* c, int x, int y, int z)  { eng(c).camTeleport(x, y, z); }
static void        tomba_replCamTeleportOff(Core* c)                    { eng(c).camTeleportOff(); }
// Per-frame billboard/bb reset (was native_step_frame's direct rend(c)->bbFrameReset()).
static void        tomba_renderBbFrameReset(Core* c)                    { rend(c)->bbFrameReset(); }
// dev-warp full area load (was native_boot.cpp game_main's eng(c).sop.transitionAreaLoad()).
static void        tomba_devWarpAreaLoad(Core* c)                       { eng(c).sop.transitionAreaLoad(); }
// game-side REPL commands (invtest/bgm/bgmstop/seqsolo/musictest) — body in game/core/repl_commands.cpp.
extern bool tomba_repl_command(Core* c, const char* cmd, const char* line);

static void tomba_frameUpdate(Core* c)                { eng(c).frameUpdate(); }
static void tomba_drawOTag(Core* c, uint32_t otHead)  { eng(c).drawOTag(otHead); }
static void tomba_musicCoordTick(Core* c)             { eng(c).musicCoord.tick(); }
static bool tomba_cdDialogToneActive(Core* c)         { return eng(c).musicCoord.dialogToneActive(); }
static void tomba_cdMusicFadeIn(Core* c)              { eng(c).musicCoord.musicFadeIn(); }

// tomba_audioMixFrame — mix the game's native music engine on top of the SPU's drained PCM. Was the
// direct native_music mix in spu_audio.cpp::frameEx: render into a per-frame scratch, saturating-add.
// `frames` is the SPU sink's per-video-frame count, capped at SPU_FRAMES_PER_VIDEO_FRAME(735)+64.
static void tomba_audioMixFrame(Core* c, int16_t* buf, int frames) {
  NativeMusic& nm = gctx(c)->native_music;
  if (!nm.active()) return;
  const int kMaxFrames = 735 + 64;         // mirrors the SPU host-sink per-frame cap (spu_audio.cpp)
  if (frames > kMaxFrames) frames = kMaxFrames;
  int16_t mbuf[2 * (735 + 64)];            // one video frame of interleaved stereo scratch
  nm.render(mbuf, frames);
  for (int i = 0; i < frames * 2; i++) {
    int v = buf[i] + mbuf[i];
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    buf[i] = (int16_t)v;
  }
}
// Sound-Test / HUD music readout — reach the game's MusicList (was rmlui_overlay's direct game->music_list.*).
static const char* tomba_audioNowPlayingName(Core* c) {
  int np = gctx(c)->music_list.nowPlaying();
  return (np >= 0) ? gctx(c)->music_list.name(np) : nullptr;
}
static void tomba_audioSoundTestPlay(Core* c, int track) {
  if (track < 0) gctx(c)->music_list.stop();
  else           gctx(c)->music_list.play(track);
}

// tomba_bootInit — the game's boot-init prologue, moved VERBATIM out of native_boot.cpp game_init.
// This is the transcription of FUN_80050b08's init prefix (no scheduler loop): rc-dispatched guest
// leaves interleaved with the native eng(c).* init calls, in the exact guest order. It moves WHOLE
// (not just the engine calls) because the engine calls are interleaved with the rc leaves and the
// trailing task0Bootstrap depends on the scheduler-table init (0x80051e00 / 0x80051f14) + the
// 0x1f800138 cur-task write that sit between them — the order is load-bearing.
static void tomba_bootInit(Core* c) {
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
  eng(c).initFrameState();      // was rc0(c, 0x80050a0c)
  eng(c).initDisplay();         // was rc0(c, 0x800509b4) — GTE projection + display (sets H=DAT_801003f8)
  eng(c).initCamera();          // was rc0(c, 0x80050a80)
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
  eng(c).font.init();                       // was rc0(c, 0x80075130)
  rc1(c, 0x8009c620, 0);
  rc0(c, 0x8001cc00);
  eng(c).initSubsystems();               // was rc0(c, 0x800520e0) — own orchestration native
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
  eng(c).task0Bootstrap();   // PC-native: was rc0(c, 0x800499e8) — CD subtree owned top-down
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          c->mem_r32(0x80106228), c->mem_r32(0x8010649c), c->mem_r16(0x801fe000), c->mem_r32(0x801fe00c));
}

// tomba_schedFreshEntry — the fresh-task-entry native stage-body dispatch, moved out of scheduler.cpp's
// recomp_run_generic_dispatch_stanza. entryPc is the fresh resume_pc. Two native stages:
//   * GAME stagePrologue (cfg->stageGame == 0x8010637C): set the coro-redirect target and run stageMain,
//     which runs stagePrologue + rec_coro_redirect(0x801063F4) → leaves c->coro_redirect_pc set. Return
//     FALSE: the caller continues to rec_coro_run, taking the redirect start from c->coro_redirect_pc.
//   * STAGE-0 startBinStage (cfg->stageStart == 0x8010649C): run the terminal startBinStage body. Return
//     TRUE: the caller finalizes the stage-0 slot (stage0_step/task_ctx/base=2/in_stage=0) and early-returns
//     the tick WITHOUT running rec_coro_run.
// A non-stage fresh entry matches neither: returns false with c->coro_redirect_pc untouched (0), so the
// caller runs rec_coro_run at the plain resume_pc — exactly the original else-fall-through.
static bool tomba_schedFreshEntry(Core* c, int /*slot*/, uint32_t /*base*/, uint32_t entryPc) {
  if (entryPc == c->cfg->stageGame) {
    c->override_tgt = entryPc;                 // GAME stageMain: coro-redirect target
    eng(c).stageMain();                     // stagePrologue + rec_coro_redirect(0x801063F4)
    return false;                              // continue to rec_coro_run with the redirect start
  }
  if (entryPc == c->cfg->stageStart) {
    eng(c).startBinStage();                 // STAGE-0 fresh; terminal — skip rec_coro_run
    return true;
  }
  return false;                                // not a native stage: plain rec_coro_run at resume_pc
}

static bool tomba_hasNativeHandlerForEntry(Core* c, uint32_t entryPc) {
  return c->game->pcSched.hasNativeHandlerForEntry(entryPc);
}

// tomba_schedStageBody — run the SchedBody-selected game stage body. PcScheduler (framework) owns the
// task/coro/yield machinery and calls this for the actual Engine::* stage body, so the framework names no
// Engine method. Returns the body's int result (Engine::frame's `handled`; 0 for the void bodies).
static int tomba_schedStageBody(Core* c, int which, void* arg) {
  switch (which) {
    case SCHED_DEMO_STAGEMAIN:          eng(c).demo.stageMain();            return 0;
    case SCHED_DEMO_FRAME:              eng(c).demo.frame();                return 0;
    case SCHED_GAME_PROLOGUE:           eng(c).stagePrologue();             return 0;
    case SCHED_GAME_FRAME:              return eng(c).frame();
    case SCHED_SOP_AREALOAD:            eng(c).sop.areaLoad();              return 0;
    case SCHED_CORO_TEXGROUP:           eng(c).asset.loadTexgroup();        return 0;
    case SCHED_CORO_PRELOAD1:           eng(c).asset.preloadStage1AsTask(); return 0;
    case SCHED_CORO_AREADATA:           eng(c).asset.areaDataLoadAsTask();  return 0;
    case SCHED_CORO_AREALOAD_FAITHFUL:  eng(c).sop.areaLoadFaithful();      return 0;
    case SCHED_FIBER_STARTBIN:          eng(c).startBinStageFaithful();     return 0;
    case SCHED_FIBER_DEMO_BODY:         eng(c).demo.stageBodyFaithful();    return 0;
    case SCHED_FIBER_STAGE_BODY:        eng(c).stageBodyFaithful();         return 0;
    case SCHED_STAGE0_ADVANCE_SKIP:     return eng(c).stage0AdvanceSkip(*(uint8_t*)arg);
    default:                                                               return 0;
  }
}
static uint32_t tomba_schedRng(Core* c) { return rngOf(c).next(); }  // FUN_8009A450 (guest seed 0x80105EE8)

// tomba_fps60WorldPass / tomba_fps60BbSwapPrev — TRANSITIONAL fps60 seam (see game_iface.h). The interp
// present's world-pass re-render lives in the framework Fps60::tier1Render; these hooks carry the two
// reaches into game Render. Body in game/render/fps60_worldpass.cpp (needs Render + the Fps60 bg-override).
extern void tomba_fps60_world_pass(Core* c, float t);
extern void tomba_fps60_bb_swap_prev(Core* c);

// tomba_selftestCameraOracle — the camera-oracle selftest branch (game/camera/cutscene_camera_selftest.cpp),
// called by the framework selftest harness through the hook so selftest.cpp names no game function.
extern int run_camera_oracle(const char* exe_path);
static int tomba_selftestCameraOracle(const char* exePath) { return run_camera_oracle(exePath); }

// registerOverrides installs ALL the game's override clusters into the process-global registry.
// Body lives in register_overrides.cpp (moved out of framework boot.cpp); declared here so the hook
// table names it. Takes Game* (not Core*): the clusters register per-Game.
extern void register_engine_overrides(Game*);

// TombaCtx lifecycle — the game's per-Core subsystem aggregate alloc/free (game_ctx.cpp). The
// framework calls these through the ctxCreate/ctxDestroy hooks to fill/free Core::gameCtx.
extern void* tomba_ctx_create(Core*);
extern void  tomba_ctx_destroy(void*);

// extern-visible: game_config.cpp names it in the install call. A namespace-scope `const` object
// has INTERNAL linkage by default in C++, so `extern` is required to export the symbol. One
// process-global instance; both SBS cores snapshot the same pointer.
extern const GameHooks g_tomba_hooks = {
  /* ctxCreate          */ tomba_ctx_create,
  /* ctxDestroy         */ tomba_ctx_destroy,
  /* frameUpdate        */ tomba_frameUpdate,
  /* drawOTag           */ tomba_drawOTag,
  /* musicCoordTick     */ tomba_musicCoordTick,
  /* cdDialogToneActive */ tomba_cdDialogToneActive,
  /* cdMusicFadeIn      */ tomba_cdMusicFadeIn,
  /* audioMixFrame      */ tomba_audioMixFrame,
  /* audioNowPlayingName*/ tomba_audioNowPlayingName,
  /* audioSoundTestPlay */ tomba_audioSoundTestPlay,
  /* bootInit           */ tomba_bootInit,
  /* schedFreshEntry    */ tomba_schedFreshEntry,
  /* hasNativeHandlerForEntry */ tomba_hasNativeHandlerForEntry,
  /* registerOverrides  */ register_engine_overrides,
  /* renderFadeState    */ tomba_renderFadeState,
  /* replBehaviorName   */ tomba_replBehaviorName,
  /* replCamTeleport    */ tomba_replCamTeleport,
  /* replCamTeleportOff */ tomba_replCamTeleportOff,
  /* renderBbFrameReset */ tomba_renderBbFrameReset,
  /* replCommand        */ tomba_repl_command,
  /* devWarpAreaLoad    */ tomba_devWarpAreaLoad,
  /* schedStageBody     */ tomba_schedStageBody,
  /* schedRng           */ tomba_schedRng,
  /* fps60WorldPass     */ tomba_fps60_world_pass,
  /* fps60BbSwapPrev    */ tomba_fps60_bb_swap_prev,
  /* selftestCameraOracle */ tomba_selftestCameraOracle,
};
