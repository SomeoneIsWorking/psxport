#include "runtime.h"

#include "core.h"
#include "frame_driver.h"
#include "game.h"
#include "native_dispatch.h"
#include "ordering_table.h"

namespace psx::testprog {
namespace {

// square_draw keyed by its square, so the verdict counts its packets as owned.
void squareDrawOverride(Core *core) {
  (void)psx::cpu::callOriginalOrPropagate(*core, session(*core).facts.squareDraw);
}

} // namespace

Session &session(Core &core) {
  return static_cast<Session &>(*core.game->sessionState);
}

void Runtime::registerOverrides(Game &game) {
  Core &core = game.core;
  Session &state = session(core);
  state.facts = Facts::read(core);
  const Facts &facts = state.facts;
  vsync_.emplace(psx::hle::StockVSync::Addresses{.entry = facts.vsync});
  vsync_->bind(game);
  for (std::uint32_t buffer = 0; buffer < 2u; ++buffer) {
    core.otTables.name(kOrderingTable,
                       facts.orderingTables + buffer * facts.orderingTableLength * 4u,
                       facts.orderingTableLength,
                       4u,
                       psx::gpu::OtWalk::HighToLow);
  }
  psx::cpu::installNativeOverride(core,
                                  facts.squareDraw,
                                  "testprog::squareDraw",
                                  squareDrawOverride,
                                  psx::present::Producer{psx::present::Arg::A0});
}

std::unique_ptr<psx::game::TitleSessionState> Runtime::createSession(Game &) {
  return std::make_unique<Session>();
}

std::unique_ptr<psx::game::FrameDriver> Runtime::createFrameDriver(Game &game) {
  return std::make_unique<FrameDriver>(game);
}

} // namespace psx::testprog
