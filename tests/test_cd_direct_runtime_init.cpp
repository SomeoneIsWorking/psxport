// A direct GameRuntime binds its StockLibcd from registerOverrides; the legacy adapter's session binds
// one from its GameConfig.
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "stock_libcd.h"
#include "stock_libgpu.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kDirectCdRead = 0x80066A50u;
constexpr uint32_t kDirectCdCommand = 0x80066A60u;
constexpr uint32_t kDirectCdSync = 0x80066A70u;
constexpr uint32_t kDirectCdSearchFile = 0x80066A80u;
constexpr uint32_t kDirectDrawSync = 0x80066C00u;
constexpr uint32_t kLegacyCdSync = 0x800647A0u;
constexpr uint32_t kLegacyCdCommand = 0x800647B0u;
constexpr uint32_t kLegacyCdSearchFile = 0x800647C0u;
constexpr uint32_t kLegacyCdWindowEnd = 0x80064810u;

class StockDirectRuntime final : public psx::test::StubGameRuntime {
public:
  void registerOverrides(Game &game) override {
    libcd.bind(game);
    libgpu.bind(game);
  }

  const psx::cd::StockLibcd libcd{
      {.read = kDirectCdRead, .command = kDirectCdCommand, .sync = kDirectCdSync, .searchFile = kDirectCdSearchFile},
      {},
      0};
  const psx::hle::StockLibgpu libgpu{{.drawSync = kDirectDrawSync}};
};

enum { V0 = 2, A0 = 4, A1 = 5 };
constexpr uint32_t kResult = 0x80110000u;

// CdSync's body reports a completed drive (v0 = 2) whoever reaches it.
void checkSyncBody(Core &core, OverrideFn entry) {
  CHECK(entry != nullptr);
  if (!entry) {
    return;
  }
  core.r[A0] = 0;
  core.r[A1] = kResult;
  core.r[V0] = 0xDEADBEEFu;
  entry(&core);
  CHECK_EQ(core.r[V0], 2u);
}

void test_direct_runtime_stock_libcd_binds_after_the_device_binds() {
  StockDirectRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  // The shipping TitleSession::boot order: device binds, then the title's registerOverrides.
  game->platform_hle.initBuiltins();
  runtime.registerOverrides(*game);

  CHECK(game->core.cfg == nullptr);
  CHECK(game->cd.stockLibrary() == &runtime.libcd);
  CHECK(game->platform_hle.lookup(kDirectCdRead) != nullptr);
  CHECK(game->platform_hle.lookup(kDirectCdCommand) != nullptr);
  CHECK(game->platform_hle.lookup(kDirectCdSearchFile) != nullptr);
  checkSyncBody(game->core, game->platform_hle.lookup(kDirectCdSync));
  CHECK(game->platform_hle.lookup(kDirectCdSearchFile + 4u) == nullptr);
  CHECK(game->platform_hle.lookup(kDirectDrawSync) != nullptr);
}

void test_legacy_runtime_keeps_existing_cd_registration() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.cdSync = kLegacyCdSync;
  config.cdCommand = kLegacyCdCommand;
  config.cdSearchFile = kLegacyCdSearchFile;
  config.hle.windowLo[0] = kLegacyCdSync;
  config.hle.windowHi[0] = kLegacyCdWindowEnd;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK_EQ(game->core.cfg, &config);
  CHECK(game->cd.stockLibrary() != nullptr);
  CHECK(game->platform_hle.lookup(kLegacyCdCommand) != nullptr);
  CHECK(game->platform_hle.lookup(kLegacyCdSearchFile) != nullptr);
  checkSyncBody(game->core, game->platform_hle.lookup(kLegacyCdSync));
}

} // namespace

int main() {
  RUN(direct_runtime_stock_libcd_binds_after_the_device_binds);
  RUN(legacy_runtime_keeps_existing_cd_registration);
  return pt_summary();
}
