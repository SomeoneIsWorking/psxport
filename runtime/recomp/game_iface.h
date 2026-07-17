// game_iface.h — THE framework↔game seam (the ONLY interface between the PSX-generic framework in
// runtime/recomp/ and a game-specific reimplementation).
//
// The framework NEVER #includes anything from game/. Instead a game provides, at init:
//   * a GameConfig — the game's guest ADDRESSES/tables (MAIN.EXE-specific literals the framework's
//     generic loops iterate: crt0/boot layout, the per-frame OT/packet-pool dance, the scheduler task
//     layout, the overlay slot bases, CD chokepoints, pad buffers);
//   * a GameHooks — a function-pointer vtable the framework calls to reach game behaviour (frame
//     update, OT draw, boot init, stage entry, music, HUD readout, render-state, diagnostics);
//   * an opaque game context (void* Core::gameCtx) holding the game's per-Core subsystem aggregate.
//
// A Core reaches the game ONLY through `c->cfg->…`, `c->hooks->…(c)`, and `c->gameCtx`. This header
// carries no game types — only forward declarations — so the framework compiles standalone.
#pragma once
#include <stdint.h>

#ifdef __cplusplus

class Core;   // runtime/recomp/core.h
class Game;   // runtime/recomp/game.h  (the framework machine owner; stays framework-side)

// FadeState — the framework-side POD mirror of the game's ScreenFade::State {Mode mode; uint8_t r,g,b}.
// The present path (gpu_vk.cpp) reads the game's per-frame fade through renderFadeState() into one of
// these, so the framework never names the game's ScreenFade type. `mode` widened to int (the ScreenFade
// Mode enum is uint8_t-backed; all present-path consumers already read it as int).
struct FadeState { int mode; unsigned char r, g, b; };

// SchedBody — which game stage body the framework scheduler (PcScheduler, now framework) is asking the
// game to run. PcScheduler owns the framework-side task/coro/yield machinery; the actual stage bodies are
// game code (Engine::*), reached through the single schedStageBody hook so the framework names no Engine
// method. Values are the game's dispatch cases (see game/core/game_hooks.cpp tomba_schedStageBody).
enum SchedBody {
  SCHED_DEMO_STAGEMAIN = 0,   // eng(c).demo.stageMain()          (fresh DEMO prologue)
  SCHED_DEMO_FRAME,           // eng(c).demo.frame()
  SCHED_GAME_PROLOGUE,        // eng(c).stagePrologue()           (fresh GAME prologue)
  SCHED_GAME_FRAME,           // eng(c).frame()                   → returns handled (0/1)
  SCHED_SOP_AREALOAD,         // eng(c).sop.areaLoad()
  SCHED_CORO_TEXGROUP,        // eng(c).asset.loadTexgroup()
  SCHED_CORO_PRELOAD1,        // eng(c).asset.preloadStage1AsTask()
  SCHED_CORO_AREADATA,        // eng(c).asset.areaDataLoadAsTask()
  SCHED_CORO_AREALOAD_FAITHFUL, // eng(c).sop.areaLoadFaithful()
  SCHED_FIBER_STARTBIN,       // eng(c).startBinStageFaithful()
  SCHED_FIBER_DEMO_BODY,      // eng(c).demo.stageBodyFaithful()
  SCHED_FIBER_STAGE_BODY,     // eng(c).stageBodyFaithful()
  SCHED_STAGE0_ADVANCE_SKIP,  // eng(c).stage0AdvanceSkip(arg)   (arg = stage0_step)
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// GameConfig — the game-specific guest ADDRESSES/tables. A game fills one static instance; the
// framework substrate reads `c->cfg->field` in place of the hardcoded MAIN.EXE literals it used to bake
// in. Grouped by the framework consumer. (Values live in a game-provided instance, NOT here.)
// ─────────────────────────────────────────────────────────────────────────────────────────────────
struct GameConfig {
  // --- crt0 / boot (native_boot.cpp crt0_setup, game_init) ---
  uint32_t bssZeroLo, bssZeroHi;          // .bss clear range
  uint32_t stackTopBase, stackTopBase2;   // guest stack top globals
  uint32_t heapBase;                      // heap start
  uint32_t heapSizePtr, heapBasePtr;      // heap globals written by crt0
  uint32_t gp;                            // global pointer
  uint32_t libcInit;                      // libc init entry
  uint32_t gameMain, crt0;                // game-main / crt0 entries

  // --- per-frame OT / packet-pool dance (native_boot.cpp native_step_frame) ---
  uint32_t otRegionBase, otRegionStride;      // per-parity OT region
  uint32_t packetPoolBase, packetPoolStride;  // per-parity packet pool
  uint32_t otBasePtr;                         // OT-base pointer global
  uint32_t dwellCounter;
  uint32_t poolPtrCur, poolPtrLast;
  uint32_t clearOtagR, putDrawEnv, drawSync;
  uint32_t irqEventClasses[3];
  uint32_t dualviewRenderOrch, dualviewSubmit;

  // --- scheduler task layout (scheduler.cpp, native_boot probes) ---
  uint32_t taskTableBase, taskSlotStride, taskCount;
  uint32_t curTaskPtr;
  uint32_t stageStart, stageDemo, stageGame;  // fresh-entry stage PCs

  // --- overlay router slots (overlay_router.cpp slot_index) ---
  struct OverlaySlot { uint32_t base; const char* name; };
  OverlaySlot overlaySlots[3];

  // --- CD chokepoints (cd_override.cpp) ---
  uint32_t cdInit, cdCommand, cdSync, cdReadPrim, cdFileLoad, cdAsyncRead,
           voicePlay, voiceStop, lastSectorTracker;
  uint32_t cdInlineLoad;      // (added P1.x) FUN_8001DC40 inline (non-spawning) sync loader
  uint32_t cdCmdStream;       // (added P1.x) FUN_8001CE90 streaming CD-cmd wrapper (GetlocL)
  uint32_t cdCallbackTable[4];// the 4 guest-RAM slots hleInit writes the CD-event callbacks into
  uint32_t cdCallbackFn[4];   // (added P1.x) the 4 callback fn-ptr VALUES written into those slots

  // --- pad driver (pad_input.cpp) ---
  uint32_t padSlot0Buf, padSlot1Buf, padDriverFn;
  uint32_t padSlotPtrTable;   // (added P1.x) SIO driver per-slot buf-ptr table base (+slot*4)
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// GameHooks — the callback vtable the framework calls to reach game behaviour. Each member is a
// function pointer taking `Core* c` (the game reaches its own subsystems via `c->engine.*` inside the
// impl); the framework substrate calls `c->hooks->fn(c)` in place of the direct `c->engine.X()` calls
// it used to bake in. More hooks (bootInit, schedFreshEntry, diagnostics) land in later staging steps.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
struct GameHooks {
  // --- game context lifecycle: the framework allocates/frees the game's opaque per-Core subsystem
  // aggregate (Core::gameCtx) through these. ctxCreate runs at the end of Core's ctor; ctxDestroy in
  // its dtor. The framework never names the aggregate type — it holds only the void*. ---
  void* (*ctxCreate)(Core* c);                // allocate + wire the game's per-Core subsystem aggregate
  void  (*ctxDestroy)(void* ctx);             // free it

  void (*frameUpdate)(Core* c);               // per-frame guest body (was c->engine.frameUpdate())
  void (*drawOTag)(Core* c, uint32_t otHead); // per-frame draw kick (was c->engine.drawOTag(otHead))
  void (*musicCoordTick)(Core* c);            // per-frame music coord (was c->engine.musicCoord.tick())
  bool (*cdDialogToneActive)(Core* c);        // dialog-tone gate (was c->engine.musicCoord.dialogToneActive())
  void (*cdMusicFadeIn)(Core* c);             // ingame-music fade-in (was c->engine.musicCoord.musicFadeIn())

  // --- game audio: the framework SPU host-sink + mod-UI HUD reach the game's native music engine
  // (NativeMusic/MusicList, now game-side on TombaCtx) through these, so the framework names no game
  // audio type. ---
  void (*audioMixFrame)(Core* c, int16_t* buf, int frames);  // mix the game's in-game music on top of the
                                              // drained SPU PCM each audio frame (was spu_audio.cpp's direct
                                              // active()/render() + saturating add on the game player).
                                              // `buf` = frames*2 interleaved int16 stereo; no-op when silent.
  const char* (*audioNowPlayingName)(Core* c);// currently-playing Sound-Test track name, or nullptr when
                                              // stopped (was rmlui_overlay's direct catalogue nowPlaying()/name()).
  void (*audioSoundTestPlay)(Core* c, int track); // Sound-Test action: play catalogued track (>=0) or stop
                                              // (<0) (was rmlui_overlay's direct catalogue play()/stop()).
  void (*bootInit)(Core* c);                  // the game's boot-init prologue (was the whole init-prefix body of
                                              // native_boot.cpp game_init: the guest boot-prologue transcription —
                                              // rc-dispatched guest leaves interleaved with the c->engine.* init
                                              // calls initFrameState/initDisplay/initCamera/font.init/initSubsystems/
                                              // task0Bootstrap. Moved WHOLE because the engine calls are interleaved
                                              // with the rc leaves and task0Bootstrap depends on the scheduler-table
                                              // init between them — order is load-bearing, so it cannot be split).
  bool (*schedFreshEntry)(Core* c, int slot, uint32_t base, uint32_t entryPc); // fresh task-entry native stage body:
                                              // dispatches the GAME stagePrologue (was c->engine.stageMain(), which
                                              // sets coro_redirect_pc) or STAGE-0 startBinStage (was
                                              // c->engine.startBinStage()) by entryPc. Returns true when it ran the
                                              // TERMINAL startBinStage body (caller finalizes + early-returns the
                                              // tick); false to continue to rec_coro_run (stageMain leaves the redirect
                                              // start in c->coro_redirect_pc; a non-stage fresh entry leaves it 0).
  bool (*hasNativeHandlerForEntry)(Core* c, uint32_t entryPc); // does this task entry PC have a native stage handler
                                              // (was c->game->pcSched.hasNativeHandlerForEntry(entryPc)).
  void (*registerOverrides)(Game* g);         // install ALL game override clusters into the process-global
                                              // registry (was boot.cpp register_engine_overrides(game)).
                                              // Takes Game* (not Core*): the clusters register per-Game.
                                              // MUST run before crt0_setup/game_init on every harness Game.

  // --- present-path + diagnostics + boot-frame reset (last framework→game member refs) ---
  void (*renderFadeState)(Core* c, FadeState* out);       // present fade read (was core->screenFade.get())
  const char* (*replBehaviorName)(Core* c, unsigned int handle); // REPL `ents` (was c->engine.behaviors.nativeName)
  void (*replCamTeleport)(Core* c, int x, int y, int z);  // REPL `tp` (was c->engine.camTeleport)
  void (*replCamTeleportOff)(Core* c);                    // REPL `tp off` (was c->engine.camTeleportOff)
  void (*renderBbFrameReset)(Core* c);                    // per-frame bb reset (was c->mRender->bbFrameReset())

  // --- game-side REPL commands + dev-warp area load (last game-class refs pulled out of repl.cpp /
  // native_boot.cpp so the framework #includes no game header). ---
  bool (*replCommand)(Core* c, const char* cmd, const char* line); // REPL command the framework doesn't
                                              // itself handle — game classes / Tomba guest addrs (invtest,
                                              // bgm/bgmstop, seqsolo, musictest). Returns true iff handled.
  void (*devWarpAreaLoad)(Core* c);           // dev-warp full area load (was native_boot.cpp's
                                              // eng(c).sop.transitionAreaLoad()).

  // --- scheduler stage bodies (PcScheduler is framework; the stage bodies are game Engine methods) ---
  int      (*schedStageBody)(Core* c, int which, void* arg);  // run the SchedBody-selected game stage
                                              // body (arg used only by SCHED_STAGE0_ADVANCE_SKIP); returns the
                                              // body's int result (Engine::frame's `handled`; 0 for void bodies)
  uint32_t (*schedRng)(Core* c);              // rngOf(c).next() — FUN_8009A450, guest seed 0x80105EE8

  // --- fps60 tier-1 (Fps60 is framework render-infra: the interpolated-60fps lerp tier is a GENERIC
  // renderer feature and stays framework). TARGET ARCHITECTURE (USER 2026-07-17): the game SUBMITS its
  // objects/geometry to the framework once, and the framework lerp-renders them between logic frames —
  // NO callback into game render. The two hooks below are a TRANSITIONAL SEAM standing in for that submit
  // model: today Fps60's interp present RE-RUNS the game's world passes (Render::terrain/scene/backdrop/
  // objects) one frame behind under lerped inputs, so the framework still calls back into game render.
  // DEATH CONDITION: delete both hooks once the game submits drawables through the render queue and the
  // framework interpolates them directly (the fps60 submit-model render-frontier redesign). ---
  void (*fps60WorldPass)(Core* c, float t);   // TRANSITIONAL: interp world-pass re-render (gates + terrain/
                                              // scene-table/backdrop/objects Render::* calls) under lerped
                                              // inputs. Reads/writes the framework Fps60 bg-override on
                                              // c->game->fps60 (game writing a framework member).
  void (*fps60BbSwapPrev)(Core* c);           // TRANSITIONAL: rend(c)->bbSwapPrev() — billboard record rotate

  // --- diagnostics harness: camera-oracle selftest (game/camera/cutscene_camera_selftest.cpp) ---
  int (*selftestCameraOracle)(const char* exePath);  // was selftest.cpp's direct run_camera_oracle()
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Install — the game registers its config+hooks ONCE at startup, before any Game/Core is constructed
// (the standalone framework smoke registers a stub). Process-global; both SBS cores share it. Core's
// constructor snapshots the installed pointers into c->cfg / c->hooks. Returns nullptr until installed
// (harmless: nothing reads cfg/hooks until the corresponding literal/call-site conversions land).
// ─────────────────────────────────────────────────────────────────────────────────────────────────
void              psxport_install_game(const GameConfig* cfg, const GameHooks* hooks);
const GameConfig* psxport_game_config();
const GameHooks*  psxport_game_hooks();

#endif // __cplusplus
