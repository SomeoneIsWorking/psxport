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

  // ---- SKIP-mode frame alignment (USER 2026-07-10) ----------------------------------------------
  // MODE=skip compares core A (pc_skip=true, the real ./run.sh shortcut config) against core B (the
  // pure recomp oracle). A's collapsed-multi-step forks (Engine::startBinStage et al) finish a
  // multi-frame substrate load in one native call, so left alone A's game state races ahead of B's
  // at the SAME lockstep frame index — that's what used to force "settled divergence" (a region must
  // differ for N consecutive frames before it's reported) as a tolerance for legitimate cadence
  // drift. This pair replaces racing-ahead with an explicit barrier: a fork's shortcut leg calls
  // skipRendezvousReached() after doing its (harmless, host-only) work but BEFORE flipping any
  // guest-visible "load complete" state; while the oracle core hasn't reached the same milestone
  // yet, the shortcut leg must idle (no game-state advance) and re-check next frame.
  //
  // skipCompareMode() is the compare-mode gate every fork site must check BEFORE calling
  // skipRendezvousReached() — false (skip the wait) in every context except MODE=skip, so normal
  // ./run.sh play (game->sbs == nullptr) and every other SBS mode (full/gameplay/render/oracle) are
  // completely unaffected. No PSXPORT_* gameplay toggle: this routes entirely through the existing
  // `Game::sbs` back-pointer, which is only non-null under the SBS harness.
  bool skipCompareMode() const;
  // `addr`/`minVal`: a shared-layout guest field BOTH cores' own code independently drives to the
  // same value at the equivalent point in their own control flow (e.g. the stage-0 task's
  // CUR_TASK-relative preload-SM byte at +0x48) — NOT a hand-signalled id, since core B is pure
  // recomp substrate and never calls into this API itself. Reads the CALLING core's *sibling*'s
  // memory at `addr`, returns true once sibling >= minVal. `label` identifies the fork site for the
  // wait-timer/timeout diagnostic (one entry per label in the internal wait registry) and for the
  // end-of-run rendezvous-site report. On a wait that never resolves within the timeout window this
  // ABORTS with both sides' state dumped — a silent hang would be worse than a loud, diagnosable
  // failure (CLAUDE.md fail-fast).
  bool skipRendezvousReached(Core* c, uint32_t addr, uint32_t minVal, const char* label);

private:
  class Impl;
  Impl* mImpl;
};
