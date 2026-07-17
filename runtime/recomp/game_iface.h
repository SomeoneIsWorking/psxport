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
// GameHooks — the callback vtable the framework calls to reach game behaviour. Defined fully in a
// later staging step (P1.4); forward-declared here so Core can hold a `const GameHooks*` now.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
struct GameHooks;

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
