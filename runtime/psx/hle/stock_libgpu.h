// stock_libgpu.h — libgpu's waits, completed at once because the host GPU consumes work synchronously.
#pragma once

#include <cstdint>

class Core;
class Game;

namespace psx::hle {

// DrawSync and the DMA timeout pair. Retail derives the timeout from VSync(-1), which a native frame
// loop forbids, so the arm publishes a far deadline and clears the flag and the check reports none.
class StockLibgpu {
public:
  struct Entries {
    std::uint32_t drawSync = 0;
    std::uint32_t timeoutArm = 0;
    std::uint32_t timeoutCheck = 0;
    std::uint32_t timeoutDeadlineWord = 0; // zero leaves the word guest-owned
    std::uint32_t timeoutFlagWord = 0;
  };

  explicit StockLibgpu(const Entries &entries) : entries_(entries) {}

  void bind(Game &game) const &;
  // The Game keeps a pointer to it, so a temporary cannot bind.
  void bind(Game &game) const && = delete;
  // The timeout arm's body.
  void armTimeout(Core &core) const;

private:
  Entries entries_;
};

} // namespace psx::hle
