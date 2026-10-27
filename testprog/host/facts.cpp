#include "facts.h"

#include "core.h"

#include <lucent/log.h>

#include <cstdlib>

namespace psx::testprog {

Facts Facts::read(Core &core) {
  if (core.mem_r32(kFactsAddress) != kFactsMagic) {
    lucent::error("testprog", "no test program facts at 0x{:08X}", kFactsAddress);
    std::abort();
  }
  std::uint32_t at = kFactsAddress + 4u;
  const auto next = [&core, &at] {
    const std::uint32_t word = core.mem_r32(at);
    at += 4u;
    return word;
  };
  Facts facts;
  facts.vsync = next();
  facts.squareDraw = next();
  facts.orderingTables = next();
  facts.orderingTableLength = next();
  facts.cursor = next();
  facts.sineQuarter = next();
  facts.frameCounter = next();
  facts.frameFields = next();
  facts.scene = next();
  facts.requestedScene = next();
  facts.buffer = next();
  return facts;
}

} // namespace psx::testprog
