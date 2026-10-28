// stock_vsync.h — libetc VSync as the native frame boundary.
#pragma once

#include <cstdint>

class Core;
class Game;

namespace psx::hle {

// VSync(0) and VSync(n > 1) note the wait's fields (Timing::noteVSyncWait) and end the guest turn at a field boundary;
// VSync(1) returns the HBlanks since the last wait without waiting.
// A negative query returns the libetc field counter at `queryCounter`; a title without one refuses the query by name.
class StockVSync {
public:
  // `entry` is VSync's guest address; `queryCounter` the libetc field counter, 0 when the title has none.
  struct Addresses {
    std::uint32_t entry = 0;
    std::uint32_t queryCounter = 0;
  };
  explicit StockVSync(Addresses addresses) : entry_(addresses.entry), queryCounter_(addresses.queryCounter) {}

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
