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
  void (*frameUpdate)(Core* c);               // per-frame guest body (was c->engine.frameUpdate())
  void (*drawOTag)(Core* c, uint32_t otHead); // per-frame draw kick (was c->engine.drawOTag(otHead))
  void (*musicCoordTick)(Core* c);            // per-frame music coord (was c->engine.musicCoord.tick())
  bool (*cdDialogToneActive)(Core* c);        // dialog-tone gate (was c->engine.musicCoord.dialogToneActive())
  void (*cdMusicFadeIn)(Core* c);             // ingame-music fade-in (was c->engine.musicCoord.musicFadeIn())
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
