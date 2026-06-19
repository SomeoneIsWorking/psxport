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

// hle.cpp — BIOS HLE: event control blocks, native first-fit heap, IRQ/work-area flags.
struct HleEvCB { int open, enabled, fired; uint32_t ev_class, spec, mode, func; };  // was EvCB
struct HleHeapBlock { uint32_t addr, size; int used; };                             // was HeapBlock
struct HleState {
  HleEvCB     ev[16]      = {};   // was s_ev[EVCB_MAX]
  HleHeapBlock blk[4096]  = {};   // was s_blk[HEAP_MAX_BLOCKS]
  int      nblk       = 0;        // was s_nblk
  uint32_t heap_base  = 0;        // was s_heap_base
  uint32_t heap_size  = 0;        // was s_heap_size
  int      heap_ok    = 0;        // was s_heap_ok
  int      work_ok    = 0;        // was s_work_ok
  uint32_t int_handler = 0;       // was s_int_handler (B0:0x19 HookEntryInt)
  int      irq_enabled = 1;       // was s_irq_enabled
};

// pad_input.cpp — native controller input: current host button state + the REPL drive control.
// (Test-hook / config-cache statics inside pad_service_frame stay shared per the plan policy.)
struct PadState {
  uint16_t buttons    = 0xFFFF;  // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold  = 0xFFFF;  // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap   = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int      repl_tap_n = 0;       // REPL: tap countdown frames (was s_repl_tap_n)
  int      repl_on    = 0;       // REPL drive active (was s_repl_on)
};

class Game {
public:
  Core core;            // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  TimingState timing;
  CdState     cd;
  HleState    hle;
  PadState    pad;

  Game() { core.game = this; }
};
