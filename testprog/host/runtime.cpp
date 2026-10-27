#include "runtime.h"

#include "core.h"
#include "frame_driver.h"
#include "game.h"
#include "ordering_table.h"
#include "square_producer.h"

namespace psx::testprog {

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
  SquareProducer::install(core, facts);
}

std::unique_ptr<psx::game::TitleSessionState> Runtime::createSession(Game &) {
  return std::make_unique<Session>();
}

std::unique_ptr<psx::game::FrameDriver> Runtime::createFrameDriver(Game &game) {
  return std::make_unique<FrameDriver>(game);
}

const psx::present::StateProducer *Runtime::stateProducer(Core &core, std::uint32_t producer) const {
  return session(core).producers.find(producer);
}

} // namespace psx::testprog
