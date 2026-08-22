#include "game.h"
#include "game_runtime.h"
#include "guest_program_image.h"
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
  const GuestProgramImage *guestProgramImage() const override {
    return &programImage;
  }

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
  GuestProgramImage programImage{
      .bss = {0x800BE0D8u, 0x80106228u},
      .stackTopWordAddress = 0x800A3F88u,
      .stackReserveWordAddress = 0x800A3F8Cu,
      .heapBase = 0x80106228u,
      .heapSizeStoreAddress = 0x800ABEF8u,
      .heapBaseStoreAddress = 0x800ABEF4u,
      .globalPointer = 0x800BE0D4u,
      .libcInitEntry = 0x80089860u,
      .gameMainEntry = 0x80050B08u,
      .crt0Entry = 0x800896E0u,
      .residentText = {0x00010000u, 0x00100000u},
      .backtraceText = {0x00010000u, 0x00120000u},
      .stackBias = {true, -8},
  };
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

  GuestProgramImage image{
      .residentText = range,
      .backtraceText = missingEnd,
  };
  CHECK_EQ(image.effectiveBacktraceText().begin, range.begin);
  CHECK_EQ(image.effectiveBacktraceText().end, range.end);
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

  static GameConfig config{};
  config.bssZeroLo = 0x800BE0D8u;
  config.bssZeroHi = 0x80106228u;
  config.stackTopBase = 0x800A3F88u;
  config.stackTopBase2 = 0x800A3F8Cu;
  config.heapBase = 0x80106228u;
  config.heapSizePtr = 0x800ABEF8u;
  config.heapBasePtr = 0x800ABEF4u;
  config.gp = 0x800BE0D4u;
  config.libcInit = 0x80089860u;
  config.gameMain = 0x80050B08u;
  config.crt0 = 0x800896E0u;
  config.recMainLo = 0x00010000u;
  config.recMainHi = 0x00100000u;
  config.hle.codeScanLo = 0x00010000u;
  config.hle.codeScanHi = 0x00120000u;
  config.stackBias = {1u, -8};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  hooks.bootInit = legacy_boot_init;
  MigratingRuntime runtime(config, hooks);
  psxport_install_game(runtime);

  {
    auto core = std::make_unique<Core>();
    CHECK_EQ(core->runtime, &runtime);
    CHECK(core->guestProgramImage != nullptr);
    CHECK_EQ(core->guestProgramImage, runtime.guestProgramImage());
    CHECK_EQ(core->guestProgramImage->bss.begin, config.bssZeroLo);
    CHECK_EQ(core->guestProgramImage->bss.end, config.bssZeroHi);
    CHECK_EQ(core->guestProgramImage->stackBias.bytes, -8);
    CHECK(core->guestProgramImage->backtraceText.containsPhysical(0x80110000u));
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
    CHECK_EQ(game->core.guestProgramImage, &runtime.programImage);
    CHECK_EQ(game->core.gameCtx, &runtime.contextToken);
    CHECK(game->temporalPresentation == nullptr);
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

void test_only_legacy_adapter_installs_the_temporal_compatibility_decorator() {
  static const GameConfig config{};
  static GameHooks hooks{};
  hooks.ctxCreate = legacy_create_context;
  hooks.ctxDestroy = legacy_destroy_context;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  psxport_install_game(runtime);

  auto game = std::make_unique<Game>();
  CHECK(game->temporalPresentation != nullptr);
}

} // namespace

int main() {
  RUN(installation_reaches_derived_runtime);
  RUN(guest_address_ranges_are_validated_and_physically_normalized);
  RUN(legacy_pair_is_bounded_by_runtime_adapter);
  RUN(legacy_adapter_supports_incremental_inheritance);
  RUN(game_owns_runtime_products_and_context);
  RUN(only_legacy_adapter_installs_the_temporal_compatibility_decorator);
  return pt_summary();
}
