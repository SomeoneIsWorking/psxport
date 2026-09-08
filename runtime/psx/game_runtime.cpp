#include "game_runtime.h"

#include "frame_pacer.h"
#include "frame_presenter.h"
#include "game.h"

#include <lucent/log.h>

#include <cstdlib>

namespace {
GameRuntime *installedRuntime;
}

std::unique_ptr<TemporalFramePresentation> GameRuntime::createTemporalFramePresentation(Game &) {
  return nullptr;
}

void GameRuntime::pacePresentation(Core &core, int guestFields, int parts) {
  gpu_pace_subframe_fields(&core, guestFields, parts);
}

std::unique_ptr<FrameDriver> GameRuntime::createFrameDriver(Game &game) {
  (void)game;
  return nullptr;
}

void psxport_install_game(GameRuntime &runtime) {
  installedRuntime = &runtime;
}

GameRuntime *psxport_game_runtime() {
  return installedRuntime;
}

bool game_guest_vram_is_picture(const Game &game) {
  if (!game.runtime) {
    lucent::error("present", "Game has no installed GameRuntime; refusing to infer guest-VRAM ownership");
    std::abort();
  }
  return game.runtime->guestVramIsPicture(game);
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
