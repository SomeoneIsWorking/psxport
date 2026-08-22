// game_iface.cpp — polymorphic game runtime installation plus the bounded legacy adapter.
#include "game_iface.h"

#include <memory>

namespace {
GameRuntime *g_runtime = nullptr;
std::unique_ptr<LegacyGameRuntimeAdapter> g_ownedLegacyRuntime;

} // namespace

LegacyGameRuntimeAdapter::LegacyGameRuntimeAdapter(const GameConfig &config, const GameHooks &hooks) {
  bindLegacyInterface(&config, &hooks);
}

void *LegacyGameRuntimeAdapter::createContext(Core &core) {
  const GameHooks *hooks = legacyHooks();
  return hooks->ctxCreate ? hooks->ctxCreate(&core) : nullptr;
}

void LegacyGameRuntimeAdapter::destroyContext(void *context) {
  const GameHooks *hooks = legacyHooks();
  if (hooks->ctxDestroy) {
    hooks->ctxDestroy(context);
  }
}

void LegacyGameRuntimeAdapter::registerOverrides(Game &game) {
  legacyHooks()->registerOverrides(&game);
}

void LegacyGameRuntimeAdapter::bootInit(Core &core) {
  legacyHooks()->bootInit(&core);
}

void psxport_install_game(GameRuntime &runtime) {
  g_runtime = &runtime;
}

GameRuntime *psxport_game_runtime() {
  return g_runtime;
}

void psxport_install_game(const GameConfig *cfg, const GameHooks *hooks) {
  if (!cfg || !hooks) {
    g_ownedLegacyRuntime.reset();
    g_runtime = nullptr;
    return;
  }
  g_ownedLegacyRuntime = std::make_unique<LegacyGameRuntimeAdapter>(*cfg, *hooks);
  psxport_install_game(*g_ownedLegacyRuntime);
}

const GameConfig *psxport_game_config() {
  return g_runtime ? g_runtime->legacyConfigForMigration() : nullptr;
}

const GameHooks *psxport_game_hooks() {
  return g_runtime ? g_runtime->legacyHooksForMigration() : nullptr;
}
