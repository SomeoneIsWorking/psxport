// class Sbs — the LIVE two-core side-by-side divergence debugger (PSXPORT_SBS=1).
//
// The harness OWNS its two Games. Each Game holds a back-pointer `Game::sbs` set by the harness on
// creation, so any code with a `Core* c` reaches the harness through `c->game->sbs` — no static
// instance() lookup. In standalone (single-Game) `game->sbs` is nullptr.
//
// Pimpl: the harness state (mode, both Game handles, per-pane RGBA buffers, divergence + write-watch
// record, scripted-input list) lives on a private `Impl` defined in sbs.cpp so this header stays
// light and callers only see the public interface.
#pragma once
#include <cstdio>
#include <cstdint>
class Core;
class Game;

class Sbs {
public:
  // Process entry point — never returns (owns the process from PSXPORT_SBS=1 onward). Constructs a
  // stack Sbs, wires it into its two Games, drives them, exit(0).
  static void run(const char* exePath);

  Sbs();
  ~Sbs();
  Sbs(const Sbs&) = delete;
  Sbs& operator=(const Sbs&) = delete;

  bool     active() const;
  int      coreId(Core* c) const;
  uint32_t frame() const;
  int      dbgCmd(FILE* out, const char* line);
  // Write-watch trampoline installed on each Core (core.storeWatchCb) — static so it fits the
  // C callback signature. Dispatches to `c->game->sbs->…` when SBS is active.
  static void storeCb(Core* c, uint32_t addr, uint32_t val, uint32_t width);

  // Per-command core targeting for the debug server: 'a'/'A' → core A, 'b'/'B' → core B, else null.
  Core*    coreByLetter(char which) const;

  // The currently `sbs show`-selected core (0 → A, 1 → B).
  Core*    shownCore() const;

private:
  class Impl;
  Impl* mImpl;
};
