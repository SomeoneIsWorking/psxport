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

#include "guest_program_image.h"

class Core;
class Game;
class GuestWidescreenProjection;
class TemporalFramePresentation;
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

  // Immutable facts about the guest executable image. A direct runtime owns the returned value for
  // at least the lifetime of every Core. Null is an honest answer for tools/smoke clients that never
  // boot or route guest code; those algorithms refuse by name when invoked without it.
  virtual const GuestProgramImage *guestProgramImage() const {
    return nullptr;
  }

  // Optional title-owned guest projection. The returned object declares the aspect only; the title
  // must publish a matching guest projection plan before the host exposes a wider presentation span.
  virtual const GuestWidescreenProjection *guestWidescreenProjection() const {
    return nullptr;
  }

  // Whether guest VRAM is picture content for the current frame. This is deliberately a runtime
  // policy, not an immutable configuration bit: a title may use guest uploads for boot logos and
  // later hand the whole frame to native producers. The renderer asks at each present so the
  // derived title runtime can answer from its actual render mode.
  virtual bool guestVramIsPicture(const Game &game) const = 0;

  // Optional temporal decorator. Direct runtimes default to the neutral current-frame presenter and
  // therefore instantiate no interpolation history. Legacy consumers keep their existing behavior via
  // LegacyGameRuntimeAdapter until they declare the narrower contract directly.
  virtual std::unique_ptr<TemporalFramePresentation> createTemporalFramePresentation(Game &game);

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

// Checked shipping query used by every renderer path. A missing runtime is an installation defect,
// not an implicit answer about the picture, and therefore refuses instead of returning false.
bool game_guest_vram_is_picture(const Game &game);
