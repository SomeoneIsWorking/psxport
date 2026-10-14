// stock_libgte.h — libgte's projection setters, mirrored into the per-Core projection parameters.
#pragma once

#include <cstdint>

class Core;
class Game;

namespace psx::hle {

class StockLibgte {
public:
  struct Entries {
    std::uint32_t setGeomOffset = 0;
    std::uint32_t setGeomScreen = 0;
  };

  explicit StockLibgte(const Entries &entries) : entries_(entries) {}

  void bind(Game &game) const;
  // SetGeomOffset(x, y) and SetGeomScreen(h): guest arguments in a0-a1.
  static void setGeomOffset(Core &core);
  static void setGeomScreen(Core &core);

private:
  Entries entries_;
};

} // namespace psx::hle
