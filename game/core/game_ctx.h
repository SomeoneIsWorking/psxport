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
#include "core.h"   // Core + the 9 embedded subsystem types

static inline Engine&    eng(Core* c)        { return c->engine; }
static inline ScreenFade& fade(Core* c)      { return c->screenFade; }
static inline Rng&       rngOf(Core* c)       { return c->rng; }
static inline Trig&      trigOf(Core* c)      { return c->trig; }
static inline Math&      mathOf(Core* c)      { return c->math; }
static inline Mtx&       mtxOf(Core* c)       { return c->mtx; }
static inline Inventory& inv(Core* c)         { return c->inventory; }
static inline SaveMenu&  saveMenuOf(Core* c)  { return c->saveMenu; }
static inline Render*    rend(Core* c)        { return c->mRender; }
