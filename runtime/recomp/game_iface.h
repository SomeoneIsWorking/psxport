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

  // Recompiled MAIN .text range, masked to a physical address (addr & 0x1FFFFFFF). overlay_router
  // uses it to decide "is this address in the resident MAIN module or in an overlay slot". These are
  // GAME data — they come from the consumer's own recompiler run — so they belong here rather than
  // as a #include of the consumer's generated/overlay_table.h, which made the framework impossible to
  // compile standalone (see docs/porting-a-new-psx-game.md).
  uint32_t recMainLo, recMainHi;

  // Per-game name of the environment variable / .env key that points at this game's disc image
  // (e.g. "PSXPORT_TOMBA2_DISC", "PSXPORT_SPIDERMAN_DISC"). The resolver in disc.c used to hardcode
  // the FIRST consumer's key, so a second port set its own variable, the framework never read it,
  // and the CD model ran with NO DISC MOUNTED while every log line still looked ordinary. Leave it
  // null to rely on the generic PSXPORT_DISC / drop-in *.chd paths, which are always tried too.
  const char* discEnvVar;

  // Boot intro movies, in play order, NULL-terminated. The framework's native .STR player
  // (native_fmv.cpp) resolves each path on the disc itself and needs nothing game-specific to
  // decode one — but native_boot_run used to play a HARDCODED path, the first consumer's file. A
  // second port then failed to open it on every boot, and the only way to silence that was an
  // env-var workaround that disabled FMV wholesale.
  //
  // Leave every slot null when a game's boot plays no movie natively. That is a real state, not a
  // gap: a port whose boot runs on the recompiled substrate has its movies played by the GUEST, and
  // the framework must not invent an intro it was never asked for.
  const char* bootFmv[4];

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

  // STOCK Sony libcd CdGetSector(dest, words) — the sector-transfer routine. Appended at the END of
  // this group on purpose: GameConfig is initialised POSITIONALLY by every consumer, so inserting a
  // field mid-struct silently shifts every value after it.
  //
  // Distinct from cdReadPrim because the CONTRACT differs: this one carries no LBA. Stock libcd
  // positions the head with CdlSetloc and reads from wherever it was left, so the handler consumes
  // Cd::setloc_lba rather than an argument. Zero for a game that uses an engine loader instead.
  uint32_t cdGetSector;

  // Guest global holding stock libcd's READY callback pointer (what CdReadyCallback() writes). The
  // PC drives the read through it: a stock-libcd read is a per-sector callback loop, so invoking
  // this once per sector completes the transfer with no interrupt in the picture at all. Zero for a
  // game that does not use stock libcd. See cd_override.cpp cd_command's ReadN case.
  uint32_t cdReadyCbPtr;

  // Guest buffer holding stock libcd's LAST REQUESTED POSITION (what CdLastPos() returns), and the
  // last Setmode byte immediately after it: [+0..3] = the Setloc parameter, [+4] = mode.
  //
  // This is bookkeeping the REPLACED routine used to do. Overriding a library entry point means
  // inheriting the state it maintained: stock libcd records the Setloc parameter inside its
  // command-send routine, and its read path later seeds the expected-sector counter from it
  // (CdPosToInt(CdLastPos())). Skip the record and the counter is seeded from stale bytes, the
  // drive-position check fails, and every read is rejected — with nothing pointing at the override
  // as the cause. Zero for a game that does not use stock libcd.
  uint32_t cdLastPosBuf;

  // STOCK Sony libcd CdRead(sectors, buf, mode) and CdReadSync(mode, result). Overriding at THIS
  // level makes the PC perform the whole read: the guest's per-sector callback loop, drive-position
  // check, vblank timeout and retry path never run. Zero for a game that does not use stock libcd.
  uint32_t cdReadStock;
  uint32_t cdReadSync;

  // STOCK Sony libcd CdSearchFile(CdlFILE*, name) — resolved natively from the disc image's ISO9660
  // tree, so the guest's filesystem walk issues no drive activity at all.
  uint32_t cdSearchFile;

  // Guest slot holding stock libcd's DMA-COMPLETION callback (libcd's own callback table, indexed by
  // channel). A streaming reader marks each ring slot "DMA in flight" when it starts a transfer and
  // relies on this callback to promote it to "ready". With the transfer served synchronously and no
  // completion announced, every slot stalls half-done and the reader waits forever on a ring that is
  // full. Zero for a game that does not stream through stock libcd.
  uint32_t cdDmaDoneCbPtr;

  // --- pad driver (pad_input.cpp) ---
  uint32_t padSlot0Buf, padSlot1Buf, padDriverFn;
  uint32_t padSlotPtrTable;   // (added P1.x) SIO driver per-slot buf-ptr table base (+slot*padSlotPtrStride)
  // Byte distance between consecutive slots' buffer pointers. A driver that keeps a flat pointer
  // array uses 4 (Tomba!2); one that stores the pointer INSIDE a per-port context record uses that
  // record's size (Spyro: libpad's 240-byte per-port context). 0 is read as 4 so a config that
  // predates this field keeps its old meaning rather than silently reading slot 1 from slot 0.
  uint32_t padSlotPtrStride;

  // --- platform HLE: the PSX hardware-sync primitives (sync_overrides.cpp) ---
  // These are the SCEI library entry points whose real bodies busy-spin on a hardware IRQ our no-IRQ
  // runtime never raises (libmdec/libcd/libgpu/libetc sync + the kernel task-switch funnel).
  // PlatformHle::initBuiltins() installs a native handler at each.
  //
  // They were hardcoded in sync_overrides.cpp until 2026-07-28 — the SAME defect the seed set had,
  // and wrong in the same way: the addresses are facts about ONE executable. For a different game
  // they miss every primitive it actually uses AND install handlers over unrelated functions that
  // happen to sit at those addresses, which is a wrong abort waiting to fire rather than a silent
  // no-op. (Found standing up a second consumer; Spider-Man has real code at Tomba!2's VSync
  // address.) Same remedy as recMainLo/recMainHi: the value travels with the game.
  //
  // ZERO MEANS "this game has no such primitive, or it has not been RE'd yet" — initBuiltins skips
  // it. A game that leaves one zero and needs it will hang in the real spin loop, which is the
  // honest signal that its RE is outstanding.
  struct PlatformHleCfg {
    // The address windows register_() will accept. Everything outside them is refused, which is what
    // keeps GAME/engine logic out of this table (game logic is owned top-down through the override
    // registry instead). Two windows; an unused one is left {0,0}. If NO window is configured,
    // register_() refuses everything and says so — a game must state its own memory map.
    uint32_t windowLo[2], windowHi[2];

    // Resident-code range for the guest-backtrace heuristic (which words on the guest stack look
    // like return addresses). Compared against `addr & 0x1FFFFFFF`, so these are PHYSICAL. When left
    // zero the scan falls back to [recMainLo, recMainHi); set it explicitly when the game has
    // overlays resident above the main text.
    uint32_t codeScanLo, codeScanHi;

    uint32_t decDctInSync, decDctOutSync;      // libmdec DecDCTinSync / DecDCToutSync
    uint32_t cdReadSync, cdDataSync;           // libcd CdReadSync / CdDataSync
    uint32_t cdInitHandshake;                  // libcd low-level CdInit controller-ready handshake
    uint32_t gpuTimeoutArm, gpuTimeoutCheck;   // libgpu GPU-DMA-completion timeout (arm / check)
    uint32_t gpuTimeoutDeadlineVar;            // guest global the arm writes its far-future deadline to
    uint32_t gpuTimeoutFlagVar;                // guest global the arm clears
    uint32_t changeThread;                     // kernel cooperative task-switch / yield funnel

    // libetc VSync, as a TRAP: "nothing may reach VSync, in any mode, because the native frame loop
    // owns all timing". That is a per-game POLICY, not a framework universal — it holds only for a
    // port whose native loop actually drives the frame. A game still running the guest's own loop on
    // the substrate must instead reimplement VSync faithfully and register it itself via
    // PlatformHle::register_(). Leave this ZERO in that case; setting both is a contradiction, and
    // initBuiltins would clobber the game's handler with the trap.
    uint32_t vsyncTrap;
  } hle;

  // --- rendering policy (gpu_vk.cpp render_geom) ---
  // Does the guest's UPLOADED VRAM stay visible under the submitted primitives?
  //
  // The renderer clears the colour target to black before drawing, on the principle that "the PC
  // renderer shows ONLY what a native producer submitted". That is right for a port whose native
  // renderer owns the frame — anything left over from the guest's own drawing would be stale.
  //
  // It is WRONG for a port still running the guest's drawing code, because on real hardware an upload
  // into the display area IS visible. A game whose logo screens, FMV stills or menus are uploads with
  // no primitives renders them BLACK: the upload lands in VRAM and the clear discards it before
  // anything is drawn on top. That is exactly what Spyro's SCE/Universal screens do.
  //
  // ZERO KEEPS THE EXISTING BEHAVIOUR (clear to black), so a consumer that does not set it is
  // unaffected — and this field is APPENDED at the end of the struct because GameConfig is
  // initialised positionally by some consumers. Set to 1 while the guest still owns drawing.
  uint32_t preserveVramBackdrop;

  // --- memory card (memcard.cpp) ----------------------------------------------------------------
  // The consuming game's memory-card env key and default backing-file path. Same lesson as
  // discEnvVar: the resolver used to hardcode the FIRST consumer's key (PSXPORT_TOMBA2_CARD) and
  // filename (scratch/saves/tomba2.mcr), so a second consumer's card silently landed in the
  // reference game's file, or nowhere. NULL keeps the old behaviour for a consumer that has not set
  // them. Appended at the end because GameConfig is initialised positionally.
  const char* cardEnvVar;
  const char* cardDefaultPath;
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
  // Dev-warp AREA INDEX, for the RmlUi warp selector and the REPL `warp` range guard. Game-side because
  // the framework must not know how many areas a game has, what they are called, or which guest address
  // says "we are in the field". devAreaName returns "" when the area has no sourced name — the caller
  // renders "Area N" rather than inventing one.
  int         (*devAreaCount)(Core* c);
  const char* (*devAreaName)(Core* c, int area);
  bool        (*devWarpAllowed)(Core* c);     // is a warp legal right now (game stage test)

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

  // --- diagnostics harness: GAME-defined selftests ---
  // selftest_run() handles the framework's own PSXPORT_SELFTEST names and delegates anything else
  // here, so a game can add oracle/unit tests without the agnostic framework naming them. Return 2
  // for an unrecognised name (same "unknown selftest" code selftest_run uses).
  int (*selftestGame)(const char* which, const char* exePath);
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
