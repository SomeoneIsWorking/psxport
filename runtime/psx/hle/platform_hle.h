// class PlatformHle — the HLE table for PSX HARDWARE-SYNC primitives (SCEI libcd/libetc/libmdec/
// libgpu sync/wait functions linked into MAIN.EXE).
//
// This HLE owns measured hardware-service leaves such as VSync, CdReadSync, MDEC synchronization, and
// GPU waits. Each handler must preserve the corresponding guest-visible service contract.
//
// One per Game (`c->game->platform_hle.method()`). The table is a REGISTRATION structure (fixed at
// init, read on every guest call target). In SBS with two Games each has its own table; both
// register the same builtins so lookups are identical.
#pragma once
#include "stock_cd_work_area.h"
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
// outstanding. The consuming title supplies its platform-library entry table.
struct PlatformHlePlan {
  static constexpr int kMaxBindings = 16;

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

  // Stock Sony libcd controller and ISO lookup leaves. The framework completes commands and
  // synchronization against its native CD state, and searches the authenticated disc directly.
  uint32_t cdCommandAddress = 0;
  uint32_t cdSyncAddress = 0;
  uint32_t cdSearchFileAddress = 0;

  // Stock Sony libcd CdGetSector(dest, words): the guest's own sector copy, which an STR player calls
  // from its ready callback. The framework serves it from the sector the drive was positioned on.
  uint32_t cdGetSectorAddress = 0;

  // A direct runtime has no legacy GameConfig. Its measured stock-libcd guest work area must be
  // declared here so the shared native command path can preserve CdLastPos/last-mode state.
  psx::cd::StockCommandWorkArea stockCdWorkArea{};

  // libgpu's DMA timeout pair. Retail derives the deadline from VSync(-1), which a native frame loop
  // makes illegal; the host GPU consumes GP0/DMA work synchronously, so the timeout can never be
  // reached. `arm` publishes a far-future deadline and clears the flag in the two measured guest words,
  // and `check` reports "no timeout" without entering the guest body. A zero var leaves that word
  // untouched. Titles declare the four measured facts; they never carry the handler.
  uint32_t gpuTimeoutArmAddress = 0;
  uint32_t gpuTimeoutCheckAddress = 0;
  uint32_t gpuTimeoutDeadlineVar = 0;
  uint32_t gpuTimeoutFlagVar = 0;

  // Base of the guest-owned per-channel DMA callback table (channel `ch` is `base + 4*ch`), for a
  // title whose own libapi `DMACallback` writes it in guest RAM. The framework delivers each
  // completed transfer to the CURRENT word there, exactly as the BIOS DMA handler would. Zero means
  // the title has no guest table and its callbacks live in the native `DmaCallbackRegistry`.
  uint32_t dmaCallbackTable = 0;

  // Measured libgpu DrawSync entry. The host GPU consumes GP0/DMA work synchronously, so the
  // framework can complete this hardware wait without entering the guest's VSync-based body.
  uint32_t drawSyncAddress = 0;

  // Measured libetc VSync entry. Nonnegative waits are protected native-frame boundaries. Some
  // libetc bodies return a field count for negative queries without waiting; declare that body's
  // measured guest counter address to permit the query. An undeclared query refuses explicitly.
  uint32_t vsyncAddress = 0;
  uint32_t vsyncQueryCounterAddress = 0;

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
  // matching addresses in place and reinstalls their native bindings without growing the table.
  void initBuiltins();

  // Product preflight. A missing measured VSync address would let a retail busy-wait run and report
  // a misleading timeout, so boot refuses before title initialization can enter guest main.
  void requireNativeFrameLoopContract() const;

  [[nodiscard]] bool hasNativeFrameLoopContract() const {
    return mVSyncAddress != 0;
  }

  [[nodiscard]] uint32_t vsyncAddress() const {
    return mVSyncAddress;
  }

  // Register a single (addr → handler) pair. The addr MUST lie in the PSX BIOS-library / I/O-glue
  // window (game/engine FUN_xxxx are top-down owned, never HLE'd here). Returns false when the
  // address is refused or the local table cannot accept it.
  bool register_(uint32_t addr, OverrideFn fn);

  // Fast lookup — called on every guest call target. Uses a [min,max] gate for the common case.
  // Returns nullptr for a miss.
  OverrideFn lookup(uint32_t addr) const;

  // Advances whenever a registration is added or replaced, so a cache of lookup answers can tell that
  // it is stale without repeating the lookup.
  [[nodiscard]] uint64_t revision() const {
    return mRevision;
  }

private:
  // The accepted address windows are GAME data — GameConfig::hle.windowLo/windowHi for a legacy
  // config, the direct runtime's own PlatformHlePlan otherwise — so the guard takes the GAME rather
  // than baking one game's memory map into the framework, and reads the plan from THAT game's
  // runtime (a process can run several, one after another or all at once).
  static bool inBiosWindow(const Game *game, const struct GameConfig *cfg, uint32_t a);
  void bindVSyncBoundary(uint32_t addr, uint32_t queryCounterAddr);
  static void vsync(Core *core);

  static constexpr int kMax = 32;

  uint32_t mAddr[kMax] = {0};
  OverrideFn mFn[kMax] = {nullptr};
  int mN = 0;
  uint32_t mLo = 0xFFFFFFFFu;
  uint32_t mHi = 0;
  uint32_t mVSyncAddress = 0;
  uint32_t mVSyncQueryCounterAddress = 0;
  uint64_t mRevision = 0;
};
