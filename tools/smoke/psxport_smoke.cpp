// psxport_smoke.cpp — the game-AGNOSTIC framework smoke.
//
// P1.7 gate: this TU links against ONLY libpsxport.a (+ the framework's inherited system deps). It
// pulls in NO game/* and NO generated/* object. Any undefined game/generated symbol reachable from a
// Core-only client therefore fails THIS link — which is precisely the proof (or disproof) of the
// framework's link-level game-agnosticism.
//
// It installs a ZEROED GameConfig + an all-null GameHooks (Core tolerates null ctxCreate/ctxDestroy —
// see runtime/recomp/core.cpp), constructs a Core, does a mem_w32/mem_r32 round-trip against the Core's
// inline 2 MB main RAM, destructs, and prints "psxport_smoke ok".
#include <cstdio>
#include <cstdlib>
#include "core.h"          // Core, mem_w32/mem_r32 — framework header (no game.h in its include chain)
#include "game_iface.h"    // GameConfig / GameHooks / psxport_install_game

int main() {
  // Stub seam: a zeroed config and an all-null hooks table. Null ctxCreate/ctxDestroy is fine — Core's
  // ctor/dtor guard them (`if (hooks && hooks->ctxCreate) ...`). No game code exists to install a real one.
  static const GameConfig stub_cfg{};   // all-zero guest addresses/tables
  static const GameHooks  stub_hooks{}; // all members nullptr

  psxport_install_game(&stub_cfg, &stub_hooks);

  // Verify the seam stored what we installed (framework-side storage, game_iface.cpp).
  if (psxport_game_config() != &stub_cfg || psxport_game_hooks() != &stub_hooks) {
    fprintf(stderr, "psxport_smoke: install/read-back mismatch\n");
    return 1;
  }

  Core* c = new Core();   // ctxCreate is null -> gameCtx stays nullptr (Core tolerates it)
  if (c->gameCtx != nullptr) {
    fprintf(stderr, "psxport_smoke: expected null gameCtx with null ctxCreate\n");
    return 1;
  }

  // Trivial guest-memory sanity op against the Core's inline main RAM.
  const uint32_t addr = 0x80010000u, val = 0xDEADBEEFu;
  c->mem_w32(addr, val);
  const uint32_t got = c->mem_r32(addr);
  if (got != val) {
    fprintf(stderr, "psxport_smoke: mem round-trip failed: wrote %08x read %08x\n", val, got);
    delete c;
    return 1;
  }

  delete c;
  printf("psxport_smoke ok\n");
  return 0;
}
