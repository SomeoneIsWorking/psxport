// game/world/verify_gate.h — class VerifyGate: shared A/B verify-gate helper for the WORLD OBJECT subsystem.
//
// PROPER OOP: one instance per Core, reached as `c->engine.verifyGate.run(...)`. Back-pointer `core` wired
// once at Core construction time (same pattern as Spawn / Placement / Animation).
//
// run() runs a native object-subsystem fn, snapshots+rolls back, super-calls the recomp body, and diffs
// full main-RAM (excluding the dead stack window) + scratchpad + v0. Shared by spawn.cpp and
// graphics_bind.cpp. Gates are DORMANT diagnostic channels (REPL `debug <chan>`); off by default. The
// per-gate scratch buffers + match counters live on the instance (were file-scope statics in the old
// record_gate free function, per [[deglobalize_game_and_runtime_oop]]).
#ifndef GAME_WORLD_VERIFY_GATE_H
#define GAME_WORLD_VERIFY_GATE_H
#include <cstdint>
struct Core;
void rec_super_call(Core*, uint32_t);

class VerifyGate {
public:
  Core* core = nullptr;

  // Run the native `fn` under snapshot-rollback + super-call diff, when `on` is nonzero. When `on == 0`,
  // just run `fn` and stash its return in c->r[2] (the untraced fast path).
  void run(uint32_t (*fn)(Core*), uint32_t super_addr, const char* gate, int on);

private:
  // Per-Core snapshot scratch (2 MB main RAM x2), allocated lazily on first traced run.
  uint8_t* ram0 = nullptr;
  uint8_t* ramN = nullptr;
  // Per-gate log rate-limit + match counters (were file-scope statics ng/nb).
  long nMatch = 0;
  long nMismatch = 0;
};

#endif
