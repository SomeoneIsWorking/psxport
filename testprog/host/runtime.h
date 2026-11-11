// runtime.h — the test program as a title: its session, its VSync, its square's owner and its debug menu.
#pragma once

#include "debug_menu.h"
#include "facts.h"
#include "game_runtime.h"
#include "stock_vsync.h"

#include <optional>

namespace psx::testprog {

class Session final : public psx::game::TitleSessionState {
public:
  Facts facts;
};

Session &session(Core &core);

class Runtime final : public psx::game::GameRuntime {
public:
  void registerOverrides(Game &game) override;
  std::unique_ptr<psx::game::TitleSessionState> createSession(Game &game) override;
  std::unique_ptr<psx::game::FrameDriver> createFrameDriver(Game &game) override;
  bool interpolates() const override {
    return true;
  }
  const psx::game::TitleDebugMenu &debugMenu() const override {
    return debugMenu_;
  }

private:
  // Bound once the image names its VSync.
  std::optional<psx::hle::StockVSync> vsync_;
  DebugMenu debugMenu_;
};

} // namespace psx::testprog
