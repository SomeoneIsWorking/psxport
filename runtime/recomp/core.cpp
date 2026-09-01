// Core::Core / Core::~Core — the per-instance R3000 machine's constructor and destructor.
//
// Zero the R3000 register bank + main-RAM + scratchpad, allocate the render subsystem umbrella
// (`class Render` — game/render/render.h), and wire every owned subsystem's back-pointer to `this`
// so its methods can reach this Core's guest memory. Callers access subsystems as:
//     c->screenFade.method(args)   // embedded-value subsystems
//     c->mRender->mNodeXform.method(args)   // pointer-to-umbrella subsystem
//
// Lived in mem.cpp historically (right next to the memory-access primitives) — moved out into its
// own file so Core lifetime concerns aren't tangled with the memory-window helpers.
#include "core.h"
#include "config_vars.h"
#include "game_runtime.h"
#include <cstring>

Core::Core() {
  memset((R3000 *)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
  // The DEFAULT engine for this Core, from PSXPORT_ENGINE. AFTER the memsets: `engine` is a Core
  // member, and a derived member can legally be laid out inside the base's tail padding, so a
  // sizeof(R3000) wipe could reach it. A Core that needs a specific engine (the SBS oracle pane, the
  // selftest oracle cores) assigns one right after construction and must do so explicitly — leaving
  // it to this default would let the knob quietly turn a two-engine divergence harness into two
  // copies of the same engine, which then reports "no divergence".
  engine = psx::config::selected_engine();
  // Snapshot the game-owned polymorphic runtime. The two legacy views are non-null only when the
  // bounded adapter was installed by a consumer that has not migrated this seam yet.
  runtime = psxport_game_runtime();
  guestProgramImage = runtime ? runtime->guestProgramImage() : nullptr;
  cfg = psxport_game_config();
  hooks = psxport_game_hooks();
  if (runtime) {
    gameCtx = runtime->createContext(*this);
  }
}

Core::~Core() {
  if (runtime) {
    runtime->destroyContext(gameCtx);
  }
  gameCtx = nullptr;
}
