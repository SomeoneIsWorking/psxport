// facts.h — the guest addresses the test program publishes in its own image, read once it is loaded.
#pragma once

#include <cstdint>

class Core;

namespace psx::testprog {

// testprog.c places the table right after its entry's three instructions.
inline constexpr std::uint32_t kFactsAddress = 0x8001000Cu;
inline constexpr std::uint32_t kFactsMagic = 0x54505350u;
inline constexpr std::uint16_t kOrderingTable = 0;
// The bucket square_draw links its packet into.
inline constexpr std::uint32_t kSquareBucket = 4;

struct Facts {
  std::uint32_t vsync = 0;
  std::uint32_t squareDraw = 0;
  std::uint32_t orderingTables = 0; // two tables of `orderingTableLength` words, one per display buffer
  std::uint32_t orderingTableLength = 0;
  std::uint32_t cursor = 0;
  std::uint32_t sineQuarter = 0; // 17 Q12 halfwords
  std::uint32_t frameCounter = 0;
  std::uint32_t frameFields = 0;
  std::uint32_t scene = 0;
  std::uint32_t requestedScene = 0;
  std::uint32_t buffer = 0;

  // The table in `core`'s RAM; aborts when the loaded image is not the test program.
  static Facts read(Core &core);
};

} // namespace psx::testprog
