// stock_vsync.h — libetc VSync as the native frame boundary.
#pragma once

#include <cstdint>

class Core;
class Game;

namespace psx::hle {

// A nonnegative VSync ends the guest turn at a field boundary. A negative query returns the libetc
// field counter at `queryCounter`; a title without one refuses the query by name.
class StockVSync {
public:
  StockVSync(std::uint32_t entry, std::uint32_t queryCounter) : entry_(entry), queryCounter_(queryCounter) {}

  void bind(Game &game) const &;
  // The Game keeps a pointer to it, so a temporary cannot bind.
  void bind(Game &game) const && = delete;
  // The VSync body: guest argument in a0, result in v0.
  void serve(Core &core) const;

  std::uint32_t entry() const {
    return entry_;
  }
  std::uint32_t queryCounter() const {
    return queryCounter_;
  }

private:
  std::uint32_t entry_;
  std::uint32_t queryCounter_;
};

} // namespace psx::hle
