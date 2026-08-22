// game_iface.h — installation boundary for consumers migrating to GameRuntime inheritance.
#pragma once

#ifdef __cplusplus

#include "game_runtime.h"
#include "legacy_game_config.h"
#include "legacy_game_hooks.h"

// Temporary inheritance bridge for incremental consumer migration. A game may derive its runtime
// from this class, override cohesive behavior directly, and leave unmoved behavior/config facts on
// the legacy pair. Do not add members here: its death condition is every consumer deriving
// GameRuntime directly after narrow immutable fact groups replace the remaining GameConfig reads.
class LegacyGameRuntimeAdapter : public GameRuntime {
public:
  LegacyGameRuntimeAdapter(const GameConfig &config, const GameHooks &hooks);

  void *createContext(Core &core) override;
  void destroyContext(void *context) override;
  void registerOverrides(Game &game) override;
  void bootInit(Core &core) override;
};

// Legacy adapter install. New code calls psxport_install_game(GameRuntime&) from game_runtime.h.
void psxport_install_game(const GameConfig *cfg, const GameHooks *hooks);
const GameConfig *psxport_game_config();
const GameHooks *psxport_game_hooks();

#endif // __cplusplus
