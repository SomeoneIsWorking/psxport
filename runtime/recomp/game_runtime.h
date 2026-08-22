// game_runtime.h — polymorphic framework↔game ownership seam.
//
// A game derives one GameRuntime and owns that object for the process lifetime. Game-specific
// behavior belongs in that derived class or in the per-Game products it creates. The legacy
// GameConfig/GameHooks pair remains available only through the bounded adapter declared by
// game_iface.h while existing consumers migrate; it is not the second-generation interface. A real
// consumer derives that adapter until its narrow typed fact groups have replaced every cfg read.
#pragma once

#include <cstdint>
#include <memory>

class Core;
class Game;
struct GameConfig;
struct GameHooks;

class FrameDriver {
public:
  virtual ~FrameDriver() = default;

  virtual void stepFrame(Core &core, uint32_t frame) = 0;
  virtual void autoDrive(Core &, uint32_t) {}
  virtual void frameProbes(Core &, uint32_t) {}
};

class TaskScheduler {
public:
  virtual ~TaskScheduler() = default;

  virtual void step() = 0;
  virtual void yield(Core &core) = 0;
  virtual void tickSleepCountdown() {}
};

class GameRuntime {
public:
  virtual ~GameRuntime() = default;
  GameRuntime(const GameRuntime &) = delete;
  GameRuntime &operator=(const GameRuntime &) = delete;
  GameRuntime(GameRuntime &&) = delete;
  GameRuntime &operator=(GameRuntime &&) = delete;

  virtual void *createContext(Core &core) = 0;
  virtual void destroyContext(void *context) = 0;
  virtual void registerOverrides(Game &game) = 0;
  virtual void bootInit(Core &core) = 0;

  virtual std::unique_ptr<FrameDriver> createFrameDriver(Game &) {
    return nullptr;
  }
  virtual std::unique_ptr<TaskScheduler> createTaskScheduler(Game &) {
    return nullptr;
  }

  // Non-virtual compatibility views. Direct runtimes return null; only
  // LegacyGameRuntimeAdapter binds them while a consumer migrates.
  const GameConfig *legacyConfigForMigration() const {
    return legacyConfig_;
  }
  const GameHooks *legacyHooksForMigration() const {
    return legacyHooks_;
  }

protected:
  GameRuntime() = default;

  void bindLegacyInterface(const GameConfig *config, const GameHooks *hooks) {
    legacyConfig_ = config;
    legacyHooks_ = hooks;
  }

  const GameHooks *legacyHooks() const {
    return legacyHooks_;
  }

private:
  const GameConfig *legacyConfig_ = nullptr;
  const GameHooks *legacyHooks_ = nullptr;
};

// Installs one game-owned derived runtime before constructing any Game. The caller retains
// ownership; SBS Games share the same immutable process-lifetime runtime object.
void psxport_install_game(GameRuntime &runtime);
GameRuntime *psxport_game_runtime();
