// class PlatformHle — the process-wide HLE table for PSX HARDWARE-SYNC primitives (SCEI libcd/libetc/
// libmdec/libgpu sync/wait functions linked into MAIN.EXE).
//
// This is NOT the removed 2026-06-22 game-override runtime (which agents abused to flip game-logic
// FUN_xxxx into native C instead of porting top-down). This HLE covers hardware-service leaves —
// VSync/CdReadSync/DecDCT{in,out}Sync/GPU-timeout — that busy-spin on a hardware IRQ our no-IRQ
// runtime never satisfies. Same category as BIOS A0/B0/C0 HLE in hle.cpp.
//
// One singleton per process — the HLE table is a REGISTRATION structure (fixed at init, read on every
// interpreted call target). Both SBS cores share it correctly by design; there is nothing per-Core
// here. Handlers are stateless static methods that take a `Core*` and set its registers/memory
// directly.
#pragma once
#include <cstdint>
struct Core;

// OverrideFn is defined in scheduler.h — the (Core*)->void signature every HLE handler and every
// scheduler-owned override obeys.
typedef void (*OverrideFn)(Core* c);

class PlatformHle {
public:
  static PlatformHle& instance();

  // Register the built-in hardware-sync HLE entries (libmdec/libcd/libgpu/libetc VSync +
  // cooperative task-switch ChangeThread). Called once from boot; safe to call more than once
  // (registrations are idempotent at the same addr; last write wins).
  void initBuiltins();

  // Register a single (addr → handler) pair. The addr MUST lie in the PSX BIOS-library / I/O-glue
  // window (game/engine FUN_xxxx are top-down owned, never HLE'd here). Also wired into the recomp
  // override table so a recompiled call to `addr` resolves through the HLE before running the
  // recompiled busy-wait body.
  void register_(uint32_t addr, OverrideFn fn);

  // Fast lookup — called on every interpreted call target. Uses a [min,max] gate for the common case.
  // Returns nullptr for a miss.
  OverrideFn lookup(uint32_t addr) const;

private:
  static constexpr int kMax = 32;

  uint32_t   mAddr[kMax] = {0};
  OverrideFn mFn[kMax]   = {nullptr};
  int        mN = 0;
  uint32_t   mLo = 0xFFFFFFFFu;
  uint32_t   mHi = 0;

  static bool inBiosWindow(uint32_t a);
};
