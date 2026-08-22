#include "game.h"
#include "game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

int legacyContextToken;
int legacyContextsCreated;
int legacyContextsDestroyed;
int legacyBootInitializations;

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

class TestFrameDriver final : public FrameDriver {
public:
  void stepFrame(Core &, uint32_t) override {}
};

class TestTaskScheduler final : public TaskScheduler {
public:
  void step() override {}
  void yield(Core &) override {}
};

class TestRuntime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    ++contextsCreated;
    return &contextToken;
  }

  void destroyContext(void *context) override {
    CHECK_EQ(context, &contextToken);
    ++contextsDestroyed;
  }

  void registerOverrides(Game &) override {
    ++overrideRegistrations;
  }
  void bootInit(Core &) override {
    ++bootInitializations;
  }

  std::unique_ptr<FrameDriver> createFrameDriver(Game &game) override {
    factoriesSawWiredGame = factoriesSawWiredGame && game.core.game == &game;
    ++frameDriversCreated;
    return std::make_unique<TestFrameDriver>();
  }

  std::unique_ptr<TaskScheduler> createTaskScheduler(Game &game) override {
    factoriesSawWiredGame = factoriesSawWiredGame && game.core.game == &game;
    ++taskSchedulersCreated;
    return std::make_unique<TestTaskScheduler>();
  }

  int contextToken = 0;
  int contextsCreated = 0;
  int contextsDestroyed = 0;
  int overrideRegistrations = 0;
  int bootInitializations = 0;
  int frameDriversCreated = 0;
  int taskSchedulersCreated = 0;
  bool factoriesSawWiredGame = true;
};

class MigratingRuntime final : public LegacyGameRuntimeAdapter {
public:
  MigratingRuntime(const GameConfig &config, const GameHooks &hooks) : LegacyGameRuntimeAdapter(config, hooks) {}

  void bootInit(Core &) override {
    ++bootInitializations;
  }

  int bootInitializations = 0;
};

void test_installation_reaches_derived_runtime() {
  TestRuntime runtime;
  psxport_install_game(runtime);

  CHECK_EQ(psxport_game_runtime(), &runtime);
}

void test_legacy_pair_is_bounded_by_runtime_adapter() {
  legacyContextsCreated = 0;
  legacyContextsDestroyed = 0;
  legacyBootInitializations = 0;

  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  hooks.bootInit = legacy_boot_init;
  psxport_install_game(&config, &hooks);

  {
    auto core = std::make_unique<Core>();
    CHECK(psxport_game_runtime() != nullptr);
    CHECK_EQ(core->cfg, &config);
    CHECK_EQ(core->hooks, &hooks);
    CHECK_EQ(core->gameCtx, &legacyContextToken);
    psxport_game_runtime()->bootInit(*core);
    CHECK_EQ(legacyContextsCreated, 1);
    CHECK_EQ(legacyBootInitializations, 1);
  }

  CHECK_EQ(legacyContextsDestroyed, 1);
}

void test_legacy_adapter_supports_incremental_inheritance() {
  legacyContextsCreated = 0;
  legacyContextsDestroyed = 0;
  legacyBootInitializations = 0;

  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  hooks.bootInit = legacy_boot_init;
  MigratingRuntime runtime(config, hooks);
  psxport_install_game(runtime);

  {
    auto core = std::make_unique<Core>();
    CHECK_EQ(core->runtime, &runtime);
    CHECK_EQ(core->cfg, &config);
    CHECK_EQ(core->hooks, &hooks);
    CHECK_EQ(core->gameCtx, &legacyContextToken);
    core->runtime->bootInit(*core);
    CHECK_EQ(runtime.bootInitializations, 1);
    CHECK_EQ(legacyBootInitializations, 0);
  }

  CHECK_EQ(legacyContextsCreated, 1);
  CHECK_EQ(legacyContextsDestroyed, 1);
}

void test_game_owns_runtime_products_and_context() {
  TestRuntime runtime;
  psxport_install_game(runtime);

  {
    auto game = std::make_unique<Game>();
    CHECK_EQ(game->core.runtime, &runtime);
    CHECK_EQ(game->core.gameCtx, &runtime.contextToken);
    CHECK(game->frameDriver != nullptr);
    CHECK(game->taskScheduler != nullptr);
    CHECK_EQ(runtime.contextsCreated, 1);
    CHECK_EQ(runtime.frameDriversCreated, 1);
    CHECK_EQ(runtime.taskSchedulersCreated, 1);
    CHECK(runtime.factoriesSawWiredGame);
    game->runtime->registerOverrides(*game);
    game->runtime->bootInit(game->core);
    CHECK_EQ(runtime.overrideRegistrations, 1);
    CHECK_EQ(runtime.bootInitializations, 1);
  }

  CHECK_EQ(runtime.contextsDestroyed, 1);
}

} // namespace

int main() {
  RUN(installation_reaches_derived_runtime);
  RUN(legacy_pair_is_bounded_by_runtime_adapter);
  RUN(legacy_adapter_supports_incremental_inheritance);
  RUN(game_owns_runtime_products_and_context);
  return pt_summary();
}
