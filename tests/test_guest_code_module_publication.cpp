// A title whose loader goes through the stock CdRead publishes each whole read as a code image
// (`psx::code_module::publishStockReadLanding`). Every case drives the shipping path: the stock
// read, the framework's landing announcement, the runtime hook, the image catalog and Lightrec
// dispatch into the loaded bytes. Fake sectors stand in only for the disc.
#include "cd_control.h"
#include "cdc_state.h"
#include "core.h"
#include "execution_control.h"
#include "execution_exit.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_code_module.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <array>
#include <cstdint>
#include <memory>

namespace {

constexpr std::uint32_t kArena = 0x8014C000u;
constexpr std::uint32_t kArenaEnd = 0x80150000u;
constexpr std::uint32_t kModule = 0x8014C788u;
constexpr std::uint32_t kStopHere = 0x800F0000u;
constexpr std::uint32_t kModuleLba = 8696u;

// The next module's first instruction loads this into $v0, so two loads differ in both the
// executed result and their SHA-256.
std::uint32_t gValue = 7u;

int fakeSector(DiscState *, std::uint32_t lba, std::uint8_t *out, std::uint32_t) {
  for (std::uint32_t i = 0; i < 2352u; ++i) {
    out[i] = static_cast<std::uint8_t>(lba + i);
  }
  if (lba == kModuleLba) {
    const std::array<std::uint32_t, 3> instructions{0x24020000u | gValue, 0x03E00008u, 0u};
    for (std::uint32_t word = 0; word < instructions.size(); ++word) {
      for (std::uint32_t byte = 0; byte < 4u; ++byte) {
        out[24u + word * 4u + byte] = static_cast<std::uint8_t>(instructions[word] >> (byte * 8u));
      }
    }
  }
  return 1;
}

class LoaderRuntime final : public GameRuntime {
public:
  explicit LoaderRuntime(GuestAddressRange arena) : arena_(arena) {}
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void stockCdReadLanded(Core &core, const psx::cd::StockReadLanding &landing) override {
    psx::code_module::publishStockReadLanding(core, landing, arena_);
  }

private:
  GuestAddressRange arena_;
};

struct Machine {
  explicit Machine(GameRuntime &runtime) : game(std::make_unique<Game>()) {
    game->runtime = &runtime;
    game->gpu_dev.s_gpu_on = 0;
    game->core.r[29] = 0x801FFF00u;
    game->core.r[30] = 0x801FFF00u;
    game->cdc.disc_read_raw_fn = fakeSector;
  }

  bool read(std::uint32_t sectors, std::uint32_t destination, std::int32_t lba) {
    game->cd.setloc_lba = lba;
    game->core.r[4] = sectors;
    game->core.r[5] = destination;
    game->core.r[6] = 0x80u;
    cd_read_stock_sync(&game->core);
    return game->core.r[2] == 1u;
  }

  // $v0 after the module returns; 0xFFFFFFFF when it did not return through the JIT.
  std::uint32_t run(std::uint32_t entry) {
    Core &core = game->core;
    core.r[31] = kStopHere;
    const auto result = psx::cpu::dispatchGuest(core, entry, psx::cpu::ExecutionBudget::fromCycles(4096u));
    return result.returned() ? core.r[2] : 0xFFFFFFFFu;
  }

  std::unique_ptr<Game> game;
};

GuestAddressRange arena() {
  return {kArena & 0x1fffffffu, kArenaEnd & 0x1fffffffu};
}

} // namespace

static void test_a_read_inside_the_arena_is_published_and_executes() {
  LoaderRuntime runtime(arena());
  Machine machine(runtime);
  Core &core = machine.game->core;
  CHECK(!core.currentImageIdentity(kModule).has_value());

  gValue = 7u;
  CHECK(machine.read(2u, kModule, static_cast<std::int32_t>(kModuleLba)));
  const auto identity = core.currentImageIdentity(kModule);
  CHECK(identity.has_value());
  CHECK(core.currentImageIdentity(kModule + 2u * 2048u - 1u) == identity);
  CHECK(!core.currentImageIdentity(kModule + 2u * 2048u).has_value()); // exactly the landed bytes
  CHECK(core.imageCatalog().describe(*identity)->name.starts_with("CD read SHA-256 "));
  CHECK_EQ(machine.run(kModule), 7u);
  CHECK(core.lightrecExecutor().counters().executedBlocks != 0u);
}

static void test_a_read_outside_or_straddling_the_arena_publishes_nothing() {
  LoaderRuntime runtime(arena());
  Machine machine(runtime);
  Core &core = machine.game->core;
  const auto baseline = core.imageCatalog().activeCount();

  CHECK(machine.read(1u, 0x80110000u, static_cast<std::int32_t>(kModuleLba)));       // below
  CHECK(machine.read(1u, kArenaEnd, static_cast<std::int32_t>(kModuleLba)));         // above
  CHECK(machine.read(2u, kArenaEnd - 2048u, static_cast<std::int32_t>(kModuleLba))); // crosses the end
  CHECK(machine.read(2u, kArena - 2048u, static_cast<std::int32_t>(kModuleLba)));    // crosses the start
  CHECK(!core.currentImageIdentity(0x80110000u).has_value());
  CHECK(!core.currentImageIdentity(kArenaEnd - 2048u).has_value());
  CHECK_EQ(core.imageCatalog().activeCount(), baseline);
  CHECK(!core.executionControl().consume().has_value()); // not a fault, just not code

  CHECK(machine.read(1u, kArena, static_cast<std::int32_t>(kModuleLba))); // exactly at the start
  CHECK(core.currentImageIdentity(kArena).has_value());
}

static void test_repeated_and_different_loads_are_new_generations() {
  LoaderRuntime runtime(arena());
  Machine machine(runtime);
  Core &core = machine.game->core;
  auto &catalog = core.imageCatalog();

  gValue = 7u;
  CHECK(machine.read(2u, kModule, static_cast<std::int32_t>(kModuleLba)));
  const auto first = core.currentImageIdentity(kModule);
  CHECK(first.has_value());
  const auto firstContent = catalog.describe(*first)->contentIdentity;

  // The same bytes again: same content identity, new generation.
  CHECK(machine.read(2u, kModule, static_cast<std::int32_t>(kModuleLba)));
  const auto second = core.currentImageIdentity(kModule);
  CHECK(second.has_value() && *second != *first);
  CHECK(catalog.describe(*second)->contentIdentity == firstContent);

  // A second overlay with different bytes in the same arena replaces the first and runs.
  gValue = 11u;
  CHECK(machine.read(2u, kModule, static_cast<std::int32_t>(kModuleLba)));
  const auto third = core.currentImageIdentity(kModule);
  CHECK(third.has_value() && *third != *second);
  CHECK(catalog.describe(*third)->contentIdentity != firstContent);
  CHECK_EQ(machine.run(kModule), 11u);

  // Two reads of one load, back to back, stay separate images and both resolve.
  CHECK(machine.read(1u, kModule + 0x1000u, static_cast<std::int32_t>(kModuleLba) + 1));
  CHECK(core.currentImageIdentity(kModule + 0x1000u).has_value());
  CHECK(core.currentImageIdentity(kModule) == third);
}

static void test_an_empty_arena_admits_all_of_main_ram() {
  LoaderRuntime runtime(GuestAddressRange{});
  Machine machine(runtime);
  CHECK(machine.read(1u, 0x80110000u, static_cast<std::int32_t>(kModuleLba)));
  CHECK(machine.game->core.currentImageIdentity(0x80110000u).has_value());
}

static void test_an_unpublishable_landing_is_a_runtime_fault() {
  LoaderRuntime runtime(GuestAddressRange{});
  Machine machine(runtime);
  Core &core = machine.game->core;
  const auto baseline = core.imageCatalog().activeCount();

  runtime.stockCdReadLanded(core, {.firstLba = 1u, .sectors = 1u, .destination = 0x801FFC00u, .bytes = 2048u});
  const auto fault = core.executionControl().consume();
  CHECK(fault.has_value());
  CHECK(fault->reason == psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(core.imageCatalog().activeCount(), baseline);
}

static void test_a_read_that_moved_nothing_publishes_nothing() {
  LoaderRuntime runtime(arena());
  Machine machine(runtime);
  Core &core = machine.game->core;
  CHECK(machine.read(0u, kModule, static_cast<std::int32_t>(kModuleLba)));
  CHECK(!machine.read(1u, kModule, -1));
  machine.game->cdc.disc_read_raw_fn = [](DiscState *, std::uint32_t, std::uint8_t *, std::uint32_t) -> int {
    return 0;
  };
  CHECK(!machine.read(1u, kModule, static_cast<std::int32_t>(kModuleLba)));
  CHECK(!core.currentImageIdentity(kModule).has_value());
  CHECK(!core.executionControl().consume().has_value());
}

int main() {
  RUN(a_read_inside_the_arena_is_published_and_executes);
  RUN(a_read_outside_or_straddling_the_arena_publishes_nothing);
  RUN(repeated_and_different_loads_are_new_generations);
  RUN(an_empty_arena_admits_all_of_main_ram);
  RUN(an_unpublishable_landing_is_a_runtime_fault);
  RUN(a_read_that_moved_nothing_publishes_nothing);
  return pt_summary();
}
