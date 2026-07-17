// game_ctx.h — the game-context accessors.
//
// TRANSPARENT SEAM (prep step, 2026-07-17): these inline accessors take a Core* and return a
// reference (or pointer, for the renderer) to one of the 9 game-side subsystems that currently
// live embedded on `Core` (see runtime/recomp/core.h). Right now each body just returns the
// same Core member, so routing every game-code access through them is BEHAVIOR-NEUTRAL — a clean
// build is the correctness proof.
//
// NEXT STEP: these 9 subsystems move OFF Core into a game-side aggregate reached via
// `c->gameCtx` (a TombaCtx*). When that happens, ONLY the bodies below change
// (e.g. `return ((TombaCtx*)c->gameCtx)->engine;`) — the ~1000 call sites stay untouched.
//
// Names are chosen to NOT collide with the member names themselves (rng/trig/math/mtx would
// double as function names and `math` clashes with the type/namespace): eng/fade/rngOf/trigOf/
// mathOf/mtxOf/inv/saveMenuOf/rend.
#pragma once
#include "core.h"   // Core (framework side — no longer names any game subsystem type)
// The 9 game-side subsystem types now live in the game-owned TombaCtx aggregate below (moved OFF
// Core), so THIS game header pulls their definitions directly (core.h no longer does).
#include "core/engine.h"
#include "render/screen_fade.h"
#include "math/rng.h"
#include "math/trig.h"
#include "math/gte_math.h"
#include "math/mtx.h"
#include "items/inventory.h"
#include "ui/save_menu.h"
#include "render.h"
#include "audio/native_music.h"   // NativeMusic — in-game real-time SEP/VAB music player (game-owned)
#include "audio/music_list.h"     // MusicList — Sound Test catalogue + area BGM driver (game-owned)

// TombaCtx — the game's opaque per-Core subsystem aggregate. Allocated/wired by tomba_ctx_create
// (game_ctx.cpp) and reached from the framework only as Core::gameCtx (void*). Holds the 9 subsystems
// that used to be embedded on Core.
struct TombaCtx {
  Engine     engine;
  ScreenFade screenFade;
  Rng        rng;
  Trig       trig;
  Math       math;
  Mtx        mtx;
  Inventory  inventory;
  SaveMenu   saveMenu;
  NativeMusic native_music;   // real-time SEP/VAB synth mixed into the SPU sink via the audioMixFrame hook
  MusicList   music_list;     // Sound Test catalogue + in-game area BGM driver (uses native_music)
  Render*    mRender = nullptr;
};

static inline TombaCtx* gctx(Core* c) { return (TombaCtx*)c->gameCtx; }

static inline Engine&    eng(Core* c)        { return gctx(c)->engine; }
static inline ScreenFade& fade(Core* c)      { return gctx(c)->screenFade; }
static inline Rng&       rngOf(Core* c)       { return gctx(c)->rng; }
static inline Trig&      trigOf(Core* c)      { return gctx(c)->trig; }
static inline Math&      mathOf(Core* c)      { return gctx(c)->math; }
static inline Mtx&       mtxOf(Core* c)       { return gctx(c)->mtx; }
static inline Inventory& inv(Core* c)         { return gctx(c)->inventory; }
static inline SaveMenu&  saveMenuOf(Core* c)  { return gctx(c)->saveMenu; }
static inline Render*    rend(Core* c)        { return gctx(c)->mRender; }
