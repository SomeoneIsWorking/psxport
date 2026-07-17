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
#include <cstring>
#include "core.h"

Core::Core() {
  memset((R3000*)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
  // Snapshot the game seam the game installed at startup (game_iface.h). nullptr until installed —
  // harmless while no framework literal/call-site has been converted to read cfg/hooks yet.
  cfg   = psxport_game_config();
  hooks = psxport_game_hooks();
  // Allocate + wire the game's per-Core subsystem aggregate (Core::gameCtx). The 9 game subsystems
  // that used to be embedded on Core now live in that opaque game-owned aggregate; the framework
  // never names them — the game's ctxCreate hook does all the wiring (game_ctx.cpp).
  if (hooks && hooks->ctxCreate) gameCtx = hooks->ctxCreate(this);
}

Core::~Core() {
  if (hooks && hooks->ctxDestroy) hooks->ctxDestroy(gameCtx);
  gameCtx = nullptr;
}
