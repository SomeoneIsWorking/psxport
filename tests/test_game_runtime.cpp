#include "config_vars.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "guest_address_range.h"
#include "menu_row.h"
#include "render_path_control.h"
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

int legacyContextToken;
int legacyContextsCreated;
int legacyContextsDestroyed;
int legacyBootInitializations;
int legacyReplCommands;
std::string legacyReplCommandName;
std::string legacyReplCommandLine;

void *legacy_create_context(Core *) {
  ++legacyContextsCreated;
  return &legacyContextToken;
}

void legacy_destroy_context(void *context) {
  CHECK_EQ(context, &legacyContextToken);
  ++legacyContextsDestroyed;
}

void legacy_boot_init(Core *) {
  ++legacyBootInitializations;
}

int legacyTeleports;

void legacy_cam_teleport(Core *, int, int, int) {
  ++legacyTeleports;
}

void legacy_cam_teleport_off(Core *) {}

bool legacy_repl_command(Core *, const char *command, const char *line) {
  legacyReplCommandName = command;
  legacyReplCommandLine = line;
  ++legacyReplCommands;
  return true;
}

class TestFrameDriver final : public psx::game::FrameDriver {
public:
  void stepFrame(Core &, uint32_t) override {}
};

class TestTaskScheduler final : public psx::game::TaskScheduler {
public:
  void step() override {}
  void yield(Core &) override {}
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
  RenderCapabilities renderCapabilities() const override {
    return capabilities;
  }

  bool guestVramIsPicture(const Game &game) const override {
    return guestVramPicture || &game == pictureGame;
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

  std::unique_ptr<psx::game::TaskScheduler> createTaskScheduler(Game &game) override {
    factoriesSawWiredGame = factoriesSawWiredGame && game.core.game == &game;
    ++taskSchedulersCreated;
    return std::make_unique<TestTaskScheduler>();
  }

  int sessionsCreated = 0;
  int sessionsDestroyed = 0;
  int overrideRegistrations = 0;
  int programStarts = 0;
  int frameDriversCreated = 0;
  int taskSchedulersCreated = 0;
  bool factoriesSawWiredGame = true;
  bool guestVramPicture = false;
  const Game *pictureGame = nullptr;
  RenderCapabilities capabilities = RenderCapabilities::direct();
};

class MigratingRuntime final : public LegacyGameRuntimeAdapter {
public:
  MigratingRuntime(const GameConfig &config, const GameHooks &hooks) : LegacyGameRuntimeAdapter(config, hooks) {}

  void startProgram(Core &) override {
    ++programStarts;
  }

  int programStarts = 0;
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

  CHECK_EQ(game->core.cfg, nullptr);
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

void test_title_commands_are_game_owned_and_legacy_forwarding_is_optional() {
  TestRuntime direct;
  auto directGame = std::make_unique<Game>(direct, psx::config::Config{});
  const psx::game::TitleCommandReply unhandled = psx::game::runTitleCommand(directGame->core, "ents");
  CHECK(!unhandled.handled);
  CHECK(unhandled.text.empty());

  static const GameConfig config{};
  GameHooks emptyHooks{};
  LegacyGameRuntimeAdapter emptyLegacy(config, emptyHooks);
  auto emptyGame = std::make_unique<Game>(emptyLegacy, psx::config::Config{});
  CHECK(!psx::game::runTitleCommand(emptyGame->core, "tp 1 2 3").handled);

  GameHooks forwardingHooks{};
  forwardingHooks.replCommand = legacy_repl_command;
  forwardingHooks.replCamTeleport = legacy_cam_teleport;
  forwardingHooks.replCamTeleportOff = legacy_cam_teleport_off;
  LegacyGameRuntimeAdapter forwardingLegacy(config, forwardingHooks);
  auto forwardingGame = std::make_unique<Game>(forwardingLegacy, psx::config::Config{});
  legacyReplCommands = 0;
  const psx::game::TitleCommandReply forwarded =
      psx::game::runTitleCommand(forwardingGame->core, "title-command argument\n");
  CHECK(forwarded.handled);
  // A hook that answers only in the log still answers the control channel.
  CHECK(forwarded.text == "title-command: done (reply in the log)");
  CHECK_EQ(legacyReplCommands, 1);
  CHECK(legacyReplCommandName == "title-command");
  CHECK(legacyReplCommandLine == "title-command argument");

  legacyTeleports = 0;
  const psx::game::TitleCommandReply teleport = psx::game::runTitleCommand(forwardingGame->core, "tp 1 2 3");
  CHECK(teleport.handled);
  CHECK(teleport.text == "tp camera -> (1,2,3)");
  CHECK_EQ(legacyTeleports, 1);
  CHECK_EQ(legacyReplCommands, 1);
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

void test_legacy_pair_is_bounded_by_runtime_adapter() {
  legacyContextsCreated = 0;
  legacyContextsDestroyed = 0;

  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  LegacyGameRuntimeAdapter runtime(config, hooks);

  {
    auto game = std::make_unique<Game>(runtime, psx::config::Config{});
    CHECK_EQ(game->core.cfg, &config);
    CHECK_EQ(game->core.hooks, &hooks);
    CHECK_EQ(game->core.gameCtx, &legacyContextToken);
    CHECK_EQ(legacyContextsCreated, 1);
  }

  CHECK_EQ(legacyContextsDestroyed, 1);
}

void test_legacy_adapter_supports_incremental_inheritance() {
  legacyBootInitializations = 0;

  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.bootInit = legacy_boot_init;
  MigratingRuntime runtime(config, hooks);

  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK(&game->core.runtime == &runtime);
  game->runtime.startProgram(game->core);
  CHECK_EQ(runtime.programStarts, 1);
  CHECK_EQ(legacyBootInitializations, 0);
}

void test_game_owns_runtime_products_and_session() {
  TestRuntime runtime;

  {
    auto game = std::make_unique<Game>(runtime, psx::config::Config{});
    CHECK(&game->core.runtime == &runtime);
    CHECK(game->sessionState != nullptr);
    CHECK(game->temporalPresentation == nullptr);
    CHECK(game->frameDriver != nullptr);
    CHECK(game->taskScheduler != nullptr);
    CHECK_EQ(runtime.sessionsCreated, 1);
    CHECK_EQ(runtime.frameDriversCreated, 1);
    CHECK_EQ(runtime.taskSchedulersCreated, 1);
    CHECK(runtime.factoriesSawWiredGame);
    game->runtime.registerOverrides(*game);
    game->runtime.startProgram(game->core);
    CHECK_EQ(runtime.overrideRegistrations, 1);
    CHECK_EQ(runtime.programStarts, 1);
    CHECK_EQ(runtime.sessionsDestroyed, 0);
  }

  CHECK_EQ(runtime.sessionsDestroyed, 1);
}

void test_only_legacy_adapter_installs_the_temporal_compatibility_decorator() {
  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  LegacyGameRuntimeAdapter runtime(config, hooks);

  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK(game->temporalPresentation != nullptr);
}

void test_runtime_capabilities_are_explicit_and_preserve_legacy_temporal_titles() {
  TestRuntime direct;
  const RenderCapabilities directCapabilities = direct.renderCapabilities();
  CHECK(directCapabilities.supports(RenderPath::Native));
  CHECK(!direct.interpolates());

  static const GameConfig config{};
  static GameHooks hooks{};
  LegacyGameRuntimeAdapter legacy(config, hooks);
  const RenderCapabilities legacyCapabilities = legacy.renderCapabilities();
  CHECK(legacyCapabilities.supports(RenderPath::Native));
  CHECK(legacy.interpolates());

  const RenderCapabilities widescreenOnly = RenderCapabilities::widescreenOnly();
  CHECK(!widescreenOnly.supports(RenderPath::Native));
}

void test_live_render_path_validator_separates_player_and_diagnostic_use() {
  TestRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(render_path_apply(*game, RenderPath::Gte, RenderPathAudience::Player) == RenderPathSelectionResult::Applied);
  CHECK(game->core.rsub.mode.path() == RenderPath::Gte);
  CHECK(render_path_apply(*game, RenderPath::Device, RenderPathAudience::Player) ==
        RenderPathSelectionResult::Unsupported);
  CHECK(game->core.rsub.mode.path() == RenderPath::Gte);
  CHECK(render_path_apply(*game, RenderPath::Device, RenderPathAudience::Diagnostic) ==
        RenderPathSelectionResult::Applied);
  CHECK(game->core.rsub.mode.path() == RenderPath::Device);

  CHECK(render_path_apply(*game, RenderPath::Native, RenderPathAudience::Diagnostic) ==
        RenderPathSelectionResult::Applied);
  CHECK(game->core.rsub.mode.path() == RenderPath::Native);
}

void test_widescreen_only_runtime_refuses_native_through_shipping_validator() {
  TestRuntime runtime;
  runtime.capabilities = RenderCapabilities::widescreenOnly();
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->core.rsub.mode.setPath(RenderPath::Gte);

  CHECK(render_path_apply(*game, RenderPath::Native, RenderPathAudience::Diagnostic) ==
        RenderPathSelectionResult::Unsupported);
  CHECK(game->core.rsub.mode.path() == RenderPath::Gte);
  CHECK(render_path_apply(*game, RenderPath::Device, RenderPathAudience::Diagnostic) ==
        RenderPathSelectionResult::Applied);
}

// A title that cannot do the requested path gets its DECLARED path on its own Core, and the
// process-global ladder keeps the request: rewriting the fallback there would hand the next Core
// created in this process a path its title never asked for.
void test_startup_refuses_unsupported_native_and_leaves_the_process_ladder_alone() {
  TestRuntime runtime;
  runtime.capabilities = RenderCapabilities::widescreenOnly();
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  psx::config::cv_render_path.set(psx::config::Layer::Runtime, "native");

  game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(game->core.rsub.mode.path() == RenderPath::Gte);
  CHECK_STREQ(psx::config::cv_render_path.get().c_str(), "native");
  CHECK(psx::config::cv_render_path.layer() == psx::config::Layer::Runtime);
}

// A Core composed through Game alone lands on its runtime's declared path; the title calls nothing.
void test_game_construction_installs_the_declared_render_path() {
  TestRuntime runtime;
  runtime.capabilities = RenderCapabilities::widescreenOnly();
  runtime.capabilities.defaultPath = RenderPath::Record;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(game->core.rsub.mode.path() == RenderPath::Record);
}

void test_capability_absence_removes_player_bindings_while_capable_titles_retain_them() {
  TestRuntime unsupportedRuntime;
  unsupportedRuntime.capabilities = RenderCapabilities::widescreenOnly();
  auto unsupportedGame = std::make_unique<Game>(unsupportedRuntime, psx::config::Config{});
  psx::ui::RenderPathControl unsupportedControl(unsupportedGame.get());
  CHECK(!psx::ui::make_render_path_binding(&unsupportedControl)->available());
  CHECK(!psx::ui::make_mod_toggle_binding(&unsupportedGame->mods, "fps60")->available());

  static const GameConfig config{};
  static GameHooks hooks{};
  LegacyGameRuntimeAdapter capableRuntime(config, hooks);
  auto capableGame = std::make_unique<Game>(capableRuntime, psx::config::Config{});
  psx::ui::RenderPathControl capableControl(capableGame.get());
  CHECK(psx::ui::make_render_path_binding(&capableControl)->available());
  CHECK(psx::ui::make_mod_toggle_binding(&capableGame->mods, "fps60")->available());
}

void test_guest_vram_picture_policy_is_runtime_owned_and_dynamic() {
  TestRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(!game_guest_vram_is_picture(*game));
  runtime.guestVramPicture = true;
  CHECK(game_guest_vram_is_picture(*game));
}

void test_guest_vram_picture_policy_is_per_game() {
  TestRuntime runtime;
  auto pictureGame = std::make_unique<Game>(runtime, psx::config::Config{});
  auto nativeGame = std::make_unique<Game>(runtime, psx::config::Config{});

  runtime.pictureGame = pictureGame.get();
  CHECK(game_guest_vram_is_picture(*pictureGame));
  CHECK(!game_guest_vram_is_picture(*nativeGame));
}

void test_legacy_adapter_projects_static_backdrop_policy_only_for_migration() {
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;

  GameConfig hiddenConfig{};
  LegacyGameRuntimeAdapter hidden(hiddenConfig, hooks);
  auto hiddenGame = std::make_unique<Game>(hidden, psx::config::Config{});
  CHECK(!game_guest_vram_is_picture(*hiddenGame));

  GameConfig visibleConfig{};
  visibleConfig.preserveVramBackdrop = 1;
  LegacyGameRuntimeAdapter visible(visibleConfig, hooks);
  auto visibleGame = std::make_unique<Game>(visible, psx::config::Config{});
  CHECK(game_guest_vram_is_picture(*visibleGame));
}

} // namespace

int main() {
  RUN(game_and_core_hold_the_runtime_they_are_given);
  RUN(bound_stock_libcd_becomes_the_games_libcd);
  RUN(default_admission_maps_the_executable_and_sets_top_level_registers);
  RUN(title_commands_are_game_owned_and_legacy_forwarding_is_optional);
  RUN(guest_address_ranges_are_validated_and_physically_normalized);
  RUN(legacy_pair_is_bounded_by_runtime_adapter);
  RUN(legacy_adapter_supports_incremental_inheritance);
  RUN(game_owns_runtime_products_and_session);
  RUN(only_legacy_adapter_installs_the_temporal_compatibility_decorator);
  RUN(game_construction_installs_the_declared_render_path);
  RUN(runtime_capabilities_are_explicit_and_preserve_legacy_temporal_titles);
  RUN(live_render_path_validator_separates_player_and_diagnostic_use);
  RUN(widescreen_only_runtime_refuses_native_through_shipping_validator);
  RUN(startup_refuses_unsupported_native_and_leaves_the_process_ladder_alone);
  RUN(capability_absence_removes_player_bindings_while_capable_titles_retain_them);
  RUN(guest_vram_picture_policy_is_runtime_owned_and_dynamic);
  RUN(guest_vram_picture_policy_is_per_game);
  RUN(legacy_adapter_projects_static_backdrop_policy_only_for_migration);
  return pt_summary();
}
