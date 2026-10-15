// A title binds its StockLibcd from registerOverrides.
#include "game.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "r3000.h"
#include "stock_libcd.h"
#include "stock_libgpu.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kCdRead = 0x80066A50u;
constexpr uint32_t kCdCommand = 0x80066A60u;
constexpr uint32_t kCdSync = 0x80066A70u;
constexpr uint32_t kCdSearchFile = 0x80066A80u;
constexpr uint32_t kDrawSync = 0x80066C00u;

class StockRuntime final : public psx::test::StubGameRuntime {
public:
  void registerOverrides(Game &game) override {
    libcd.bind(game);
    libgpu.bind(game);
  }

  const psx::cd::StockLibcd libcd{
      {.read = kCdRead, .command = kCdCommand, .sync = kCdSync, .searchFile = kCdSearchFile}, {}, 0};
  const psx::hle::StockLibgpu libgpu{{.drawSync = kDrawSync}};
};

constexpr uint32_t kResult = 0x80110000u;

// CdSync's body reports a completed drive (v0 = 2) whoever reaches it.
void checkSyncBody(Core &core, OverrideFn entry) {
  CHECK(entry != nullptr);
  if (!entry) {
    return;
  }
  core.r[psx::cpu::kA0] = 0;
  core.r[psx::cpu::kA1] = kResult;
  core.r[psx::cpu::kV0] = 0xDEADBEEFu;
  entry(&core);
  CHECK_EQ(core.r[psx::cpu::kV0], 2u);
}

void test_runtime_stock_libcd_binds_from_register_overrides() {
  StockRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  runtime.registerOverrides(*game);

  CHECK(game->cd.stockLibrary() == &runtime.libcd);
  CHECK(game->platform_hle.lookup(kCdRead) != nullptr);
  CHECK(game->platform_hle.lookup(kCdCommand) != nullptr);
  CHECK(game->platform_hle.lookup(kCdSearchFile) != nullptr);
  checkSyncBody(game->core, game->platform_hle.lookup(kCdSync));
  CHECK(game->platform_hle.lookup(kCdSearchFile + 4u) == nullptr);
  CHECK(game->platform_hle.lookup(kDrawSync) != nullptr);
}

} // namespace

int main() {
  RUN(runtime_stock_libcd_binds_from_register_overrides);
  return psx::test::summary();
}
