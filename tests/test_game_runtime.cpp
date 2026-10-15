#include "config.h"
#include "config_vars.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_address_range.h"
#include "menu_row.h"
#include "stock_libcd.h"
#include "stub_game_runtime.h"
#include "testutil.h"
#include "title_command.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

class TestFrameDriver final : public psx::game::FrameDriver {
public:
  void stepFrame(Core &, uint32_t) override {}
};

class TestSession final : public psx::game::TitleSessionState {
public:
  explicit TestSession(int &destroyed) : destroyed_(destroyed) {}
  ~TestSession() override {
    ++destroyed_;
  }
  TestSession(const TestSession &) = delete;
  TestSession &operator=(const TestSession &) = delete;
  TestSession(TestSession &&) = delete;
  TestSession &operator=(TestSession &&) = delete;

private:
  int &destroyed_;
};

class TestRuntime final : public psx::test::StubGameRuntime {
public:
  bool interpolates() const override {
    return interpolating;
  }

  std::unique_ptr<psx::game::TitleSessionState> createSession(Game &game) override {
    factoriesSawWiredGame = factoriesSawWiredGame && game.core.game == &game;
    ++sessionsCreated;
    return std::make_unique<TestSession>(sessionsDestroyed);
  }

  void registerOverrides(Game &) override {
    ++overrideRegistrations;
  }
  void startProgram(Core &) override {
    ++programStarts;
  }

  std::unique_ptr<psx::game::FrameDriver> createFrameDriver(Game &game) override {
    factoriesSawWiredGame = factoriesSawWiredGame && game.core.game == &game;
    ++frameDriversCreated;
    return std::make_unique<TestFrameDriver>();
  }

  int sessionsCreated = 0;
  int sessionsDestroyed = 0;
  int overrideRegistrations = 0;
  int programStarts = 0;
  int frameDriversCreated = 0;
  bool factoriesSawWiredGame = true;
  bool interpolating = false;
};

void test_game_and_core_hold_the_runtime_they_are_given() {
  TestRuntime runtime;
  const auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(&game->runtime == &runtime);
  CHECK(&game->core.runtime == &runtime);
}

void test_bound_stock_libcd_becomes_the_games_libcd() {
  TestRuntime runtime;
  const psx::cd::StockLibcd libcd({}, {}, 0x80123450u);
  const auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(game->cd.stockLibrary() == nullptr);

  libcd.bind(*game);
  CHECK(game->cd.stockLibrary() == &libcd);
  CHECK_EQ(game->cd.stockLibrary()->readySlot(), 0x80123450u);
}

void test_default_admission_maps_the_executable_and_sets_top_level_registers() {
  TestRuntime runtime;
  const auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  std::vector<std::uint8_t> bytes(0x1000u);
  std::memcpy(bytes.data(), "PS-X EXE", 8);
  const auto put = [&bytes](std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
  };
  put(0x10u, 0x80010010u);
  put(0x18u, 0x80010000u);
  put(0x1Cu, 0x800u);
  put(0x800u, 0x03E00008u);

  const psx::cpu::PsxExeLoadResult admitted = runtime.admitExecutable(game->core, bytes);

  CHECK(static_cast<bool>(admitted));
  CHECK_EQ(game->core.pc, 0x80010010u);
  CHECK_EQ(game->core.mem_r32(0x80010000u), 0x03E00008u);
  CHECK_EQ(game->core.r[29], 0x801FFFF0u);
  CHECK_EQ(game->core.r[31], 0xDEAD0000u);

  bytes[0] = 'X';
  game->core.r[31] = 0u;
  CHECK(!runtime.admitExecutable(game->core, bytes));
  CHECK_EQ(game->core.r[31], 0u);
}

void test_an_unclaimed_title_command_is_unhandled() {
  TestRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  const psx::game::TitleCommandReply unhandled = psx::game::runTitleCommand(game->core, "ents");
  CHECK(!unhandled.handled);
  CHECK(unhandled.text.empty());
}

// The world line is the title's text read from its own Core; the framework adds nothing.
class WorldLineRuntime final : public psx::test::StubGameRuntime {
public:
  std::string worldReadout(Core &core) const override {
    return "stage 0x" + std::to_string(core.mem_r32(kStageWord));
  }

  static constexpr std::uint32_t kStageWord = 0x80010000u;
};

void test_world_readout_is_the_titles_line_for_its_core() {
  psx::test::StubGameRuntime silent;
  const auto silentGame = std::make_unique<Game>(silent, psx::config::Config{});
  CHECK(silent.worldReadout(silentGame->core).empty());

  WorldLineRuntime runtime;
  const auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->core.mem_w32(WorldLineRuntime::kStageWord, 7u);
  CHECK(game->runtime.worldReadout(game->core) == "stage 0x7");
}

void test_guest_address_ranges_are_validated_and_physically_normalized() {
  const GuestAddressRange range{0x00010000u, 0x00100000u};
  CHECK(range.valid());
  CHECK(!range.empty());
  CHECK(range.containsPhysical(0x80010000u));
  CHECK(!range.containsPhysical(0x80100000u));

  const GuestAddressRange missingEnd{0x00010000u, 0};
  CHECK(missingEnd.empty());
  CHECK(!missingEnd.valid());
  CHECK(!missingEnd.containsPhysical(0x80010000u));

  const GuestAddressRange inverted{0x00100000u, 0x00010000u};
  CHECK(!inverted.empty());
  CHECK(!inverted.valid());
  CHECK(!inverted.containsPhysical(0x80010000u));
}

void test_game_owns_runtime_products_and_session() {
  TestRuntime runtime;

  {
    auto game = std::make_unique<Game>(runtime, psx::config::Config{});
    CHECK(&game->core.runtime == &runtime);
    CHECK(game->sessionState != nullptr);
    CHECK(game->frameDriver != nullptr);
    CHECK_EQ(runtime.sessionsCreated, 1);
    CHECK_EQ(runtime.frameDriversCreated, 1);
    CHECK(runtime.factoriesSawWiredGame);
    game->runtime.registerOverrides(*game);
    game->runtime.startProgram(game->core);
    CHECK_EQ(runtime.overrideRegistrations, 1);
    CHECK_EQ(runtime.programStarts, 1);
    CHECK_EQ(runtime.sessionsDestroyed, 0);
  }

  CHECK_EQ(runtime.sessionsDestroyed, 1);
}

// Every Game starts on the configured render path, Record unless PSXPORT_RENDER_PATH names another.
void test_game_construction_installs_the_configured_render_path() {
  TestRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK(game->core.rsub.mode.path() == RenderPath::Record);

  psx::config::set_runtime("PSXPORT_RENDER_PATH", "device");
  game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK(game->core.rsub.mode.path() == RenderPath::Device);

  psx::config::set_runtime("PSXPORT_RENDER_PATH", "native");
  game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK(game->core.rsub.mode.path() == RenderPath::Record);
  psx::config::clear_runtime("PSXPORT_RENDER_PATH");
}

void test_fps60_is_offered_only_to_an_interpolating_title() {
  TestRuntime still;
  auto stillGame = std::make_unique<Game>(still, psx::config::Config{});
  CHECK(!psx::ui::make_mod_toggle_binding(&stillGame->mods, "fps60")->available());

  TestRuntime interpolating;
  interpolating.interpolating = true;
  auto interpolatingGame = std::make_unique<Game>(interpolating, psx::config::Config{});
  CHECK(psx::ui::make_mod_toggle_binding(&interpolatingGame->mods, "fps60")->available());
}

} // namespace

int main() {
  RUN(game_and_core_hold_the_runtime_they_are_given);
  RUN(bound_stock_libcd_becomes_the_games_libcd);
  RUN(default_admission_maps_the_executable_and_sets_top_level_registers);
  RUN(an_unclaimed_title_command_is_unhandled);
  RUN(world_readout_is_the_titles_line_for_its_core);
  RUN(guest_address_ranges_are_validated_and_physically_normalized);
  RUN(game_owns_runtime_products_and_session);
  RUN(game_construction_installs_the_configured_render_path);
  RUN(fps60_is_offered_only_to_an_interpolating_title);
  return psx::test::summary();
}
