#include "game_runtime.h"

#include "frame_presenter.h"

namespace {
GameRuntime *installedRuntime;
}

std::unique_ptr<TemporalFramePresentation> GameRuntime::createTemporalFramePresentation(Game &) {
  return nullptr;
}

void psxport_install_game(GameRuntime &runtime) {
  installedRuntime = &runtime;
}

GameRuntime *psxport_game_runtime() {
  return installedRuntime;
}

void psxport_clear_game_runtime_for_legacy() {
  installedRuntime = nullptr;
}

const GameConfig *psxport_game_config() {
  return installedRuntime ? installedRuntime->legacyConfigForMigration() : nullptr;
}

const GameHooks *psxport_game_hooks() {
  return installedRuntime ? installedRuntime->legacyHooksForMigration() : nullptr;
}
