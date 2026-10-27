// runtime.h — the test program as a title: its session, its VSync, its producer and its debug menu.
#pragma once

#include "debug_menu.h"
#include "facts.h"
#include "game_runtime.h"
#include "state_producer.h"
#include "stock_vsync.h"

#include <optional>

namespace psx::testprog {

class Session final : public psx::game::TitleSessionState {
public:
  Facts facts;
  psx::present::StateProducerTable producers;
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
  const psx::present::StateProducer *stateProducer(Core &core, std::uint32_t producer) const override;
  const psx::game::TitleDebugMenu &debugMenu() const override {
    return debugMenu_;
  }

private:
  // Bound once the image names its VSync.
  std::optional<psx::hle::StockVSync> vsync_;
  DebugMenu debugMenu_;
};

} // namespace psx::testprog
