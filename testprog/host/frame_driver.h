// frame_driver.h — the test program's logic frame: guest fields until it has drawn a new frame, then one commit.
#pragma once

#include "game_runtime.h"

#include <cstdint>

class Core;
class Game;

namespace psx::testprog {

// Each guest VSync ends a turn at a field boundary; the first boundary after the frame counter moves commits the
// frame it drew, for the fields the guest says it will wait.
class FrameDriver final : public psx::game::FrameDriver {
public:
  explicit FrameDriver(Game &game) : game_(game) {}

  void stepFrame(Core &core, std::uint32_t frame) override;

private:
  // More fields than any planted frame waits, so a guest that stops drawing ends the run.
  static constexpr int kMaxFieldsPerFrame = 8;

  Game &game_;
  std::uint32_t committed_ = 0;
};

} // namespace psx::testprog
