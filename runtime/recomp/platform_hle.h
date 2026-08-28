// class PlatformHle — the HLE table for PSX HARDWARE-SYNC primitives (SCEI libcd/libetc/libmdec/
// libgpu sync/wait functions linked into MAIN.EXE).
//
// This is NOT the removed 2026-06-22 game-override runtime. This HLE covers hardware-service leaves —
// VSync/CdReadSync/DecDCT{in,out}Sync/GPU-timeout — that busy-spin on a hardware IRQ our no-IRQ
// runtime never satisfies. Same category as BIOS A0/B0/C0 HLE in hle.cpp.
//
// One per Game (`c->game->platform_hle.method()`). The table is a REGISTRATION structure (fixed at
// init, read on every interpreted call target). In SBS with two Games each has its own table; both
// register the same builtins so lookups are identical.
#pragma once
#include <cstdint>
struct Core;
class Game;

// OverrideFn is defined in scheduler.h — the (Core*)->void signature every HLE handler obeys.
typedef void (*OverrideFn)(Core *c);

// Both direct runtimes and the legacy adapter use this one capacity for exact, half-open library
// entry windows. Keep the storage and every validation loop tied to it; a duplicated array bound
// silently made the third measured leaf impossible to declare through one of the two seams.
inline constexpr int kPlatformHleWindowCapacity = 4;

// One game-declared hardware-sync primitive: the measured address of a SCEI library leaf (libetc /
// libcd / libmdec / libgpu sync glue) and the native handler that owns it. Addresses are GAME data.
struct PlatformHleBinding {
  uint32_t addr;
  OverrideFn fn;
};

// The consumer-owned fact slice for DIRECT runtimes (core.cfg == nullptr, no legacy GameConfig):
// which hardware-sync primitives the game's binary links and which address windows admit them.
// The windows are what keep engine FUN_xxxx out of this table — the same guard
// GameConfig::hle.windowLo/windowHi provides for adapter runtimes. A runtime that declares nothing
// (nullptr plan or zero windows) installs nothing, and initBuiltins() announces that out loud:
// the guest then spins in any real sync loop it reaches, which is the honest signal that RE is
// outstanding. docs/plans/game-seam-redesign.md, "platform-library entry tables".
struct PlatformHlePlan {
  static constexpr int kMaxBindings = 8;

  // Standard SCEI library leaves whose native behavior is game-independent and framework-owned.
  // A direct runtime supplies only the measured addresses from its executable; initBuiltins()
  // selects the existing framework handlers. Zero means the leaf has not been located.
  uint32_t setGeomOffset = 0;
  uint32_t setGeomScreen = 0;

  // Stock Sony libcd finite-read leaves. When cdReadAddress is declared, the framework owns the
  // whole finite read synchronously; a later ReadN/ReadS command is therefore a continuous stream,
  // never the callback-driven finite-read state machine. Keep this typed so the command owner can
  // distinguish the two paths without treating `core.cfg == nullptr` as game behavior.
  uint32_t cdReadAddress = 0;
  uint32_t cdReadSyncAddress = 0;

  // Measured libgpu DrawSync entry. The host GPU consumes GP0/DMA work synchronously, so the
  // framework can complete this hardware wait without entering the guest's VSync-based body.
  uint32_t drawSyncAddress = 0;

  // Measured libetc VSync entry. Product boot requires this fact and the framework always binds it
  // to its fatal native-frame-loop ownership trap. A title supplies no handler and cannot replace
  // the trap through `bindings`.
  uint32_t vsyncAddress = 0;

  // Title-specific sync leaves remain explicit address/function bindings. Do not use this table to
  // expose a framework-owned standard handler (including VSync): add a typed address above so games
  // cannot duplicate or reach private handler implementations.
  PlatformHleBinding bindings[kMaxBindings] = {};
  int bindingCount = 0;
  // Exact accepted windows; a zero hi disables a slot. Addresses are KSEG0 (0x8xxxxxxx).
  uint32_t windowLo[kPlatformHleWindowCapacity] = {};
  uint32_t windowHi[kPlatformHleWindowCapacity] = {};
};

class PlatformHle {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  // Register the built-in hardware-sync HLE entries (libmdec/libcd/libgpu/libetc VSync +
  // cooperative task-switch ChangeThread). Boot may call this more than once; registration replaces
  // matching addresses in place and reinstalls their generated overrides without growing the table.
  void initBuiltins();

  // Product preflight. A missing measured VSync address would let a retail busy-wait run and report
  // a misleading timeout, so boot refuses before title initialization can enter guest main.
  void requireNativeFrameLoopContract() const;

  [[nodiscard]] uint32_t vsyncAddress() const {
    return mVSyncAddress;
  }

  // Register a single (addr → handler) pair. The addr MUST lie in the PSX BIOS-library / I/O-glue
  // window (game/engine FUN_xxxx are top-down owned, never HLE'd here). Returns false when the
  // address is refused or the local table cannot accept it.
  bool register_(uint32_t addr, OverrideFn fn);

  // Fast lookup — called on every interpreted call target. Uses a [min,max] gate for the common case.
  // Returns nullptr for a miss.
  OverrideFn lookup(uint32_t addr) const;

private:
  // The accepted address windows are GAME data (GameConfig::hle.windowLo/windowHi), so the guard
  // takes the config rather than baking one game's memory map into the framework.
  static bool inBiosWindow(const struct GameConfig *cfg, uint32_t a);
  void bindVSyncTrap(uint32_t addr);

  static constexpr int kMax = 32;

  uint32_t mAddr[kMax] = {0};
  OverrideFn mFn[kMax] = {nullptr};
  int mN = 0;
  uint32_t mLo = 0xFFFFFFFFu;
  uint32_t mHi = 0;
  uint32_t mVSyncAddress = 0;
};
