// class PlatformHle — the HLE table for PSX HARDWARE-SYNC primitives (SCEI libcd/libetc/libmdec/
// libgpu sync/wait functions linked into MAIN.EXE).
//
// This HLE owns measured hardware-service leaves such as VSync, CdReadSync, MDEC synchronization, and
// GPU waits. Each handler must preserve the corresponding guest-visible service contract.
//
// One per Game (`c->game->platform_hle`): the stock library classes (psx::hle::Stock*) bind into it from
// the title's registerOverrides, and the legacy adapter's remaining GameConfig group binds in initBuiltins.
#pragma once
#include <cstdint>
class Core;
class Game;
namespace psx::hle {
class StockLibgpu;
class StockVSync;
} // namespace psx::hle

// OverrideFn is defined in scheduler.h — the (Core*)->void signature every HLE handler obeys.
typedef void (*OverrideFn)(Core *c);

// The legacy GameConfig::hle window count.
inline constexpr int kPlatformHleWindowCapacity = 4;

class PlatformHle {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  // The legacy adapter's GameConfig::hle group. Boot may call this more than once; registration
  // replaces matching addresses in place without growing the table.
  void initBuiltins();

  // A stock library entry; zero `addr` is an entry the title has not located. Aborts on a full table.
  void bindStock(uint32_t addr, OverrideFn fn);
  // The libetc VSync entry as the protected native frame boundary.
  void bindVSync(const psx::hle::StockVSync &vsync);
  const psx::hle::StockVSync *vsync() const {
    return mVSync;
  }
  // The title's libgpu, whose timeout arm this table dispatches.
  void bindLibgpu(const psx::hle::StockLibgpu &libgpu) {
    mLibgpu = &libgpu;
  }
  const psx::hle::StockLibgpu *libgpu() const {
    return mLibgpu;
  }
  // Completes a hardware wait the host already finished: v0 = 0.
  static void completeImmediately(Core *core);

  // Product preflight. A missing measured VSync address would let a retail busy-wait run and report
  // a misleading timeout, so boot refuses before title initialization can enter guest main.
  void requireNativeFrameLoopContract() const;

  [[nodiscard]] bool hasNativeFrameLoopContract() const {
    return vsyncAddress() != 0;
  }

  [[nodiscard]] uint32_t vsyncAddress() const;

  // Register a legacy (addr → handler) pair inside the GameConfig::hle windows. Returns false when
  // the address is refused or the table cannot accept it.
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
  static bool inBiosWindow(const struct GameConfig &cfg, uint32_t a);
  bool insert(uint32_t addr, OverrideFn fn);

  static constexpr int kMax = 32;

  uint32_t mAddr[kMax] = {0};
  OverrideFn mFn[kMax] = {nullptr};
  int mN = 0;
  uint32_t mLo = 0xFFFFFFFFu;
  uint32_t mHi = 0;
  const psx::hle::StockVSync *mVSync = nullptr;
  const psx::hle::StockLibgpu *mLibgpu = nullptr;
  uint64_t mRevision = 0;
};
