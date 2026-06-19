// Game — the WHOLE machine as ONE per-instance object, so NOTHING is a file-scope global.
//
// Goal (user directive, 2026-06-19): run TWO cores in one process in lockstep (overrides-ON vs
// overrides-OFF) and diff their state to find the first divergence. That requires every piece of
// mutable machine state to live on an instance — no `static` file-scope variables anywhere in the
// runtime. `Game` is that instance: it OWNS the CPU/RAM `Core` plus every subsystem's state.
//
// Threading: `Core*` is already passed through the entire interpreter + generated shards, so we do
// NOT re-thread the CPU handle. Instead `Core` carries a back-pointer `core.game`, and any code that
// holds a `Core* c` reaches migrated subsystem state via `c->game->...`. New subsystem code may take
// a `Game*` directly. Each subsystem's former file-scope statics become a `*State` sub-struct member
// here, migrated ONE subsystem per phase, 0-diff-verified each step (see docs/game-deglobalize-plan.md).
#pragma once
#include "core.h"
#include <stdint.h>

// ---- per-subsystem state structs (former file-scope statics), migrated one phase at a time ----

// timing.cpp — native VBlank/VSync frame clock.
struct TimingState {
  uint32_t vblank = 0;   // libetc VSync counter mirror (was g_vblank)
};

// cd_override.cpp — deferred ingame-music state (suppressed during dialog, resumed after).
struct CdState {
  int      pending_music = 0;   // a looping ingame-music clip is deferred/remembered (was s_pending_music)
  uint8_t  pm_chan  = 0;        // was s_pm_chan
  uint32_t pm_start = 0;        // was s_pm_start
  uint32_t pm_end   = 0;        // was s_pm_end
};

class Game {
public:
  Core core;            // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  TimingState timing;
  CdState     cd;

  Game() { core.game = this; }
};
